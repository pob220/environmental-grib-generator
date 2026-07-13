#include "environmental_grib/xtd.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sodium.h>
#include <zstd.h>

#include "environmental_grib/error.h"
#include "xtd_crypto_internal.h"

namespace environmental_grib {
namespace {

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<unsigned char>;

struct SecureKey {
  std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_KEYBYTES> bytes{};

  ~SecureKey() { sodium_memzero(bytes.data(), bytes.size()); }
  unsigned char* data() noexcept { return bytes.data(); }
  const unsigned char* data() const noexcept { return bytes.data(); }
  constexpr std::size_t size() const noexcept { return bytes.size(); }
};

constexpr std::array<unsigned char, 8> kMagic{'X', 'G', 'R', 'I', 'B', 'X',
                                               '1', 0};
constexpr std::array<unsigned char, 4> kTileMagic{'X', 'T', 'P', '1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kHeaderSize = 512;
constexpr std::uint32_t kEndianMarker = 0x01020304;
constexpr std::uint32_t kIndexEntrySize = 64;
constexpr std::uint32_t kEmptyTile = 1;
constexpr std::uint32_t kZstdCompression = 1;
constexpr std::uint32_t kRawCoefficientEncoding = 0;
constexpr std::uint32_t kDelta2dCoefficientEncoding = 1;
constexpr std::uint32_t kDelta2dInt12CoefficientEncoding = 2;
constexpr std::uint64_t kMaximumIndexEntries = 1'000'000;
constexpr std::uint64_t kMaximumMetadataBytes = 16 * 1024 * 1024;

[[noreturn]] void Invalid(const std::string& message) {
  throw ValidationError("invalid XTD package: " + message);
}

template <typename T>
T ReadLe(const unsigned char* data) {
  static_assert(std::is_integral_v<T>);
  using U = std::make_unsigned_t<T>;
  U value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<U>(data[i]) << (i * 8);
  }
  return static_cast<T>(value);
}

double ReadF64(const unsigned char* data) {
  const auto bits = ReadLe<std::uint64_t>(data);
  return std::bit_cast<double>(bits);
}

float ReadF32(const unsigned char* data) {
  const auto bits = ReadLe<std::uint32_t>(data);
  return std::bit_cast<float>(bits);
}

std::uint64_t CheckedAdd(std::uint64_t left, std::uint64_t right,
                         std::string_view what) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    Invalid(std::string(what) + " overflows");
  }
  return left + right;
}

std::uint64_t CheckedMultiply(std::uint64_t left, std::uint64_t right,
                              std::string_view what) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    Invalid(std::string(what) + " overflows");
  }
  return left * right;
}

Bytes ReadAt(std::ifstream& input, std::uint64_t offset, std::size_t length,
             const std::string& description) {
  if (offset > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamoff>::max())) {
    Invalid(description + " offset is not representable");
  }
  Bytes result(length);
  input.clear();
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!input ||
      (length != 0 &&
       !input.read(reinterpret_cast<char*>(result.data()),
                   static_cast<std::streamsize>(length)))) {
    Invalid("could not read " + description);
  }
  return result;
}

struct Header {
  std::uint32_t flags{};
  std::uint32_t nx{}, ny{}, tile_width{}, tile_height{};
  std::uint32_t tile_columns{}, tile_rows{}, constituent_count{};
  std::uint64_t metadata_offset{}, metadata_length{}, index_offset{};
  std::uint64_t index_count{}, payload_offset{};
  double lon_u0{}, lon_step{}, lat_u0{}, lat_step{};
  double lon_v0{}, lat_v0{};
  BoundingBox bbox;
  std::array<unsigned char, 16> package_id{};
  std::array<unsigned char, 24> wrap_nonce{};
  std::array<unsigned char, 48> wrapped_content_key{};
  std::uint64_t file_length{};
  std::array<unsigned char, 32> public_region_mac{};
};

