#include "xtd_test_support.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <sodium.h>
#include <zstd.h>

#include "../src/xtd_crypto_internal.h"

namespace environmental_grib::test {
namespace {

using Bytes = std::vector<unsigned char>;

constexpr std::array<unsigned char, 8> kMagic{'X', 'G', 'R', 'I',
                                              'B', 'X', '1', 0};
constexpr std::size_t kHeaderSize = 512;
constexpr std::size_t kIndexEntrySize = 64;

template <typename T>
void PutLe(Bytes* output, std::size_t offset, T value) {
  static_assert(std::is_integral_v<T>);
  using U = std::make_unsigned_t<T>;
  const U raw = static_cast<U>(value);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    (*output)[offset + i] = static_cast<unsigned char>(raw >> (i * 8));
  }
}

void PutF32(Bytes* output, std::size_t offset, float value) {
  PutLe(output, offset, std::bit_cast<std::uint32_t>(value));
}

void PutF64(Bytes* output, std::size_t offset, double value) {
  PutLe(output, offset, std::bit_cast<std::uint64_t>(value));
}

void Append(Bytes* output, const Bytes& value) {
  output->insert(output->end(), value.begin(), value.end());
}

bool IsEmpty(const XtdFixtureOptions& options, std::uint32_t tile_id) {
  return std::find(options.empty_tiles.begin(), options.empty_tiles.end(),
                   tile_id) != options.empty_tiles.end();
}

double FixtureValue(const XtdFixtureOptions& options, std::size_t constituent,
                    std::size_t component, std::uint32_t x, std::uint32_t y) {
  if (options.value) return options.value(constituent, component, x, y);
  const int quantized = static_cast<int>((constituent + 1) * 300 +
                                         component * 200 + x * 7 + y * 11);
  return quantized * static_cast<double>(options.quantization_scale);
}

bool FixtureValid(const XtdFixtureOptions& options, bool u_component,
                  std::uint32_t x, std::uint32_t y) {
  return !options.valid || options.valid(u_component, x, y);
}

Bytes MakePlaintext(const XtdFixtureOptions& options, std::uint32_t tx,
                    std::uint32_t ty, std::uint16_t width,
                    std::uint16_t height) {
  const std::uint32_t cells = static_cast<std::uint32_t>(width) * height;
  const std::uint32_t mask_bytes = (cells + 7) / 8;
  const std::uint32_t scale_count =
      static_cast<std::uint32_t>(options.constituents.size()) * 4;
  const std::uint64_t coefficient_count =
      static_cast<std::uint64_t>(scale_count) * cells;
  if (options.coefficient_bits != 12 && options.coefficient_bits != 16) {
    throw std::runtime_error("XTD fixture coefficient bits must be 12 or 16");
  }
  const std::uint64_t coefficient_bytes =
      (coefficient_count * options.coefficient_bits + 7) / 8;
  const std::uint64_t size =
      40ULL + mask_bytes * 2ULL + scale_count * 4ULL + coefficient_bytes;
  if (size > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("XTD fixture tile is too large");
  }
  Bytes result(static_cast<std::size_t>(size));
  std::copy_n("XTP1", 4, result.begin());
  PutLe(&result, 4, static_cast<std::uint16_t>(1));
  PutLe(&result, 6, static_cast<std::uint16_t>(options.constituents.size()));
  PutLe(&result, 8, width);
  PutLe(&result, 10, height);
  PutLe(&result, 12, cells);
  PutLe(&result, 16, mask_bytes);
  PutLe(&result, 20, mask_bytes);
  PutLe(&result, 24, scale_count);
  PutLe(&result, 28, coefficient_count);
  PutLe(&result, 36, static_cast<std::uint32_t>(0));

  std::size_t cursor = 40;
  for (std::uint32_t local_y = 0; local_y < height; ++local_y) {
    for (std::uint32_t local_x = 0; local_x < width; ++local_x) {
      const std::uint32_t cell = local_y * width + local_x;
      const std::uint32_t x = tx * options.tile_width + local_x;
      const std::uint32_t y = ty * options.tile_height + local_y;
      if (FixtureValid(options, true, x, y)) {
        result[cursor + cell / 8] |=
            static_cast<unsigned char>(1U << (cell % 8));
      }
      if (FixtureValid(options, false, x, y)) {
        result[cursor + mask_bytes + cell / 8] |=
            static_cast<unsigned char>(1U << (cell % 8));
      }
    }
  }
  cursor += mask_bytes * 2;
  for (std::uint32_t i = 0; i < scale_count; ++i) {
    PutF32(&result, cursor, options.quantization_scale);
    cursor += 4;
  }
  std::vector<std::int16_t> coefficients;
  coefficients.reserve(static_cast<std::size_t>(coefficient_count));
  const long minimum_quantized =
      options.coefficient_bits == 12 ? -2048 : -32768;
  const long maximum_quantized = options.coefficient_bits == 12 ? 2047 : 32767;
  for (std::size_t constituent = 0; constituent < options.constituents.size();
       ++constituent) {
    for (std::size_t component = 0; component < 4; ++component) {
      for (std::uint32_t local_y = 0; local_y < height; ++local_y) {
        for (std::uint32_t local_x = 0; local_x < width; ++local_x) {
          const std::uint32_t x = tx * options.tile_width + local_x;
          const std::uint32_t y = ty * options.tile_height + local_y;
          const double value =
              FixtureValue(options, constituent, component, x, y);
          const long quantized =
              std::lround(value / options.quantization_scale);
          if (!std::isfinite(value) || quantized < minimum_quantized ||
              quantized > maximum_quantized) {
            throw std::runtime_error(
                "XTD fixture value cannot be represented at the requested "
                "coefficient precision");
          }
          coefficients.push_back(static_cast<std::int16_t>(quantized));
        }
      }
    }
  }
  const std::uint16_t modulus_mask =
      options.coefficient_bits == 12 ? 0x0fffU : 0xffffU;
  for (std::uint32_t component = 0; component < scale_count; ++component) {
    const std::size_t base = static_cast<std::size_t>(component) * cells;
    for (std::uint32_t y = height; y-- > 1;) {
      for (std::uint32_t x = 0; x < width; ++x) {
        const std::size_t cell = base + y * width + x;
        const auto value = static_cast<std::uint16_t>(coefficients[cell]);
        const auto above =
            static_cast<std::uint16_t>(coefficients[cell - width]);
        const auto residual =
            static_cast<std::uint16_t>(value - above) & modulus_mask;
        coefficients[cell] = static_cast<std::int16_t>(residual);
      }
    }
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = width; x-- > 1;) {
        const std::size_t cell = base + y * width + x;
        const auto value = static_cast<std::uint16_t>(coefficients[cell]);
        const auto left = static_cast<std::uint16_t>(coefficients[cell - 1]);
        const auto residual =
            static_cast<std::uint16_t>(value - left) & modulus_mask;
        coefficients[cell] = static_cast<std::int16_t>(residual);
      }
    }
  }
  if (options.coefficient_bits == 16) {
    for (const auto coefficient : coefficients) {
      PutLe(&result, cursor, coefficient);
      cursor += 2;
    }
    PutLe(&result, 36, static_cast<std::uint32_t>(1));
  } else {
    for (std::size_t i = 0; i < coefficients.size(); i += 2) {
      const auto first = static_cast<std::uint16_t>(coefficients[i]) & 0x0fffU;
      const auto second =
          static_cast<std::uint16_t>(coefficients[i + 1]) & 0x0fffU;
      result[cursor++] = static_cast<unsigned char>(first & 0xffU);
      result[cursor++] = static_cast<unsigned char>(((first >> 8) & 0x0fU) |
                                                    ((second & 0x0fU) << 4));
      result[cursor++] = static_cast<unsigned char>(second >> 4);
    }
    PutLe(&result, 36, static_cast<std::uint32_t>(2));
  }
  if (cursor != result.size()) {
    throw std::runtime_error("XTD fixture tile size calculation is wrong");
  }
  return result;
}

