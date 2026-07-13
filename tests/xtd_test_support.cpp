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

constexpr std::array<unsigned char, 8> kMagic{'X', 'G', 'R', 'I', 'B', 'X',
                                               '1', 0};
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
  const std::uint64_t size = 40ULL + mask_bytes * 2ULL + scale_count * 4ULL +
                             coefficient_bytes;
  if (size > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("XTD fixture tile is too large");
  }
  Bytes result(static_cast<std::size_t>(size));
  std::copy_n("XTP1", 4, result.begin());
  PutLe(&result, 4, static_cast<std::uint16_t>(1));
  PutLe(&result, 6,
        static_cast<std::uint16_t>(options.constituents.size()));
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
        result[cursor + cell / 8] |= static_cast<unsigned char>(1U << (cell % 8));
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
  const long maximum_quantized =
      options.coefficient_bits == 12 ? 2047 : 32767;
  for (std::size_t constituent = 0;
       constituent < options.constituents.size(); ++constituent) {
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
      const auto first =
          static_cast<std::uint16_t>(coefficients[i]) & 0x0fffU;
      const auto second =
          static_cast<std::uint16_t>(coefficients[i + 1]) & 0x0fffU;
      result[cursor++] = static_cast<unsigned char>(first & 0xffU);
      result[cursor++] = static_cast<unsigned char>(
          ((first >> 8) & 0x0fU) | ((second & 0x0fU) << 4));
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
  const std::size_t size = ZSTD_compress(compressed.data(), compressed.size(),
                                         plaintext.data(), plaintext.size(), level);
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
    PutLe(&index, cursor + 32,
          static_cast<std::uint32_t>(
              tile.compressed.size() +
              crypto_aead_xchacha20poly1305_ietf_ABYTES));
    PutLe(&index, cursor + 36,
          static_cast<std::uint32_t>(tile.plaintext.size()));
    std::copy(tile.nonce.begin(), tile.nonce.end(), index.begin() + cursor + 40);
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
  PutLe(&header, 48,
        static_cast<std::uint32_t>(options.constituents.size()));
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
      crypto_generichash_update(&mac_state, header.data(), header.size()) != 0 ||
      crypto_generichash_update(&mac_state, metadata_bytes.data(),
                                metadata_bytes.size()) != 0 ||
      crypto_generichash_update(&mac_state, index.data(), index.size()) != 0 ||
      crypto_generichash_final(&mac_state, mac.data(), mac.size()) != 0) {
    throw std::runtime_error("could not authenticate XTD fixture public region");
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

void CorruptXtdFixtureByte(const std::filesystem::path& path,
                           std::uint64_t offset, unsigned char xor_mask) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) throw std::runtime_error("could not open XTD fixture for corruption");
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