Header ParseHeader(const Bytes& raw, std::uint64_t actual_file_length) {
  if (raw.size() != kHeaderSize ||
      !std::equal(kMagic.begin(), kMagic.end(), raw.begin())) {
    Invalid("bad magic or truncated header");
  }
  if (ReadLe<std::uint32_t>(&raw[8]) != kVersion) {
    Invalid("unsupported version");
  }
  if (ReadLe<std::uint32_t>(&raw[12]) != kHeaderSize) {
    Invalid("header size is not 512 bytes");
  }
  if (ReadLe<std::uint32_t>(&raw[16]) != kEndianMarker) {
    Invalid("bad endian marker");
  }

  Header h;
  h.flags = ReadLe<std::uint32_t>(&raw[20]);
  h.nx = ReadLe<std::uint32_t>(&raw[24]);
  h.ny = ReadLe<std::uint32_t>(&raw[28]);
  h.tile_width = ReadLe<std::uint32_t>(&raw[32]);
  h.tile_height = ReadLe<std::uint32_t>(&raw[36]);
  h.tile_columns = ReadLe<std::uint32_t>(&raw[40]);
  h.tile_rows = ReadLe<std::uint32_t>(&raw[44]);
  h.constituent_count = ReadLe<std::uint32_t>(&raw[48]);
  if (ReadLe<std::uint32_t>(&raw[52]) != kIndexEntrySize) {
    Invalid("index entry size is not 64 bytes");
  }
  h.metadata_offset = ReadLe<std::uint64_t>(&raw[56]);
  h.metadata_length = ReadLe<std::uint64_t>(&raw[64]);
  h.index_offset = ReadLe<std::uint64_t>(&raw[72]);
  h.index_count = ReadLe<std::uint64_t>(&raw[80]);
  h.payload_offset = ReadLe<std::uint64_t>(&raw[88]);
  h.lon_u0 = ReadF64(&raw[96]);
  h.lon_step = ReadF64(&raw[104]);
  h.lat_u0 = ReadF64(&raw[112]);
  h.lat_step = ReadF64(&raw[120]);
  h.lon_v0 = ReadF64(&raw[128]);
  h.lat_v0 = ReadF64(&raw[136]);
  h.bbox = {ReadF64(&raw[144]), ReadF64(&raw[152]), ReadF64(&raw[160]),
            ReadF64(&raw[168])};
  std::copy_n(raw.begin() + 176, h.package_id.size(), h.package_id.begin());
  std::copy_n(raw.begin() + 192, h.wrap_nonce.size(), h.wrap_nonce.begin());
  std::copy_n(raw.begin() + 216, h.wrapped_content_key.size(),
              h.wrapped_content_key.begin());
  h.file_length = ReadLe<std::uint64_t>(&raw[264]);
  std::copy_n(raw.begin() + 272, h.public_region_mac.size(),
              h.public_region_mac.begin());

  if (h.flags != 0) Invalid("unsupported header flags");
  if (h.nx < 2 || h.nx > 20'000 || h.ny < 2 || h.ny > 10'000) {
    Invalid("grid dimensions are outside supported limits");
  }
  if (h.tile_width == 0 || h.tile_width > 256 || h.tile_height == 0 ||
      h.tile_height > 256) {
    Invalid("tile dimensions are outside supported limits");
  }
  if (h.constituent_count == 0 || h.constituent_count > 64) {
    Invalid("constituent count is outside supported limits");
  }
  const std::uint32_t expected_columns =
      (h.nx + h.tile_width - 1) / h.tile_width;
  const std::uint32_t expected_rows =
      (h.ny + h.tile_height - 1) / h.tile_height;
  if (h.tile_columns != expected_columns || h.tile_rows != expected_rows) {
    Invalid("tile grid does not match dimensions");
  }
  const std::uint64_t expected_index_count = CheckedMultiply(
      h.tile_columns, h.tile_rows, "tile grid size");
  if (h.index_count != expected_index_count ||
      h.index_count > kMaximumIndexEntries) {
    Invalid("index count does not match tile grid or exceeds its limit");
  }
  if (h.file_length != actual_file_length) {
    Invalid("declared file length does not match the file");
  }
  if (h.metadata_offset != kHeaderSize || h.metadata_length == 0 ||
      h.metadata_length > kMaximumMetadataBytes) {
    Invalid("metadata location or length is invalid");
  }
  if (h.index_offset !=
      CheckedAdd(h.metadata_offset, h.metadata_length, "metadata range")) {
    Invalid("index does not immediately follow metadata");
  }
  const std::uint64_t index_bytes =
      CheckedMultiply(h.index_count, kIndexEntrySize, "index size");
  if (h.payload_offset !=
      CheckedAdd(h.index_offset, index_bytes, "index range") ||
      h.payload_offset > h.file_length) {
    Invalid("payload offset does not follow the index");
  }
  const std::array<double, 10> finite_values{
      h.lon_u0, h.lon_step, h.lat_u0, h.lat_step, h.lon_v0,
      h.lat_v0, h.bbox.west, h.bbox.south, h.bbox.east, h.bbox.north};
  if (!std::all_of(finite_values.begin(), finite_values.end(),
                   [](double value) { return std::isfinite(value); }) ||
      h.lon_step <= 0.0 || h.lat_step <= 0.0) {
    Invalid("coordinate axes contain invalid values");
  }
  if (h.bbox.west < -180.0 || h.bbox.west > 360.0 ||
      h.bbox.east < -180.0 || h.bbox.east > 360.0 ||
      h.bbox.west >= h.bbox.east || h.bbox.east - h.bbox.west > 360.0 ||
      h.bbox.south < -90.0 || h.bbox.north > 90.0 ||
      h.bbox.south >= h.bbox.north) {
    Invalid("bounding box is invalid");
  }
  if (!std::all_of(raw.begin() + 304, raw.end(),
                   [](unsigned char value) { return value == 0; })) {
    Invalid("reserved header bytes are nonzero");
  }
  return h;
}