Bytes Compress(const Bytes& plaintext, int level) {
  Bytes compressed(ZSTD_compressBound(plaintext.size()));
  const std::size_t size =
      ZSTD_compress(compressed.data(), compressed.size(), plaintext.data(),
                    plaintext.size(), level);
  if (ZSTD_isError(size)) {
    throw std::runtime_error(std::string("could not compress XTD fixture: ") +
                             ZSTD_getErrorName(size));
  }
  compressed.resize(size);
  return compressed;
}

std::array<unsigned char, 24> TileNonce(std::uint32_t tile_id) {
  std::array<unsigned char, 24> nonce{};
  for (std::size_t i = 0; i < nonce.size(); ++i) {
    nonce[i] = static_cast<unsigned char>(0x31 + i * 13 + tile_id * 17);
  }
  return nonce;
}

struct PendingTile {
  bool empty{};
  std::uint32_t tile_id{}, tx{}, ty{};
  std::uint16_t width{}, height{};
  Bytes plaintext;
  Bytes compressed;
  Bytes encrypted;
  std::array<unsigned char, 24> nonce{};
};

struct SecureKey {
  std::array<unsigned char, 32> bytes{};
  ~SecureKey() { sodium_memzero(bytes.data(), bytes.size()); }
};

}  // namespace

void WriteXtdFixture(const std::filesystem::path& path,
                     const XtdFixtureOptions& options) {
  xtd_internal::EnsureSodium();
  if (options.nx < 2 || options.ny < 2 || options.nx > 20'000 ||
      options.ny > 10'000 || options.tile_width == 0 ||
      options.tile_width > 256 || options.tile_height == 0 ||
      options.tile_height > 256 || options.constituents.empty() ||
      options.constituents.size() > 64 ||
      !std::isfinite(options.quantization_scale) ||
      options.quantization_scale <= 0.0F) {
    throw std::runtime_error("invalid XTD fixture options");
  }

  const std::uint32_t tile_columns =
      (options.nx + options.tile_width - 1) / options.tile_width;
  const std::uint32_t tile_rows =
      (options.ny + options.tile_height - 1) / options.tile_height;
  const std::uint32_t tile_count = tile_columns * tile_rows;
  std::vector<PendingTile> tiles;
  tiles.reserve(tile_count);
  for (std::uint32_t tile_id = 0; tile_id < tile_count; ++tile_id) {
    const std::uint32_t tx = tile_id % tile_columns;
    const std::uint32_t ty = tile_id / tile_columns;
    const auto width = static_cast<std::uint16_t>(
        std::min(options.tile_width, options.nx - tx * options.tile_width));
    const auto height = static_cast<std::uint16_t>(
        std::min(options.tile_height, options.ny - ty * options.tile_height));
    PendingTile tile;
    tile.empty = IsEmpty(options, tile_id);
    tile.tile_id = tile_id;
    tile.tx = tx;
    tile.ty = ty;
    tile.width = width;
    tile.height = height;
    if (!tile.empty) {
      tile.plaintext = MakePlaintext(options, tx, ty, width, height);
      tile.compressed = Compress(tile.plaintext, options.zstd_level);
      tile.nonce = TileNonce(tile_id);
    }
    tiles.push_back(std::move(tile));
  }

  Json::Value metadata = options.metadata;
  if (!metadata.isObject()) metadata = Json::Value(Json::objectValue);
  metadata["format"] = "xgrib-xtd";
  metadata["format_version"] = 1;
  metadata["model_name"] = metadata.get("model_name", "synthetic-test");
  metadata["velocity_units"] = "cm/s";
  metadata["corrections"] = "ATLAS";
  metadata["constituents"] = Json::Value(Json::arrayValue);
  for (const auto& constituent : options.constituents) {
    metadata["constituents"].append(constituent);
  }
  Json::StreamWriterBuilder json_builder;
  json_builder["indentation"] = "";
  const std::string json = Json::writeString(json_builder, metadata);
  const Bytes metadata_bytes(json.begin(), json.end());

  const std::uint64_t metadata_offset = kHeaderSize;
  const std::uint64_t index_offset = metadata_offset + metadata_bytes.size();
  const std::uint64_t payload_offset =
      index_offset + static_cast<std::uint64_t>(tile_count) * kIndexEntrySize;
  Bytes index(static_cast<std::size_t>(tile_count) * kIndexEntrySize);
  std::uint64_t payload_cursor = payload_offset;
  for (const auto& tile : tiles) {
    const std::size_t cursor = tile.tile_id * kIndexEntrySize;
    PutLe(&index, cursor, tile.tile_id);
    PutLe(&index, cursor + 4, tile.tx);
    PutLe(&index, cursor + 8, tile.ty);
    PutLe(&index, cursor + 12, tile.width);
    PutLe(&index, cursor + 14, tile.height);
    if (tile.empty) {
      PutLe(&index, cursor + 16, static_cast<std::uint32_t>(1));
      continue;
    }
    PutLe(&index, cursor + 20, static_cast<std::uint32_t>(1));
    PutLe(&index, cursor + 24, payload_cursor);
    PutLe(
        &index, cursor + 32,
        static_cast<std::uint32_t>(tile.compressed.size() +
                                   crypto_aead_xchacha20poly1305_ietf_ABYTES));
    PutLe(&index, cursor + 36,
          static_cast<std::uint32_t>(tile.plaintext.size()));
    std::copy(tile.nonce.begin(), tile.nonce.end(),
              index.begin() + cursor + 40);
    payload_cursor +=
        tile.compressed.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES;
  }

  Bytes header(kHeaderSize);
  std::copy(kMagic.begin(), kMagic.end(), header.begin());
  PutLe(&header, 8, static_cast<std::uint32_t>(1));
  PutLe(&header, 12, static_cast<std::uint32_t>(kHeaderSize));
  PutLe(&header, 16, static_cast<std::uint32_t>(0x01020304));
  PutLe(&header, 24, options.nx);
  PutLe(&header, 28, options.ny);
  PutLe(&header, 32, options.tile_width);
  PutLe(&header, 36, options.tile_height);
  PutLe(&header, 40, tile_columns);
  PutLe(&header, 44, tile_rows);
  PutLe(&header, 48, static_cast<std::uint32_t>(options.constituents.size()));
  PutLe(&header, 52, static_cast<std::uint32_t>(kIndexEntrySize));
  PutLe(&header, 56, metadata_offset);
  PutLe(&header, 64, static_cast<std::uint64_t>(metadata_bytes.size()));
  PutLe(&header, 72, index_offset);
  PutLe(&header, 80, static_cast<std::uint64_t>(tile_count));
  PutLe(&header, 88, payload_offset);
  PutF64(&header, 96, options.lon_u0);
  PutF64(&header, 104, options.lon_step);
  PutF64(&header, 112, options.lat_u0);
  PutF64(&header, 120, options.lat_step);
  PutF64(&header, 128, options.lon_v0);
  PutF64(&header, 136, options.lat_v0);
  PutF64(&header, 144, options.west);
  PutF64(&header, 152, options.south);
  PutF64(&header, 160, options.east);
  PutF64(&header, 168, options.north);
  for (std::size_t i = 0; i < 16; ++i) {
    header[176 + i] = static_cast<unsigned char>(0x42 + i * 9);
  }
  for (std::size_t i = 0; i < 24; ++i) {
    header[192 + i] = static_cast<unsigned char>(0x91 + i * 7);
  }
  PutLe(&header, 264, payload_cursor);

  SecureKey content;
  for (std::size_t i = 0; i < content.bytes.size(); ++i) {
    content.bytes[i] = static_cast<unsigned char>(0xa7 + i * 11);
  }
  SecureKey root{xtd_internal::RuntimeRootKey()};
  std::array<unsigned char, 28> wrapper_aad{};
  std::copy(kMagic.begin(), kMagic.end(), wrapper_aad.begin());
  wrapper_aad[8] = 1;
  std::copy_n(header.begin() + 176, 16, wrapper_aad.begin() + 12);
  unsigned long long wrapped_length = 0;
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(
          header.data() + 216, &wrapped_length, content.bytes.data(),
          content.bytes.size(), wrapper_aad.data(), wrapper_aad.size(), nullptr,
          header.data() + 192, root.bytes.data()) != 0 ||
      wrapped_length != 48) {
    throw std::runtime_error("could not wrap XTD fixture content key");
  }

  Bytes payload;
  payload.reserve(static_cast<std::size_t>(payload_cursor - payload_offset));
  for (auto& tile : tiles) {
    if (tile.empty) continue;
    std::array<unsigned char, 56> aad{};
    std::copy_n(header.begin() + 176, 16, aad.begin());
    std::copy_n(index.begin() + tile.tile_id * kIndexEntrySize, 40,
                aad.begin() + 16);
    tile.encrypted.resize(tile.compressed.size() +
                          crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long encrypted_length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            tile.encrypted.data(), &encrypted_length, tile.compressed.data(),
            tile.compressed.size(), aad.data(), aad.size(), nullptr,
            tile.nonce.data(), content.bytes.data()) != 0 ||
        encrypted_length != tile.encrypted.size()) {
      throw std::runtime_error("could not encrypt XTD fixture tile");
    }
    Append(&payload, tile.encrypted);
  }

  crypto_generichash_state mac_state;
  std::array<unsigned char, crypto_generichash_BYTES> mac{};
  if (crypto_generichash_init(&mac_state, content.bytes.data(),
                              content.bytes.size(), mac.size()) != 0 ||
      crypto_generichash_update(&mac_state, header.data(), header.size()) !=
          0 ||
      crypto_generichash_update(&mac_state, metadata_bytes.data(),
                                metadata_bytes.size()) != 0 ||
      crypto_generichash_update(&mac_state, index.data(), index.size()) != 0 ||
      crypto_generichash_final(&mac_state, mac.data(), mac.size()) != 0) {
    throw std::runtime_error(
        "could not authenticate XTD fixture public region");
  }
  std::copy(mac.begin(), mac.end(), header.begin() + 272);
  sodium_memzero(mac.data(), mac.size());

  const auto directory = path.parent_path();
  if (!directory.empty()) std::filesystem::create_directories(directory);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("could not create XTD fixture");
  output.write(reinterpret_cast<const char*>(header.data()), header.size());
  output.write(reinterpret_cast<const char*>(metadata_bytes.data()),
               static_cast<std::streamsize>(metadata_bytes.size()));
  output.write(reinterpret_cast<const char*>(index.data()),
               static_cast<std::streamsize>(index.size()));
  output.write(reinterpret_cast<const char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
  if (!output) throw std::runtime_error("could not write XTD fixture");
}