Json::Value ParseMetadata(const Bytes& raw, const Header& header,
                          std::vector<std::string>* constituents) {
  std::size_t position = 0;
  while (position < raw.size()) {
    const unsigned char first = raw[position];
    std::size_t continuation = 0;
    std::uint32_t codepoint = 0;
    if (first <= 0x7f) {
      codepoint = first;
    } else if (first >= 0xc2 && first <= 0xdf) {
      continuation = 1;
      codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation = 2;
      codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation = 3;
      codepoint = first & 0x07;
    } else {
      Invalid("metadata is not valid UTF-8");
    }
    if (continuation > raw.size() - position - 1) {
      Invalid("metadata is not valid UTF-8");
    }
    for (std::size_t i = 1; i <= continuation; ++i) {
      const unsigned char byte = raw[position + i];
      if ((byte & 0xc0) != 0x80) Invalid("metadata is not valid UTF-8");
      codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    const bool overlong =
        (continuation == 1 && codepoint < 0x80) ||
        (continuation == 2 && codepoint < 0x800) ||
        (continuation == 3 && codepoint < 0x10000);
    if (overlong || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
      Invalid("metadata is not valid UTF-8");
    }
    position += continuation + 1;
  }

  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  builder["failIfExtra"] = true;
  builder["rejectDupKeys"] = true;
  Json::Value metadata;
  std::string errors;
  const std::string json(reinterpret_cast<const char*>(raw.data()), raw.size());
  std::istringstream input(json);
  if (!Json::parseFromStream(builder, input, &metadata, &errors) ||
      !metadata.isObject()) {
    Invalid("metadata must be one UTF-8 JSON object");
  }
  const Json::Value& names = metadata["constituents"];
  if (!names.isArray() || names.size() != header.constituent_count) {
    Invalid("metadata constituents do not match the header");
  }
  constituents->clear();
  constituents->reserve(names.size());
  for (const auto& name : names) {
    if (!name.isString() || name.asString().empty() ||
        name.asString().size() > 32) {
      Invalid("metadata contains an invalid constituent name");
    }
    if (std::find(constituents->begin(), constituents->end(), name.asString()) !=
        constituents->end()) {
      Invalid("metadata constituent names must be unique");
    }
    constituents->push_back(name.asString());
  }
  if (metadata.isMember("format") && !metadata["format"].isString()) {
    Invalid("metadata format must be a string");
  }
  if (metadata.isMember("format_version") &&
      (!metadata["format_version"].isUInt() ||
       metadata["format_version"].asUInt() != 1)) {
    Invalid("metadata format_version must be 1");
  }
  if (metadata.isMember("model_name") &&
      (!metadata["model_name"].isString() ||
       metadata["model_name"].asString().empty())) {
    Invalid("metadata model_name must be a nonempty string");
  }
  if (metadata.isMember("velocity_units") &&
      (!metadata["velocity_units"].isString() ||
       metadata["velocity_units"].asString() != "cm/s")) {
    Invalid("metadata velocity_units must be cm/s");
  }
  if (metadata.isMember("corrections") &&
      (!metadata["corrections"].isString() ||
       metadata["corrections"].asString() != "ATLAS")) {
    Invalid("metadata corrections must be ATLAS");
  }
  return metadata;
}

struct IndexEntry {
  std::uint32_t tile_id{}, tx{}, ty{}, flags{}, compression{};
  std::uint16_t width{}, height{};
  std::uint64_t offset{};
  std::uint32_t encrypted_length{}, plaintext_length{};
  std::array<unsigned char, 24> nonce{};
  std::array<unsigned char, 40> aad_entry{};
};

std::uint64_t ExpectedPlaintextLength(const Header& h, std::uint16_t width,
                                      std::uint16_t height,
                                      std::uint32_t coefficient_bits) {
  const std::uint64_t cells = CheckedMultiply(width, height, "tile cell count");
  const std::uint64_t mask_bytes = (cells + 7) / 8;
  const std::uint64_t scales = CheckedMultiply(h.constituent_count, 4 * 4,
                                               "tile scale bytes");
  const std::uint64_t coefficient_count = CheckedMultiply(
      CheckedMultiply(h.constituent_count, 4, "tile component count"),
      cells, "tile coefficient count");
  const std::uint64_t coefficients =
      (CheckedMultiply(coefficient_count, coefficient_bits,
                       "tile coefficient bits") + 7) /
      8;
  return CheckedAdd(CheckedAdd(40, mask_bytes * 2, "tile masks"),
                    CheckedAdd(scales, coefficients, "tile data"),
                    "tile plaintext length");
}

std::vector<IndexEntry> ParseIndex(const Bytes& raw, const Header& h) {
  std::vector<IndexEntry> entries;
  entries.reserve(static_cast<std::size_t>(h.index_count));
  std::uint64_t previous_end = h.payload_offset;
  for (std::uint64_t i = 0; i < h.index_count; ++i) {
    const unsigned char* item = &raw[static_cast<std::size_t>(i * 64)];
    IndexEntry entry;
    entry.tile_id = ReadLe<std::uint32_t>(item);
    entry.tx = ReadLe<std::uint32_t>(item + 4);
    entry.ty = ReadLe<std::uint32_t>(item + 8);
    entry.width = ReadLe<std::uint16_t>(item + 12);
    entry.height = ReadLe<std::uint16_t>(item + 14);
    entry.flags = ReadLe<std::uint32_t>(item + 16);
    entry.compression = ReadLe<std::uint32_t>(item + 20);
    entry.offset = ReadLe<std::uint64_t>(item + 24);
    entry.encrypted_length = ReadLe<std::uint32_t>(item + 32);
    entry.plaintext_length = ReadLe<std::uint32_t>(item + 36);
    std::copy_n(item + 40, entry.nonce.size(), entry.nonce.begin());
    std::copy_n(item, entry.aad_entry.size(), entry.aad_entry.begin());

    const std::uint32_t expected_tx = static_cast<std::uint32_t>(i) % h.tile_columns;
    const std::uint32_t expected_ty = static_cast<std::uint32_t>(i) / h.tile_columns;
    const std::uint16_t expected_width = static_cast<std::uint16_t>(
        std::min(h.tile_width, h.nx - expected_tx * h.tile_width));
    const std::uint16_t expected_height = static_cast<std::uint16_t>(
        std::min(h.tile_height, h.ny - expected_ty * h.tile_height));
    if (entry.tile_id != i || entry.tx != expected_tx ||
        entry.ty != expected_ty || entry.width != expected_width ||
        entry.height != expected_height) {
      Invalid("index tile grid is inconsistent");
    }
    if ((entry.flags & ~kEmptyTile) != 0) {
      Invalid("index contains unsupported tile flags");
    }
    if ((entry.flags & kEmptyTile) != 0) {
      if (entry.compression != 0 || entry.offset != 0 ||
          entry.encrypted_length != 0 || entry.plaintext_length != 0 ||
          !std::all_of(entry.nonce.begin(), entry.nonce.end(),
                       [](unsigned char value) { return value == 0; })) {
        Invalid("empty tile carries payload fields");
      }
    } else {
      const std::uint64_t expected_int16 =
          ExpectedPlaintextLength(h, entry.width, entry.height, 16);
      const std::uint64_t expected_int12 =
          ExpectedPlaintextLength(h, entry.width, entry.height, 12);
      if (entry.compression != kZstdCompression ||
          entry.encrypted_length < crypto_aead_xchacha20poly1305_ietf_ABYTES ||
          (entry.plaintext_length != expected_int16 &&
           entry.plaintext_length != expected_int12)) {
        Invalid("tile payload lengths or compression are invalid");
      }
      const std::uint64_t end = CheckedAdd(entry.offset, entry.encrypted_length,
                                           "tile payload range");
      if (entry.offset < h.payload_offset || entry.offset < previous_end ||
          end > h.file_length) {
        Invalid("tile payload ranges are unsorted, overlapping, or out of bounds");
      }
      previous_end = end;
    }
    entries.push_back(entry);
  }
  return entries;
}

std::array<unsigned char, 28> WrapperAad(const Header& h) {
  std::array<unsigned char, 28> aad{};
  std::copy(kMagic.begin(), kMagic.end(), aad.begin());
  aad[8] = 1;
  std::copy(h.package_id.begin(), h.package_id.end(), aad.begin() + 12);
  return aad;
}

std::array<unsigned char, 56> TileAad(const Header& h,
                                      const IndexEntry& entry) {
  std::array<unsigned char, 56> aad{};
  std::copy(h.package_id.begin(), h.package_id.end(), aad.begin());
  std::copy(entry.aad_entry.begin(), entry.aad_entry.end(), aad.begin() + 16);
  return aad;
}

struct Tile {
  bool empty{};
  std::uint16_t width{}, height{};
  std::vector<unsigned char> u_mask, v_mask;
  std::vector<float> scales;
  std::vector<std::int16_t> coefficients;

  [[nodiscard]] std::uint64_t ByteSize() const {
    return sizeof(Tile) + u_mask.capacity() + v_mask.capacity() +
           scales.capacity() * sizeof(float) +
           coefficients.capacity() * sizeof(std::int16_t);
  }

  [[nodiscard]] bool Valid(bool u_component, std::size_t cell) const {
    if (empty) return false;
    const auto& mask = u_component ? u_mask : v_mask;
    return (mask[cell / 8] & (1U << (cell % 8))) != 0;
  }

  [[nodiscard]] double Value(std::size_t constituent, std::size_t component,
                             std::size_t cell) const {
    const std::size_t cells = static_cast<std::size_t>(width) * height;
    const std::size_t component_index = constituent * 4 + component;
    return static_cast<double>(coefficients[component_index * cells + cell]) *
           static_cast<double>(scales[component_index]);
  }
};

std::shared_ptr<Tile> ParseTile(const Bytes& raw, const Header& h,
                                const IndexEntry& entry) {
  if (raw.size() != entry.plaintext_length || raw.size() < 40 ||
      !std::equal(kTileMagic.begin(), kTileMagic.end(), raw.begin())) {
    Invalid("tile plaintext has a bad header");
  }
  const std::uint16_t version = ReadLe<std::uint16_t>(&raw[4]);
  const std::uint16_t constituents = ReadLe<std::uint16_t>(&raw[6]);
  const std::uint16_t width = ReadLe<std::uint16_t>(&raw[8]);
  const std::uint16_t height = ReadLe<std::uint16_t>(&raw[10]);
  const std::uint32_t cells = ReadLe<std::uint32_t>(&raw[12]);
  const std::uint32_t u_mask_bytes = ReadLe<std::uint32_t>(&raw[16]);
  const std::uint32_t v_mask_bytes = ReadLe<std::uint32_t>(&raw[20]);
  const std::uint32_t scale_count = ReadLe<std::uint32_t>(&raw[24]);
  const std::uint64_t coefficient_count = ReadLe<std::uint64_t>(&raw[28]);
  const std::uint32_t coefficient_encoding =
      ReadLe<std::uint32_t>(&raw[36]);
  const std::uint32_t expected_cells =
      static_cast<std::uint32_t>(entry.width) * entry.height;
  const std::uint32_t expected_mask_bytes = (expected_cells + 7) / 8;
  const std::uint64_t expected_coefficients =
      static_cast<std::uint64_t>(h.constituent_count) * 4 * expected_cells;
  const std::uint32_t coefficient_bits =
      coefficient_encoding == kDelta2dInt12CoefficientEncoding ? 12 : 16;
  const std::uint64_t expected_plaintext_length =
      ExpectedPlaintextLength(h, entry.width, entry.height, coefficient_bits);
  if (version != 1 || constituents != h.constituent_count ||
      width != entry.width || height != entry.height ||
      cells != expected_cells || u_mask_bytes != expected_mask_bytes ||
      v_mask_bytes != expected_mask_bytes ||
      scale_count != h.constituent_count * 4 ||
      coefficient_count != expected_coefficients ||
      raw.size() != expected_plaintext_length ||
      (coefficient_encoding != kRawCoefficientEncoding &&
       coefficient_encoding != kDelta2dCoefficientEncoding &&
       coefficient_encoding != kDelta2dInt12CoefficientEncoding)) {
    Invalid("tile plaintext fields do not match its index entry");
  }

  std::size_t cursor = 40;
  auto tile = std::make_shared<Tile>();
  tile->width = width;
  tile->height = height;
  tile->u_mask.assign(raw.begin() + static_cast<std::ptrdiff_t>(cursor),
                      raw.begin() + static_cast<std::ptrdiff_t>(cursor + u_mask_bytes));
  cursor += u_mask_bytes;
  tile->v_mask.assign(raw.begin() + static_cast<std::ptrdiff_t>(cursor),
                      raw.begin() + static_cast<std::ptrdiff_t>(cursor + v_mask_bytes));
  cursor += v_mask_bytes;
  if ((expected_cells % 8) != 0) {
    const unsigned char used = static_cast<unsigned char>((1U << (expected_cells % 8)) - 1U);
    if ((tile->u_mask.back() & ~used) != 0 ||
        (tile->v_mask.back() & ~used) != 0) {
      Invalid("tile validity masks have nonzero padding bits");
    }
  }
  tile->scales.reserve(scale_count);
  for (std::uint32_t i = 0; i < scale_count; ++i, cursor += 4) {
    const float value = ReadF32(&raw[cursor]);
    if (!std::isfinite(value)) {
      Invalid("tile contains an invalid quantization scale");
    }
    tile->scales.push_back(value);
  }
  tile->coefficients.reserve(static_cast<std::size_t>(coefficient_count));
  if (coefficient_encoding == kDelta2dInt12CoefficientEncoding) {
    if ((coefficient_count & 1U) != 0) {
      Invalid("12-bit tile has an odd coefficient count");
    }
    for (std::uint64_t i = 0; i < coefficient_count; i += 2, cursor += 3) {
      const std::uint16_t first = static_cast<std::uint16_t>(
          raw[cursor] | ((raw[cursor + 1] & 0x0fU) << 8));
      const std::uint16_t second = static_cast<std::uint16_t>(
          (raw[cursor + 1] >> 4) | (raw[cursor + 2] << 4));
      const auto signed12 = [](std::uint16_t value) {
        return static_cast<std::int16_t>(
            (value & 0x800U) != 0 ? static_cast<int>(value) - 4096
                                  : static_cast<int>(value));
      };
      tile->coefficients.push_back(signed12(first));
      tile->coefficients.push_back(signed12(second));
    }
  } else {
    for (std::uint64_t i = 0; i < coefficient_count; ++i, cursor += 2) {
      tile->coefficients.push_back(ReadLe<std::int16_t>(&raw[cursor]));
    }
  }
  if (coefficient_encoding == kDelta2dCoefficientEncoding ||
      coefficient_encoding == kDelta2dInt12CoefficientEncoding) {
    const std::uint16_t modulus_mask =
        coefficient_encoding == kDelta2dInt12CoefficientEncoding ? 0x0fffU
                                                                  : 0xffffU;
    const std::size_t cells_per_component =
        static_cast<std::size_t>(width) * height;
    for (std::uint32_t component = 0;
         component < scale_count; ++component) {
      const std::size_t base = component * cells_per_component;
      for (std::size_t y = 1; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
          const std::size_t cell = base + y * width + x;
          const std::uint16_t residual = static_cast<std::uint16_t>(
              tile->coefficients[cell]) & modulus_mask;
          const std::uint16_t above = static_cast<std::uint16_t>(
              tile->coefficients[cell - width]) & modulus_mask;
          const std::uint16_t decoded =
              static_cast<std::uint16_t>(residual + above) & modulus_mask;
          tile->coefficients[cell] =
              coefficient_encoding == kDelta2dInt12CoefficientEncoding
                  ? static_cast<std::int16_t>(
                        (decoded & 0x800U) != 0
                            ? static_cast<int>(decoded) - 4096
                            : static_cast<int>(decoded))
                  : std::bit_cast<std::int16_t>(decoded);
        }
      }
      for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 1; x < width; ++x) {
          const std::size_t cell = base + y * width + x;
          const std::uint16_t residual = static_cast<std::uint16_t>(
              tile->coefficients[cell]) & modulus_mask;
          const std::uint16_t left = static_cast<std::uint16_t>(
              tile->coefficients[cell - 1]) & modulus_mask;
          const std::uint16_t decoded =
              static_cast<std::uint16_t>(residual + left) & modulus_mask;
          tile->coefficients[cell] =
              coefficient_encoding == kDelta2dInt12CoefficientEncoding
                  ? static_cast<std::int16_t>(
                        (decoded & 0x800U) != 0
                            ? static_cast<int>(decoded) - 4096
                            : static_cast<int>(decoded))
                  : std::bit_cast<std::int16_t>(decoded);
        }
      }
    }
  }
  if (cursor != raw.size()) Invalid("tile plaintext has trailing bytes");
  return tile;
}

struct Bracket {
  std::uint32_t first{}, second{};
  double fraction{};
  bool valid{};
};

Bracket LatitudeBracket(double value, double origin, double step,
                         std::uint32_t count) {
  double position = (value - origin) / step;
  const double tolerance = 1e-10;
  if (position < -tolerance ||
      position > static_cast<double>(count - 1) + tolerance) {
    return {};
  }
  position = std::clamp(position, 0.0, static_cast<double>(count - 1));
  std::uint32_t first = static_cast<std::uint32_t>(std::floor(position));
  if (first == count - 1) return {count - 2, count - 1, 1.0, true};
  return {first, first + 1, position - first, true};
}

Bracket LongitudeBracket(double value, double origin, double step,
                          std::uint32_t count) {
  const double span = step * count;
  const bool global = std::abs(span - 360.0) <=
                      std::max(1e-8, std::abs(span) * 1e-10);
  if (global) {
    double position = std::fmod((value - origin) / step,
                                static_cast<double>(count));
    if (position < 0.0) position += count;
    if (position >= count) position = 0.0;
    const auto first = static_cast<std::uint32_t>(std::floor(position));
    return {first, (first + 1) % count, position - first, true};
  }

  double adjusted = value;
  const double last = origin + step * (count - 1);
  adjusted += 360.0 * std::round((origin - adjusted) / 360.0);
  if (adjusted < origin) adjusted += 360.0;
  if (adjusted > last && adjusted - 360.0 >= origin) adjusted -= 360.0;
  return LatitudeBracket(adjusted, origin, step, count);
}