namespace {

constexpr std::array<unsigned char, 8> kV2Magic{'X', 'G', 'R', 'I',
                                                'B', 'X', '2', 0};
constexpr std::size_t kV2ComponentEntrySize = 256;

Bytes ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("could not read fixture file");
  const auto length = input.tellg();
  if (length < 0) throw std::runtime_error("could not size fixture file");
  Bytes result(static_cast<std::size_t>(length));
  input.seekg(0);
  if (!result.empty() &&
      !input.read(reinterpret_cast<char*>(result.data()), length)) {
    throw std::runtime_error("could not read fixture bytes");
  }
  return result;
}

std::array<unsigned char, 32> Sha256(const Bytes& bytes) {
  std::array<unsigned char, 32> result{};
  crypto_hash_sha256(result.data(), bytes.data(), bytes.size());
  return result;
}

std::string HexDigest(const std::array<unsigned char, 32>& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result(digest.size() * 2, '0');
  for (std::size_t i = 0; i < digest.size(); ++i) {
    result[2 * i] = kHex[digest[i] >> 4];
    result[2 * i + 1] = kHex[digest[i] & 15];
  }
  return result;
}

std::array<unsigned char, 32> DeriveV2ComponentKey(
    const std::array<unsigned char, 32>& package_key,
    const std::array<unsigned char, 16>& component_id) {
  constexpr std::array<unsigned char, 12> domain{'X', 'T', 'D', '2', '-', 'C',
                                                 'O', 'M', 'P', '-', 'v', '1'};
  std::array<unsigned char, 32> result{};
  crypto_generichash_state state;
  if (crypto_generichash_init(&state, package_key.data(), package_key.size(),
                              result.size()) != 0 ||
      crypto_generichash_update(&state, domain.data(), domain.size()) != 0 ||
      crypto_generichash_update(&state, component_id.data(),
                                component_id.size()) != 0 ||
      crypto_generichash_final(&state, result.data(), result.size()) != 0) {
    throw std::runtime_error("could not derive XTD v2 fixture component key");
  }
  return result;
}