void ValidateOutputGrid(const RegularGrid& grid) {
  if (grid.nx() == 0 || grid.ny() == 0 ||
      grid.ny() > std::numeric_limits<std::size_t>::max() / grid.nx()) {
    throw ValidationError("XTD output grid is empty or too large");
  }
  if (!std::all_of(grid.longitudes.begin(), grid.longitudes.end(),
                   [](double value) { return std::isfinite(value); }) ||
      !std::all_of(grid.latitudes.begin(), grid.latitudes.end(),
                   [](double value) { return std::isfinite(value); })) {
    throw ValidationError("XTD output grid coordinates must be finite");
  }
}

}  // namespace

class XtdReader::Impl {
 public:
  Impl(const std::filesystem::path& path, XtdReaderOptions options)
      : path_(path),
        capacity_(options.tile_cache_capacity),
        max_cache_bytes_(options.tile_cache_max_bytes),
        input_(path, std::ios::binary) {
    const auto validation_started = Clock::now();
    xtd_internal::EnsureSodium();
    if (!input_) throw ValidationError("could not read XTD package " + path.string());
    std::error_code error;
    const std::uint64_t file_length = std::filesystem::file_size(path, error);
    if (error || file_length < kHeaderSize) {
      throw ValidationError("could not determine XTD package size " + path.string());
    }
    header_raw_ = ReadAt(input_, 0, kHeaderSize, "XTD header");
    header_ = ParseHeader(header_raw_, file_length);
    metadata_raw_ = ReadAt(input_, header_.metadata_offset,
                           static_cast<std::size_t>(header_.metadata_length),
                           "XTD metadata");
    status_.metadata =
        ParseMetadata(metadata_raw_, header_, &status_.constituents);
    const std::uint64_t index_size = header_.index_count * kIndexEntrySize;
    index_raw_ = ReadAt(input_, header_.index_offset,
                        static_cast<std::size_t>(index_size), "XTD index");
    index_ = ParseIndex(index_raw_, header_);
    Authenticate();

    status_.path = path;
    status_.bbox = header_.bbox;
    std::copy(header_.package_id.begin(), header_.package_id.end(),
              status_.package_id.begin());
    status_.nx = header_.nx;
    status_.ny = header_.ny;
    status_.tile_width = header_.tile_width;
    status_.tile_height = header_.tile_height;
    status_.tile_columns = header_.tile_columns;
    status_.tile_rows = header_.tile_rows;
    status_.file_length = header_.file_length;
    status_.lon_u0 = header_.lon_u0;
    status_.lon_step = header_.lon_step;
    status_.lat_u0 = header_.lat_u0;
    status_.lat_step = header_.lat_step;
    status_.lon_v0 = header_.lon_v0;
    status_.lat_v0 = header_.lat_v0;
    status_.validation_ms = std::chrono::duration<double, std::milli>(
                                Clock::now() - validation_started)
                                .count();
    status_.authenticated = true;
  }

  const XtdStatus& status() const noexcept { return status_; }

  XtdStatistics statistics() const noexcept { return statistics_; }

  void ResetStatistics() noexcept { statistics_ = {}; }

  void ClearTileCache() noexcept {
    cache_.clear();
    lru_.clear();
    cache_bytes_ = 0;
  }

  XtdStatistics VerifyAllTiles() {
    for (std::uint32_t tile_id = 0; tile_id < index_.size(); ++tile_id) {
      (void)GetTile(tile_id);
    }
    return statistics_;
  }

  TpxoCache LoadRegion(const RegularGrid& output_grid) {
    ValidateOutputGrid(output_grid);
    const double load_before = statistics_.load_ms;
    const auto started = Clock::now();
    const std::size_t points = output_grid.size();
    const std::size_t constituents = status_.constituents.size();
    const double missing = std::numeric_limits<double>::quiet_NaN();
    TpxoCache result;
    result.metadata = status_.metadata;
    result.metadata["source_format"] = "XTD";
    result.metadata["velocity_units"] = "cm/s";
    if (!result.metadata.isMember("corrections")) {
      result.metadata["corrections"] = "ATLAS";
    }
    result.grid = output_grid;
    result.constituents = status_.constituents;
    result.u_cm_s.assign(constituents * points, {missing, missing});
    result.v_cm_s.assign(constituents * points, {missing, missing});
    const auto [lon_min, lon_max] =
        std::minmax_element(output_grid.longitudes.begin(), output_grid.longitudes.end());
    const auto [lat_min, lat_max] =
        std::minmax_element(output_grid.latitudes.begin(), output_grid.latitudes.end());
    result.bbox = {*lon_min, *lat_min, *lon_max, *lat_max};

    for (std::size_t y = 0; y < output_grid.ny(); ++y) {
      for (std::size_t x = 0; x < output_grid.nx(); ++x) {
        const std::size_t point = y * output_grid.nx() + x;
        InterpolateComponent(output_grid.longitudes[x], output_grid.latitudes[y],
                             true, point, points, &result.u_cm_s);
        InterpolateComponent(output_grid.longitudes[x], output_grid.latitudes[y],
                             false, point, points, &result.v_cm_s);
      }
    }
    const double total_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    const double tile_load_ms = statistics_.load_ms - load_before;
    statistics_.interpolation_ms += std::max(0.0, total_ms - tile_load_ms);
    return result;
  }