Bytes MakeV2TilePlaintext(const XtdV2FixtureOptions& options, std::uint32_t tx,
                          std::uint32_t ty, std::uint16_t width,
                          std::uint16_t height, bool uncertainty) {
  const std::uint32_t cells = static_cast<std::uint32_t>(width) * height;
  const std::uint32_t mask_bytes = (cells + 7) / 8;
  const std::uint32_t fields =
      uncertainty                                                         ? 4
      : options.representation == XtdV2ResidualRepresentation::kHarmonic2 ? 10
                                                                          : 24;
  const std::uint32_t quality_bytes = uncertainty ? cells * 7 : 0;
  const std::uint32_t total =
      32 + mask_bytes + fields * 4 + fields * cells * 2 + quality_bytes;
  Bytes result(total);
  std::copy_n(uncertainty ? "XCU1" : "XCR1", 4, result.begin());
  PutLe(&result, 4, static_cast<std::uint16_t>(1));
  PutLe(&result, 6,
        static_cast<std::uint16_t>(
            uncertainty ? 0
            : options.representation == XtdV2ResidualRepresentation::kHarmonic2
                ? 2
                : 3));
  PutLe(&result, 8, width);
  PutLe(&result, 10, height);
  PutLe(&result, 12, static_cast<std::uint16_t>(fields));
  PutLe(&result, 16, cells);
  PutLe(&result, 20, mask_bytes);
  PutLe(&result, 24, static_cast<std::uint32_t>(0));
  PutLe(&result, 28, total);

  std::size_t cursor = 32;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto cell = y * width + x;
      const auto global_x = tx * options.tile_width + x;
      const auto global_y = ty * options.tile_height + y;
      if (!options.valid || options.valid(global_x, global_y)) {
        result[cursor + cell / 8] |=
            static_cast<unsigned char>(1U << (cell % 8));
      }
    }
  }
  cursor += mask_bytes;
  for (std::uint32_t field = 0; field < fields; ++field) {
    PutF32(&result, cursor, options.quantization_scale);
    cursor += 4;
  }
  for (std::uint32_t field = 0; field < fields; ++field) {
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = 0; x < width; ++x) {
        const auto global_x = tx * options.tile_width + x;
        const auto global_y = ty * options.tile_height + y;
        double value;
        if (uncertainty) {
          constexpr std::array<double, 4> values{0.12, 0.16, 0.004, 0.03};
          value = values[field];
        } else if (options.residual_value) {
          value = options.residual_value(field, global_x, global_y);
        } else if (options.representation ==
                   XtdV2ResidualRepresentation::kHarmonic2) {
          value = field == 0   ? 0.20 + global_x * 0.001
                  : field == 5 ? -0.10 + global_y * 0.001
                               : 0.01 * (field + 1);
        } else {
          value = field < 12 ? 0.01 * (field + 1) : -0.01 * (field - 11);
        }
        const long quantized = std::lround(value / options.quantization_scale);
        if (quantized < std::numeric_limits<std::int16_t>::min() ||
            quantized > std::numeric_limits<std::int16_t>::max()) {
          throw std::runtime_error("XTD v2 fixture value is out of range");
        }
        PutLe(&result, cursor, static_cast<std::int16_t>(quantized));
        cursor += 2;
      }
    }
  }
  if (uncertainty) {
    for (std::uint32_t i = 0; i < cells; ++i) result[cursor++] = 20;
    for (std::uint32_t i = 0; i < cells; ++i) result[cursor++] = 250;
    for (std::uint32_t i = 0; i < cells; ++i) result[cursor++] = 12;
    for (std::uint32_t i = 0; i < cells; ++i) result[cursor++] = 80;
    for (std::uint32_t i = 0; i < cells; ++i) result[cursor++] = 30;
    for (std::uint32_t i = 0; i < cells; ++i) {
      PutLe(&result, cursor, static_cast<std::uint16_t>(0));
      cursor += 2;
    }
  }
  if (cursor != result.size()) {
    throw std::runtime_error("XTD v2 fixture tile length is inconsistent");
  }
  return result;
}

struct V2Tile {
  std::uint32_t id{}, tx{}, ty{};
  std::uint16_t width{}, height{};
  Bytes plaintext;
  Bytes compressed;
  Bytes encrypted;
  std::array<unsigned char, 24> nonce{};
};