 private:
  struct CacheItem {
    std::shared_ptr<Tile> tile;
    std::list<std::uint32_t>::iterator position;
    std::uint64_t bytes{};
  };

  void Authenticate() {
    auto root = xtd_internal::RuntimeRootKey();
    const auto wrapper_aad = WrapperAad(header_);
    unsigned long long content_length = 0;
    const int unwrap_result = crypto_aead_xchacha20poly1305_ietf_decrypt(
        content_key_.data(), &content_length, nullptr,
        header_.wrapped_content_key.data(), header_.wrapped_content_key.size(),
        wrapper_aad.data(), wrapper_aad.size(), header_.wrap_nonce.data(),
        root.data());
    sodium_memzero(root.data(), root.size());
    if (unwrap_result != 0 || content_length != content_key_.size()) {
      sodium_memzero(content_key_.data(), content_key_.size());
      Invalid("content key authentication failed");
    }

    Bytes authenticated_header = header_raw_;
    std::fill(authenticated_header.begin() + 272,
              authenticated_header.begin() + 304, 0);
    crypto_generichash_state state;
    std::array<unsigned char, crypto_generichash_BYTES> actual{};
    if (crypto_generichash_init(&state, content_key_.data(), content_key_.size(),
                                actual.size()) != 0 ||
        crypto_generichash_update(&state, authenticated_header.data(),
                                  authenticated_header.size()) != 0 ||
        crypto_generichash_update(&state, metadata_raw_.data(),
                                  metadata_raw_.size()) != 0 ||
        crypto_generichash_update(&state, index_raw_.data(), index_raw_.size()) !=
            0 ||
        crypto_generichash_final(&state, actual.data(), actual.size()) != 0) {
      sodium_memzero(content_key_.data(), content_key_.size());
      Invalid("could not authenticate public region");
    }
    if (sodium_memcmp(actual.data(), header_.public_region_mac.data(),
                      actual.size()) != 0) {
      sodium_memzero(actual.data(), actual.size());
      sodium_memzero(content_key_.data(), content_key_.size());
      Invalid("public-region authentication failed");
    }
    sodium_memzero(actual.data(), actual.size());
  }

  std::shared_ptr<Tile> GetTile(std::uint32_t tile_id) {
    const auto found = cache_.find(tile_id);
    if (found != cache_.end()) {
      ++statistics_.cache_hits;
      lru_.splice(lru_.begin(), lru_, found->second.position);
      return found->second.tile;
    }
    const auto started = Clock::now();
    const IndexEntry& entry = index_[tile_id];
    std::shared_ptr<Tile> tile;
    if ((entry.flags & kEmptyTile) != 0) {
      tile = std::make_shared<Tile>();
      tile->empty = true;
      tile->width = entry.width;
      tile->height = entry.height;
    } else {
      Bytes encrypted = ReadAt(input_, entry.offset, entry.encrypted_length,
                               "encrypted XTD tile");
      Bytes compressed(entry.encrypted_length -
                       crypto_aead_xchacha20poly1305_ietf_ABYTES);
      const auto aad = TileAad(header_, entry);
      unsigned long long compressed_length = 0;
      if (crypto_aead_xchacha20poly1305_ietf_decrypt(
              compressed.data(), &compressed_length, nullptr, encrypted.data(),
              encrypted.size(), aad.data(), aad.size(), entry.nonce.data(),
              content_key_.data()) != 0 ||
          compressed_length != compressed.size()) {
        Invalid("tile " + std::to_string(tile_id) +
                " authentication failed");
      }
      Bytes plaintext(entry.plaintext_length);
      const std::size_t decompressed =
          ZSTD_decompress(plaintext.data(), plaintext.size(), compressed.data(),
                          compressed.size());
      if (ZSTD_isError(decompressed) || decompressed != plaintext.size()) {
        Invalid("tile " + std::to_string(tile_id) +
                " zstd decompression failed: " + ZSTD_getErrorName(decompressed));
      }
      statistics_.encrypted_bytes += encrypted.size();
      statistics_.compressed_bytes += compressed.size();
      statistics_.decompressed_bytes += plaintext.size();
      tile = ParseTile(plaintext, header_, entry);
    }
    ++statistics_.tiles_loaded;
    statistics_.load_ms +=
        std::chrono::duration<double, std::milli>(Clock::now() - started).count();

    if (capacity_ != 0 && max_cache_bytes_ != 0) {
      const std::uint64_t bytes = tile->ByteSize();
      if (bytes > max_cache_bytes_) return tile;
      lru_.push_front(tile_id);
      cache_.emplace(tile_id, CacheItem{tile, lru_.begin(), bytes});
      cache_bytes_ += bytes;
      while (cache_.size() > capacity_ || cache_bytes_ > max_cache_bytes_) {
        const std::uint32_t evicted = lru_.back();
        cache_bytes_ -= cache_.at(evicted).bytes;
        cache_.erase(evicted);
        lru_.pop_back();
      }
      statistics_.peak_cache_bytes =
          std::max(statistics_.peak_cache_bytes, cache_bytes_);
    }
    return tile;
  }

  struct Cell {
    std::shared_ptr<Tile> tile;
    std::size_t local{};
  };

  Cell GetCell(std::uint32_t x, std::uint32_t y) {
    const std::uint32_t tx = x / header_.tile_width;
    const std::uint32_t ty = y / header_.tile_height;
    const std::uint32_t tile_id = ty * header_.tile_columns + tx;
    auto tile = GetTile(tile_id);
    const std::size_t local_x = x - tx * header_.tile_width;
    const std::size_t local_y = y - ty * header_.tile_height;
    return {std::move(tile), local_y * index_[tile_id].width + local_x};
  }

  void InterpolateComponent(double longitude, double latitude, bool u_component,
                            std::size_t point, std::size_t points,
                            std::vector<std::complex<double>>* output) {
    const double lon_origin = u_component ? header_.lon_u0 : header_.lon_v0;
    const double lat_origin = u_component ? header_.lat_u0 : header_.lat_v0;
    const Bracket x =
        LongitudeBracket(longitude, lon_origin, header_.lon_step, header_.nx);
    const Bracket y =
        LatitudeBracket(latitude, lat_origin, header_.lat_step, header_.ny);
    if (!x.valid || !y.valid) return;

    const std::array<Cell, 4> cells{
        GetCell(x.first, y.first), GetCell(x.second, y.first),
        GetCell(x.first, y.second), GetCell(x.second, y.second)};
    const std::array<double, 4> weights{
        (1.0 - x.fraction) * (1.0 - y.fraction),
        x.fraction * (1.0 - y.fraction),
        (1.0 - x.fraction) * y.fraction, x.fraction * y.fraction};
    const std::size_t real_component = u_component ? 0 : 2;
    for (std::size_t constituent = 0;
         constituent < status_.constituents.size(); ++constituent) {
      bool valid = true;
      double real = 0.0;
      double imaginary = 0.0;
      for (std::size_t corner = 0; corner < cells.size(); ++corner) {
        if (!cells[corner].tile->Valid(u_component, cells[corner].local)) {
          valid = false;
          break;
        }
        real += weights[corner] * cells[corner].tile->Value(
                                      constituent, real_component,
                                      cells[corner].local);
        imaginary += weights[corner] * cells[corner].tile->Value(
                                           constituent, real_component + 1,
                                           cells[corner].local);
      }
      if (valid) {
        (*output)[constituent * points + point] = {real, imaginary};
      }
    }
  }

  std::filesystem::path path_;
  std::size_t capacity_{};
  std::uint64_t max_cache_bytes_{};
  std::ifstream input_;
  Header header_;
  Bytes header_raw_, metadata_raw_, index_raw_;
  std::vector<IndexEntry> index_;
  SecureKey content_key_;
  XtdStatus status_;
  XtdStatistics statistics_;
  std::list<std::uint32_t> lru_;
  std::unordered_map<std::uint32_t, CacheItem> cache_;
  std::uint64_t cache_bytes_{};
};

XtdReader::XtdReader(const std::filesystem::path& path,
                     XtdReaderOptions options)
    : impl_(std::make_unique<Impl>(path, options)) {}

XtdReader::~XtdReader() = default;
XtdReader::XtdReader(XtdReader&&) noexcept = default;
XtdReader& XtdReader::operator=(XtdReader&&) noexcept = default;

const XtdStatus& XtdReader::status() const noexcept { return impl_->status(); }

const Json::Value& XtdReader::metadata() const noexcept {
  return impl_->status().metadata;
}

XtdStatistics XtdReader::statistics() const noexcept {
  return impl_->statistics();
}

void XtdReader::ResetStatistics() noexcept { impl_->ResetStatistics(); }

void XtdReader::ClearTileCache() noexcept { impl_->ClearTileCache(); }

TpxoCache XtdReader::LoadRegion(const RegularGrid& output_grid) {
  return impl_->LoadRegion(output_grid);
}

XtdStatistics XtdReader::VerifyAllTiles() { return impl_->VerifyAllTiles(); }

TpxoCache LoadXtdCache(const std::filesystem::path& path,
                       const RegularGrid& output_grid,
                       XtdReaderOptions options) {
  XtdReader reader(path, options);
  return reader.LoadRegion(output_grid);
}

Json::Value InspectXtd(const std::filesystem::path& path) {
  XtdReader reader(path);
  const auto& status = reader.status();
  Json::Value result = status.metadata;
  result["input_package"] = status.path.string();
  result["grid_size"]["nx"] = status.nx;
  result["grid_size"]["ny"] = status.ny;
  result["tile_size"]["width"] = status.tile_width;
  result["tile_size"]["height"] = status.tile_height;
  result["tile_grid"]["columns"] = status.tile_columns;
  result["tile_grid"]["rows"] = status.tile_rows;
  result["file_length"] = Json::UInt64(status.file_length);
  result["authenticated"] = status.authenticated;
  result["coverage"]["west"] = status.bbox.west;
  result["coverage"]["south"] = status.bbox.south;
  result["coverage"]["east"] = status.bbox.east;
  result["coverage"]["north"] = status.bbox.north;
  result["constituent_count"] =
      Json::UInt64(status.constituents.size());
  result["nominal_resolution_degrees"] = status.lon_step;
  result["validation_ms"] = status.validation_ms;
  result["valid"] = true;
  return result;
}

Json::Value VerifyXtd(const std::filesystem::path& path,
                      XtdReaderOptions options) {
  XtdReader reader(path, options);
  const auto statistics = reader.VerifyAllTiles();
  Json::Value result = InspectXtd(path);
  result["verified_all_tiles"] = true;
  result["verification"]["tiles_loaded"] =
      Json::UInt64(statistics.tiles_loaded);
  result["verification"]["encrypted_bytes"] =
      Json::UInt64(statistics.encrypted_bytes);
  result["verification"]["compressed_bytes"] =
      Json::UInt64(statistics.compressed_bytes);
  result["verification"]["decompressed_bytes"] =
      Json::UInt64(statistics.decompressed_bytes);
  result["verification"]["peak_cache_bytes"] =
      Json::UInt64(statistics.peak_cache_bytes);
  result["verification"]["load_ms"] = statistics.load_ms;
  return result;
}

}  // namespace environmental_grib