struct V2Component {
  std::uint32_t type{}, representation{};
  std::array<unsigned char, 16> id{};
  Bytes metadata;
  Bytes index;
  Bytes payload;
  std::array<unsigned char, 32> logical_hash{};
  std::array<unsigned char, 32> stored_hash{};
  std::uint64_t metadata_offset{}, index_offset{}, payload_offset{};
};

V2Component MakeV2TiledComponent(
    const XtdV2FixtureOptions& options, std::uint32_t type,
    std::uint32_t representation,
    const std::array<unsigned char, 16>& package_id,
    const std::array<unsigned char, 32>& package_key,
    std::uint64_t metadata_offset) {
  V2Component component;
  component.type = type;
  component.representation = representation;
  for (std::size_t i = 0; i < component.id.size(); ++i) {
    component.id[i] = static_cast<unsigned char>(type * 31 + i * 7);
  }
  Json::Value metadata(Json::objectValue);
  metadata["component"] =
      type == 2 ? "climatological_residual" : "climatological_uncertainty";
  metadata["representation"] = type == 3             ? "uncertainty_v1"
                               : representation == 2 ? "harmonic2"
                                                     : "monthly12";
  metadata["velocity_units"] = "m/s";
  auto& grid = metadata["grid"];
  grid["nx"] = options.tide.nx;
  grid["ny"] = options.tide.ny;
  grid["lon0"] = options.tide.lon_u0;
  grid["lat0"] = options.tide.lat_u0;
  grid["lon_step"] = options.tide.lon_step;
  grid["lat_step"] = options.tide.lat_step;
  grid["tile_width"] = options.tile_width;
  grid["tile_height"] = options.tile_height;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const auto json = Json::writeString(writer, metadata);
  component.metadata.assign(json.begin(), json.end());
  component.metadata_offset = metadata_offset;

  const std::uint32_t columns =
      (options.tide.nx + options.tile_width - 1) / options.tile_width;
  const std::uint32_t rows =
      (options.tide.ny + options.tile_height - 1) / options.tile_height;
  const std::uint32_t tile_count = columns * rows;
  component.index_offset = metadata_offset + component.metadata.size();
  component.index.resize(static_cast<std::size_t>(tile_count) * 64);
  component.payload_offset = component.index_offset + component.index.size();
  std::vector<V2Tile> tiles;
  tiles.reserve(tile_count);
  std::uint64_t payload_cursor = component.payload_offset;
  for (std::uint32_t id = 0; id < tile_count; ++id) {
    V2Tile tile;
    tile.id = id;
    tile.tx = id % columns;
    tile.ty = id / columns;
    tile.width = static_cast<std::uint16_t>(std::min(
        options.tile_width, options.tide.nx - tile.tx * options.tile_width));
    tile.height = static_cast<std::uint16_t>(std::min(
        options.tile_height, options.tide.ny - tile.ty * options.tile_height));
    tile.plaintext = MakeV2TilePlaintext(options, tile.tx, tile.ty, tile.width,
                                         tile.height, type == 3);
    tile.compressed = Compress(tile.plaintext, 3);
    for (std::size_t i = 0; i < tile.nonce.size(); ++i) {
      tile.nonce[i] = static_cast<unsigned char>(type * 41 + id * 13 + i * 3);
    }
    const auto position = static_cast<std::size_t>(id) * 64;
    PutLe(&component.index, position, id);
    PutLe(&component.index, position + 4, tile.tx);
    PutLe(&component.index, position + 8, tile.ty);
    PutLe(&component.index, position + 12, tile.width);
    PutLe(&component.index, position + 14, tile.height);
    PutLe(&component.index, position + 20, static_cast<std::uint32_t>(1));
    PutLe(&component.index, position + 24, payload_cursor);
    PutLe(&component.index, position + 32,
          static_cast<std::uint32_t>(tile.compressed.size() + 16));
    PutLe(&component.index, position + 36,
          static_cast<std::uint32_t>(tile.plaintext.size()));
    std::copy(tile.nonce.begin(), tile.nonce.end(),
              component.index.begin() + position + 40);
    payload_cursor += tile.compressed.size() + 16;
    tiles.push_back(std::move(tile));
  }

  const auto component_key = DeriveV2ComponentKey(package_key, component.id);
  crypto_hash_sha256_state logical_state;
  crypto_hash_sha256_init(&logical_state);
  for (auto& tile : tiles) {
    crypto_hash_sha256_update(&logical_state, tile.plaintext.data(),
                              tile.plaintext.size());
    const auto position = static_cast<std::size_t>(tile.id) * 64;
    std::array<unsigned char, 81> aad{};
    std::copy_n("XTD2-TILE", 9, aad.begin());
    std::copy(package_id.begin(), package_id.end(), aad.begin() + 9);
    std::copy(component.id.begin(), component.id.end(), aad.begin() + 25);
    std::copy_n(component.index.begin() + position, 40, aad.begin() + 41);
    tile.encrypted.resize(tile.compressed.size() + 16);
    unsigned long long encrypted_length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            tile.encrypted.data(), &encrypted_length, tile.compressed.data(),
            tile.compressed.size(), aad.data(), aad.size(), nullptr,
            tile.nonce.data(), component_key.data()) != 0 ||
        encrypted_length != tile.encrypted.size()) {
      throw std::runtime_error("could not encrypt XTD v2 fixture tile");
    }
    Append(&component.payload, tile.encrypted);
  }
  crypto_hash_sha256_final(&logical_state, component.logical_hash.data());
  Bytes stored;
  Append(&stored, component.metadata);
  Append(&stored, component.index);
  Append(&stored, component.payload);
  component.stored_hash = Sha256(stored);
  return component;
}

}  // namespace

void WriteXtdV2Fixture(const std::filesystem::path& path,
                       const XtdV2FixtureOptions& options) {
  xtd_internal::EnsureSodium();
  const auto nested_path = path.string() + ".nested-v1";
  WriteXtdFixture(nested_path, options.tide);
  const auto nested = ReadFile(nested_path);
  std::filesystem::remove(nested_path);
  const auto tide_hash = Sha256(nested);

  std::array<unsigned char, 16> package_id{};
  std::array<unsigned char, 32> package_key{};
  std::array<unsigned char, 24> wrap_nonce{};
  for (std::size_t i = 0; i < package_id.size(); ++i)
    package_id[i] = static_cast<unsigned char>(0x20 + i * 5);
  for (std::size_t i = 0; i < package_key.size(); ++i)
    package_key[i] = static_cast<unsigned char>(0x91 + i * 11);
  for (std::size_t i = 0; i < wrap_nonce.size(); ++i)
    wrap_nonce[i] = static_cast<unsigned char>(0x61 + i * 7);

  Json::Value outer_metadata(Json::objectValue);
  outer_metadata["format"] = "xgrib-xtd";
  outer_metadata["format_version"] = 2;
  outer_metadata["package_version"] = "synthetic-v2";
  outer_metadata["parent_package_hash"] = HexDigest(tide_hash);
  outer_metadata["tide_component_hash"] = HexDigest(tide_hash);
  outer_metadata["climatology_representation"] =
      options.representation == XtdV2ResidualRepresentation::kHarmonic2
          ? "harmonic2"
          : "monthly12";
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const auto outer_json = Json::writeString(writer, outer_metadata);
  const Bytes outer_metadata_bytes(outer_json.begin(), outer_json.end());

  constexpr std::uint32_t component_count = 3;
  const std::uint64_t directory_offset =
      kHeaderSize + outer_metadata_bytes.size();
  const std::uint64_t payload_offset =
      directory_offset + component_count * kV2ComponentEntrySize;
  const std::uint64_t nested_offset = payload_offset;
  const std::uint64_t residual_offset = nested_offset + nested.size();
  const std::uint32_t residual_representation =
      options.representation == XtdV2ResidualRepresentation::kHarmonic2 ? 2 : 3;
  auto residual =
      MakeV2TiledComponent(options, 2, residual_representation, package_id,
                           package_key, residual_offset);
  const auto uncertainty_offset =
      residual.payload_offset + residual.payload.size();
  auto uncertainty = MakeV2TiledComponent(options, 3, 4, package_id,
                                          package_key, uncertainty_offset);
  const std::uint64_t file_length =
      uncertainty.payload_offset + uncertainty.payload.size();

  Bytes directory(component_count * kV2ComponentEntrySize);
  std::array<unsigned char, 16> tide_id{};
  for (std::size_t i = 0; i < tide_id.size(); ++i)
    tide_id[i] = static_cast<unsigned char>(0x11 + i * 7);
  PutLe(&directory, 0, static_cast<std::uint32_t>(1));
  PutLe(&directory, 4, static_cast<std::uint32_t>(1));
  PutLe(&directory, 8, static_cast<std::uint32_t>(1));
  PutLe(&directory, 12, static_cast<std::uint32_t>(1 | 8));
  std::copy(tide_id.begin(), tide_id.end(), directory.begin() + 16);
  PutLe(&directory, 96, nested_offset);
  PutLe(&directory, 104, static_cast<std::uint64_t>(nested.size()));
  std::copy(tide_hash.begin(), tide_hash.end(), directory.begin() + 112);
  std::copy(tide_hash.begin(), tide_hash.end(), directory.begin() + 144);

  const std::uint32_t columns =
      (options.tide.nx + options.tile_width - 1) / options.tile_width;
  const std::uint32_t rows =
      (options.tide.ny + options.tile_height - 1) / options.tile_height;
  for (std::size_t component_index = 0; component_index < 2;
       ++component_index) {
    const auto& component = component_index == 0 ? residual : uncertainty;
    const std::size_t position = (component_index + 1) * kV2ComponentEntrySize;
    PutLe(&directory, position, component.type);
    PutLe(&directory, position + 4, component.representation);
    PutLe(&directory, position + 8, static_cast<std::uint32_t>(1));
    PutLe(&directory, position + 12, static_cast<std::uint32_t>(1));
    std::copy(component.id.begin(), component.id.end(),
              directory.begin() + position + 16);
    PutLe(&directory, position + 32, static_cast<std::uint32_t>(1));
    PutLe(&directory, position + 36, static_cast<std::uint32_t>(64));
    PutLe(&directory, position + 40, columns * rows);
    PutLe(&directory, position + 48, component.metadata_offset);
    PutLe(&directory, position + 56,
          static_cast<std::uint64_t>(component.metadata.size()));
    PutLe(&directory, position + 64, component.index_offset);
    PutLe(&directory, position + 72,
          static_cast<std::uint64_t>(component.index.size()));
    PutLe(&directory, position + 80, component.payload_offset);
    PutLe(&directory, position + 88,
          static_cast<std::uint64_t>(component.payload.size()));
    std::copy(component.logical_hash.begin(), component.logical_hash.end(),
              directory.begin() + position + 112);
    std::copy(component.stored_hash.begin(), component.stored_hash.end(),
              directory.begin() + position + 144);
  }

  Bytes header(kHeaderSize);
  std::copy(kV2Magic.begin(), kV2Magic.end(), header.begin());
  PutLe(&header, 8, static_cast<std::uint32_t>(2));
  PutLe(&header, 12, static_cast<std::uint32_t>(kHeaderSize));
  PutLe(&header, 16, static_cast<std::uint32_t>(0x01020304));
  PutLe(&header, 24, component_count);
  PutLe(&header, 28, static_cast<std::uint32_t>(kV2ComponentEntrySize));
  PutLe(&header, 32, static_cast<std::uint64_t>(kHeaderSize));
  PutLe(&header, 40, static_cast<std::uint64_t>(outer_metadata_bytes.size()));
  PutLe(&header, 48, directory_offset);
  PutLe(&header, 56, static_cast<std::uint64_t>(directory.size()));
  PutLe(&header, 64, payload_offset);
  PutLe(&header, 72, file_length);
  std::copy(package_id.begin(), package_id.end(), header.begin() + 80);
  std::copy(wrap_nonce.begin(), wrap_nonce.end(), header.begin() + 96);
  const auto root_key = xtd_internal::RuntimeRootKey();
  std::array<unsigned char, 28> wrapper_aad{};
  std::copy(kV2Magic.begin(), kV2Magic.end(), wrapper_aad.begin());
  wrapper_aad[8] = 2;
  std::copy(package_id.begin(), package_id.end(), wrapper_aad.begin() + 12);
  unsigned long long wrapped_length = 0;
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(
          header.data() + 120, &wrapped_length, package_key.data(),
          package_key.size(), wrapper_aad.data(), wrapper_aad.size(), nullptr,
          wrap_nonce.data(), root_key.data()) != 0 ||
      wrapped_length != 48) {
    throw std::runtime_error("could not wrap XTD v2 fixture package key");
  }
  crypto_generichash_state mac_state;
  std::array<unsigned char, 32> public_mac{};
  crypto_generichash_init(&mac_state, package_key.data(), package_key.size(),
                          public_mac.size());
  crypto_generichash_update(&mac_state, header.data(), header.size());
  crypto_generichash_update(&mac_state, outer_metadata_bytes.data(),
                            outer_metadata_bytes.size());
  crypto_generichash_update(&mac_state, directory.data(), directory.size());
  crypto_generichash_final(&mac_state, public_mac.data(), public_mac.size());
  std::copy(public_mac.begin(), public_mac.end(), header.begin() + 168);

  const auto directory_path = path.parent_path();
  if (!directory_path.empty())
    std::filesystem::create_directories(directory_path);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(header.data()), header.size());
  output.write(reinterpret_cast<const char*>(outer_metadata_bytes.data()),
               static_cast<std::streamsize>(outer_metadata_bytes.size()));
  output.write(reinterpret_cast<const char*>(directory.data()),
               directory.size());
  output.write(reinterpret_cast<const char*>(nested.data()), nested.size());
  for (const auto* component : {&residual, &uncertainty}) {
    output.write(reinterpret_cast<const char*>(component->metadata.data()),
                 component->metadata.size());
    output.write(reinterpret_cast<const char*>(component->index.data()),
                 component->index.size());
    output.write(reinterpret_cast<const char*>(component->payload.data()),
                 component->payload.size());
  }
  if (!output) throw std::runtime_error("could not write XTD v2 fixture");
}

void CorruptXtdFixtureByte(const std::filesystem::path& path,
                           std::uint64_t offset, unsigned char xor_mask) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file)
    throw std::runtime_error("could not open XTD fixture for corruption");
  file.seekg(static_cast<std::streamoff>(offset));
  char value = 0;
  if (!file.read(&value, 1)) {
    throw std::runtime_error("XTD fixture corruption offset is out of range");
  }
  value = static_cast<char>(static_cast<unsigned char>(value) ^ xor_mask);
  file.seekp(static_cast<std::streamoff>(offset));
  if (!file.write(&value, 1)) {
    throw std::runtime_error("could not corrupt XTD fixture byte");
  }
}

void TruncateXtdFixture(const std::filesystem::path& path,
                        std::uint64_t length) {
  std::error_code error;
  std::filesystem::resize_file(path, length, error);
  if (error) throw std::runtime_error("could not truncate XTD fixture");
}

}  // namespace environmental_grib::test
