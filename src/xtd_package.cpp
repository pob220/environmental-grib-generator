#include "environmental_grib/xtd_package.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <numbers>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include <sodium.h>
#include <zstd.h>

#include "environmental_grib/error.h"
#include "xtd_crypto_internal.h"

namespace environmental_grib {
namespace {

using Bytes = std::vector<unsigned char>;
using Clock = std::chrono::steady_clock;

constexpr std::array<unsigned char, 8> kV1Magic{'X', 'G', 'R', 'I',
                                                'B', 'X', '1', 0};
constexpr std::array<unsigned char, 8> kV2Magic{'X', 'G', 'R', 'I',
                                                'B', 'X', '2', 0};
constexpr std::uint32_t kV2Version = 2;
constexpr std::uint32_t kHeaderSize = 512;
constexpr std::uint32_t kComponentSize = 256;
constexpr std::uint32_t kIndexSize = 64;
constexpr std::uint32_t kEndian = 0x01020304;
constexpr std::uint32_t kTypeTide = 1;
constexpr std::uint32_t kTypeResidual = 2;
constexpr std::uint32_t kTypeUncertainty = 3;
constexpr std::uint32_t kRepEmbeddedV1 = 1;
constexpr std::uint32_t kRepHarmonic2 = 2;
constexpr std::uint32_t kRepMonthly12 = 3;
constexpr std::uint32_t kRepUncertainty1 = 4;
constexpr std::uint32_t kFlagRequired = 1;
constexpr std::uint32_t kFlagNested = 8;
constexpr std::uint32_t kEmptyTile = 1;
constexpr std::uint32_t kZstd = 1;
constexpr std::uint64_t kMaxMetadata = 16ULL * 1024 * 1024;
constexpr std::uint64_t kMaxComponents = 64;
constexpr std::uint64_t kMaxTiles = 1'000'000;
constexpr std::uint64_t kMaxTilePlaintext = 64ULL * 1024 * 1024;

[[noreturn]] void Invalid(const std::string& message) {
  throw ValidationError("invalid XTD v2 package: " + message);
}

template <typename T>
T ReadLe(const unsigned char* p) {
  static_assert(std::is_integral_v<T>);
  using U = std::make_unsigned_t<T>;
  U value{};
  for (std::size_t i = 0; i < sizeof(T); ++i)
    value |= static_cast<U>(p[i]) << (8 * i);
  return static_cast<T>(value);
}

float ReadF32(const unsigned char* p) {
  return std::bit_cast<float>(ReadLe<std::uint32_t>(p));
}
std::uint64_t Add(std::uint64_t a, std::uint64_t b, std::string_view what) {
  if (b > std::numeric_limits<std::uint64_t>::max() - a)
    Invalid(std::string(what) + " overflows");
  return a + b;
}
std::uint64_t Mul(std::uint64_t a, std::uint64_t b, std::string_view what) {
  if (a && b > std::numeric_limits<std::uint64_t>::max() / a)
    Invalid(std::string(what) + " overflows");
  return a * b;
}

Json::Value ParseJson(const Bytes& raw, const std::string& what) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  builder["failIfExtra"] = true;
  builder["rejectDupKeys"] = true;
  Json::Value value;
  std::string errors;
  std::istringstream stream(
      std::string(reinterpret_cast<const char*>(raw.data()), raw.size()));
  if (!Json::parseFromStream(builder, stream, &value, &errors) ||
      !value.isObject())
    Invalid(what + " is not one JSON object");
  return value;
}

std::string Hex(const unsigned char* data, std::size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result(size * 2, '0');
  for (std::size_t i = 0; i < size; ++i) {
    result[2 * i] = kHex[data[i] >> 4];
    result[2 * i + 1] = kHex[data[i] & 15];
  }
  return result;
}

struct SecureKey {
  std::array<unsigned char, 32> bytes{};
  ~SecureKey() { sodium_memzero(bytes.data(), bytes.size()); }
};

struct OuterHeader {
  std::uint32_t flags{}, component_count{};
  std::uint64_t metadata_offset{}, metadata_length{}, directory_offset{};
  std::uint64_t directory_length{}, payload_offset{}, file_length{};
  std::array<unsigned char, 16> package_id{};
  std::array<unsigned char, 24> wrap_nonce{};
  std::array<unsigned char, 48> wrapped_key{};
  std::array<unsigned char, 32> public_mac{};
};

OuterHeader ParseOuterHeader(const Bytes& raw, std::uint64_t actual) {
  if (raw.size() != kHeaderSize ||
      !std::equal(kV2Magic.begin(), kV2Magic.end(), raw.begin()))
    Invalid("bad magic or truncated header");
  if (ReadLe<std::uint32_t>(&raw[8]) != kV2Version)
    Invalid("unsupported version");
  if (ReadLe<std::uint32_t>(&raw[12]) != kHeaderSize)
    Invalid("header size is not 512 bytes");
  if (ReadLe<std::uint32_t>(&raw[16]) != kEndian) Invalid("bad endian marker");
  OuterHeader h;
  h.flags = ReadLe<std::uint32_t>(&raw[20]);
  h.component_count = ReadLe<std::uint32_t>(&raw[24]);
  if (ReadLe<std::uint32_t>(&raw[28]) != kComponentSize)
    Invalid("component entry size is not 256 bytes");
  h.metadata_offset = ReadLe<std::uint64_t>(&raw[32]);
  h.metadata_length = ReadLe<std::uint64_t>(&raw[40]);
  h.directory_offset = ReadLe<std::uint64_t>(&raw[48]);
  h.directory_length = ReadLe<std::uint64_t>(&raw[56]);
  h.payload_offset = ReadLe<std::uint64_t>(&raw[64]);
  h.file_length = ReadLe<std::uint64_t>(&raw[72]);
  std::copy_n(raw.begin() + 80, 16, h.package_id.begin());
  std::copy_n(raw.begin() + 96, 24, h.wrap_nonce.begin());
  std::copy_n(raw.begin() + 120, 48, h.wrapped_key.begin());
  std::copy_n(raw.begin() + 168, 32, h.public_mac.begin());
  if (h.flags != 0) Invalid("unsupported outer flags");
  if (h.component_count == 0 || h.component_count > kMaxComponents)
    Invalid("component count outside supported limits");
  if (h.file_length != actual) Invalid("declared file length mismatch");
  if (h.metadata_offset != kHeaderSize || h.metadata_length == 0 ||
      h.metadata_length > kMaxMetadata)
    Invalid("metadata range is invalid");
  if (h.directory_offset !=
          Add(h.metadata_offset, h.metadata_length, "metadata") ||
      h.directory_length !=
          Mul(h.component_count, kComponentSize, "directory") ||
      h.payload_offset !=
          Add(h.directory_offset, h.directory_length, "directory") ||
      h.payload_offset > h.file_length)
    Invalid("public ranges are not contiguous");
  if (!std::all_of(raw.begin() + 200, raw.end(),
                   [](unsigned char v) { return v == 0; }))
    Invalid("reserved header bytes are nonzero");
  return h;
}

struct Component {
  std::uint32_t type{}, representation{}, version{}, flags{}, grid_id{};
  std::uint32_t index_entry_size{}, tile_count{};
  std::array<unsigned char, 16> id{};
  std::uint64_t metadata_offset{}, metadata_length{}, index_offset{};
  std::uint64_t index_length{}, payload_offset{}, payload_length{};
  std::uint64_t nested_offset{}, nested_length{};
  std::array<unsigned char, 32> logical_hash{}, stored_hash{}, source_hash{};
};

std::vector<Component> ParseDirectory(const Bytes& raw, const OuterHeader& h) {
  std::vector<Component> result;
  std::uint64_t previous_end = h.payload_offset;
  std::uint32_t previous_type = 0;
  for (std::uint32_t i = 0; i < h.component_count; ++i) {
    const auto* p = &raw[static_cast<std::size_t>(i) * kComponentSize];
    Component c;
    c.type = ReadLe<std::uint32_t>(p);
    c.representation = ReadLe<std::uint32_t>(p + 4);
    c.version = ReadLe<std::uint32_t>(p + 8);
    c.flags = ReadLe<std::uint32_t>(p + 12);
    std::copy_n(p + 16, 16, c.id.begin());
    c.grid_id = ReadLe<std::uint32_t>(p + 32);
    c.index_entry_size = ReadLe<std::uint32_t>(p + 36);
    c.tile_count = ReadLe<std::uint32_t>(p + 40);
    if (ReadLe<std::uint32_t>(p + 44) != 0)
      Invalid("component reserved field is nonzero");
    c.metadata_offset = ReadLe<std::uint64_t>(p + 48);
    c.metadata_length = ReadLe<std::uint64_t>(p + 56);
    c.index_offset = ReadLe<std::uint64_t>(p + 64);
    c.index_length = ReadLe<std::uint64_t>(p + 72);
    c.payload_offset = ReadLe<std::uint64_t>(p + 80);
    c.payload_length = ReadLe<std::uint64_t>(p + 88);
    c.nested_offset = ReadLe<std::uint64_t>(p + 96);
    c.nested_length = ReadLe<std::uint64_t>(p + 104);
    std::copy_n(p + 112, 32, c.logical_hash.begin());
    std::copy_n(p + 144, 32, c.stored_hash.begin());
    std::copy_n(p + 176, 32, c.source_hash.begin());
    if (!std::all_of(p + 208, p + 256, [](unsigned char v) { return v == 0; }))
      Invalid("component reserved bytes are nonzero");
    if (c.type <= previous_type) Invalid("component directory is not ordered");
    previous_type = c.type;
    if (c.version != 1 || (c.flags & ~(kFlagRequired | 2U | 4U | kFlagNested)))
      Invalid("unsupported component version or flags");
    if (c.type == kTypeTide && c.representation == kRepEmbeddedV1) {
      if (!(c.flags & kFlagNested) || c.grid_id || c.index_entry_size ||
          c.tile_count || c.metadata_offset || c.metadata_length ||
          c.index_offset || c.index_length || c.payload_offset ||
          c.payload_length || c.nested_length < kHeaderSize)
        Invalid("embedded v1 component fields are inconsistent");
      const auto end = Add(c.nested_offset, c.nested_length, "nested package");
      if (c.nested_offset < previous_end || end > h.file_length)
        Invalid("nested component is overlapping or out of range");
      previous_end = end;
    } else {
      if (c.flags & kFlagNested || c.grid_id == 0 ||
          c.index_entry_size != kIndexSize || c.tile_count == 0 ||
          c.tile_count > kMaxTiles || c.nested_offset || c.nested_length ||
          c.metadata_length == 0 || c.metadata_length > kMaxMetadata ||
          c.index_length != Mul(c.tile_count, kIndexSize, "component index"))
        Invalid("tiled component fields are inconsistent");
      if (c.metadata_offset < previous_end ||
          c.index_offset !=
              Add(c.metadata_offset, c.metadata_length, "component metadata") ||
          c.payload_offset !=
              Add(c.index_offset, c.index_length, "component index") ||
          Add(c.payload_offset, c.payload_length, "component payload") >
              h.file_length)
        Invalid("component ranges overlap or are not contiguous");
      previous_end =
          Add(c.payload_offset, c.payload_length, "component payload");
    }
    result.push_back(c);
  }
  if (previous_end != h.file_length)
    Invalid("unreferenced or truncated trailing bytes");
  return result;
}

struct TileIndex {
  std::uint32_t id{}, tx{}, ty{}, flags{}, compression{};
  std::uint16_t width{}, height{};
  std::uint64_t offset{};
  std::uint32_t encrypted_length{}, plaintext_length{};
  std::array<unsigned char, 24> nonce{};
  std::array<unsigned char, 40> aad{};
};

struct GridInfo {
  std::uint32_t nx{}, ny{}, tile_width{}, tile_height{}, columns{}, rows{};
  double lon0{}, lat0{}, lon_step{}, lat_step{};
};

GridInfo ParseGrid(const Json::Value& metadata) {
  const auto& g = metadata["grid"];
  if (!g.isObject()) Invalid("component grid metadata is missing");
  GridInfo x;
  x.nx = g["nx"].asUInt();
  x.ny = g["ny"].asUInt();
  x.tile_width = g["tile_width"].asUInt();
  x.tile_height = g["tile_height"].asUInt();
  x.lon0 = g["lon0"].asDouble();
  x.lat0 = g["lat0"].asDouble();
  x.lon_step = g["lon_step"].asDouble();
  x.lat_step = g["lat_step"].asDouble();
  if (x.nx < 2 || x.nx > 20000 || x.ny < 2 || x.ny > 10000 || !x.tile_width ||
      x.tile_width > 256 || !x.tile_height || x.tile_height > 256 ||
      !std::isfinite(x.lon0) || !std::isfinite(x.lat0) ||
      !std::isfinite(x.lon_step) || !std::isfinite(x.lat_step) ||
      x.lon_step <= 0 || x.lat_step <= 0)
    Invalid("component grid metadata is invalid");
  x.columns = (x.nx + x.tile_width - 1) / x.tile_width;
  x.rows = (x.ny + x.tile_height - 1) / x.tile_height;
  return x;
}

std::vector<TileIndex> ParseTileIndex(const Bytes& raw, const Component& c,
                                      const GridInfo& g, const OuterHeader& h) {
  if (c.tile_count != Mul(g.columns, g.rows, "tile grid"))
    Invalid("component tile count does not match grid");
  std::vector<TileIndex> out;
  out.reserve(c.tile_count);
  std::uint64_t previous = c.payload_offset;
  for (std::uint32_t i = 0; i < c.tile_count; ++i) {
    const auto* p = &raw[static_cast<std::size_t>(i) * 64];
    TileIndex e;
    e.id = ReadLe<std::uint32_t>(p);
    e.tx = ReadLe<std::uint32_t>(p + 4);
    e.ty = ReadLe<std::uint32_t>(p + 8);
    e.width = ReadLe<std::uint16_t>(p + 12);
    e.height = ReadLe<std::uint16_t>(p + 14);
    e.flags = ReadLe<std::uint32_t>(p + 16);
    e.compression = ReadLe<std::uint32_t>(p + 20);
    e.offset = ReadLe<std::uint64_t>(p + 24);
    e.encrypted_length = ReadLe<std::uint32_t>(p + 32);
    e.plaintext_length = ReadLe<std::uint32_t>(p + 36);
    std::copy_n(p + 40, 24, e.nonce.begin());
    std::copy_n(p, 40, e.aad.begin());
    const auto tx = i % g.columns, ty = i / g.columns;
    const auto width = std::min(g.tile_width, g.nx - tx * g.tile_width);
    const auto height = std::min(g.tile_height, g.ny - ty * g.tile_height);
    if (e.id != i || e.tx != tx || e.ty != ty || e.width != width ||
        e.height != height || (e.flags & ~kEmptyTile))
      Invalid("component tile index is inconsistent");
    if (e.flags & kEmptyTile) {
      if (e.compression || e.offset || e.encrypted_length ||
          e.plaintext_length ||
          !std::all_of(e.nonce.begin(), e.nonce.end(),
                       [](unsigned char v) { return v == 0; }))
        Invalid("empty component tile has payload");
    } else {
      const auto end = Add(e.offset, e.encrypted_length, "tile payload");
      if (e.compression != kZstd || e.encrypted_length < 16 ||
          e.plaintext_length < 32 || e.plaintext_length > kMaxTilePlaintext ||
          e.offset < previous || e.offset < c.payload_offset ||
          end > Add(c.payload_offset, c.payload_length, "component payload"))
        Invalid("component tile payload range is invalid");
      previous = end;
    }
    out.push_back(e);
  }
  if (previous != Add(c.payload_offset, c.payload_length, "component payload"))
    Invalid("component payload has unindexed bytes");
  (void)h;
  return out;
}

SecureKey DeriveComponentKey(const SecureKey& package,
                             const Component& component) {
  static constexpr std::array<unsigned char, 12> domain{
      'X', 'T', 'D', '2', '-', 'C', 'O', 'M', 'P', '-', 'v', '1'};
  SecureKey key;
  crypto_generichash_state state;
  if (crypto_generichash_init(&state, package.bytes.data(),
                              package.bytes.size(), key.bytes.size()) ||
      crypto_generichash_update(&state, domain.data(), domain.size()) ||
      crypto_generichash_update(&state, component.id.data(),
                                component.id.size()) ||
      crypto_generichash_final(&state, key.bytes.data(), key.bytes.size()))
    Invalid("component key derivation failed");
  return key;
}

struct Bracket {
  std::uint32_t a{}, b{};
  double f{};
  bool valid{};
};
Bracket LatBracket(double v, double o, double s, std::uint32_t n) {
  double p = (v - o) / s;
  if (p < -1e-10 || p > n - 1 + 1e-10) return {};
  p = std::clamp(p, 0.0, static_cast<double>(n - 1));
  auto a = static_cast<std::uint32_t>(std::floor(p));
  if (a == n - 1) return {n - 2, n - 1, 1, true};
  return {a, a + 1, p - a, true};
}
Bracket LonBracket(double v, double o, double s, std::uint32_t n) {
  const double span = s * n;
  if (std::abs(span - 360.0) <= std::max(1e-8, std::abs(span) * 1e-10)) {
    double p = std::fmod((v - o) / s, static_cast<double>(n));
    if (p < 0) p += n;
    auto a = static_cast<std::uint32_t>(std::floor(p));
    return {a, (a + 1) % n, p - a, true};
  }
  double x = v;
  const double last = o + s * (n - 1);
  x += 360 * std::round((o - x) / 360);
  if (x < o) x += 360;
  if (x > last && x - 360 >= o) x -= 360;
  return LatBracket(x, o, s, n);
}

struct ResidualTile {
  bool empty{};
  std::uint16_t width{}, height{};
  std::vector<unsigned char> mask;
  std::vector<float> scales;
  std::vector<std::int16_t> values;
  std::uint64_t ByteSize() const {
    return sizeof(*this) + mask.capacity() + scales.capacity() * 4 +
           values.capacity() * 2;
  }
};

std::shared_ptr<ResidualTile> ParseResidualTile(const Bytes& raw,
                                                const TileIndex& e,
                                                std::uint32_t expected_fields) {
  if (raw.size() < 32 ||
      !std::equal(raw.begin(), raw.begin() + 4,
                  std::array<unsigned char, 4>{'X', 'C', 'R', '1'}.begin()))
    Invalid("residual tile magic is invalid");
  const auto version = ReadLe<std::uint16_t>(&raw[4]);
  const auto width = ReadLe<std::uint16_t>(&raw[8]),
             height = ReadLe<std::uint16_t>(&raw[10]);
  const auto fields = ReadLe<std::uint16_t>(&raw[12]);
  if (ReadLe<std::uint16_t>(&raw[14]) != 0)
    Invalid("residual tile reserved field is nonzero");
  const auto cells = ReadLe<std::uint32_t>(&raw[16]),
             mask_bytes = ReadLe<std::uint32_t>(&raw[20]);
  const auto encoding = ReadLe<std::uint32_t>(&raw[24]),
             declared = ReadLe<std::uint32_t>(&raw[28]);
  const auto expected_cells = static_cast<std::uint32_t>(e.width) * e.height;
  const auto expected_mask = (expected_cells + 7) / 8;
  const auto expected =
      Add(Add(32, expected_mask, "residual mask"),
          Add(Mul(fields, 4, "residual scales"),
              Mul(Mul(fields, cells, "residual values"), 2, "residual values"),
              "residual fields"),
          "residual tile");
  if (version != 1 || width != e.width || height != e.height ||
      fields != expected_fields || cells != expected_cells ||
      mask_bytes != expected_mask || encoding > 1 || declared != raw.size() ||
      expected != raw.size())
    Invalid("residual tile header is inconsistent");
  auto tile = std::make_shared<ResidualTile>();
  tile->width = width;
  tile->height = height;
  std::size_t pos = 32;
  tile->mask.assign(raw.begin() + pos, raw.begin() + pos + mask_bytes);
  pos += mask_bytes;
  if (cells % 8 && (tile->mask.back() & ~((1U << (cells % 8)) - 1U)))
    Invalid("residual mask padding is nonzero");
  for (std::uint32_t i = 0; i < fields; ++i, pos += 4) {
    float s = ReadF32(&raw[pos]);
    if (!std::isfinite(s) || s < 0) Invalid("residual scale invalid");
    tile->scales.push_back(s);
  }
  tile->values.reserve(static_cast<std::size_t>(fields) * cells);
  for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(fields) * cells;
       ++i, pos += 2)
    tile->values.push_back(ReadLe<std::int16_t>(&raw[pos]));
  if (encoding == 1)
    for (std::uint32_t field = 0; field < fields; ++field) {
      const std::size_t base = static_cast<std::size_t>(field) * cells;
      for (std::size_t y = 1; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x) {
          auto q = base + y * width + x;
          const auto decoded = static_cast<std::uint16_t>(
              static_cast<std::uint16_t>(tile->values[q]) +
              static_cast<std::uint16_t>(tile->values[q - width]));
          tile->values[q] = std::bit_cast<std::int16_t>(decoded);
        }
      for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 1; x < width; ++x) {
          auto q = base + y * width + x;
          const auto decoded = static_cast<std::uint16_t>(
              static_cast<std::uint16_t>(tile->values[q]) +
              static_cast<std::uint16_t>(tile->values[q - 1]));
          tile->values[q] = std::bit_cast<std::int16_t>(decoded);
        }
    }
  return tile;
}

double YearPhase(TimePoint time) {
  const auto day = std::chrono::floor<std::chrono::days>(time);
  const std::chrono::year_month_day ymd{day};
  const auto start = std::chrono::sys_days{ymd.year() / 1 / 1};
  const auto end =
      std::chrono::sys_days{(ymd.year() + std::chrono::years{1}) / 1 / 1};
  const auto elapsed = std::chrono::duration<double>(time - start).count();
  const auto length = std::chrono::duration<double>(end - start).count();
  return 2.0 * std::numbers::pi * elapsed / length;
}

struct MonthlyInterpolation {
  std::size_t first{};
  std::size_t second{};
  double fraction{};
};

MonthlyInterpolation MonthCentreInterpolation(TimePoint time) {
  const auto day = std::chrono::floor<std::chrono::days>(time);
  const std::chrono::year_month_day ymd{day};
  const auto year = ymd.year();
  const unsigned month = static_cast<unsigned>(ymd.month());
  const auto month_start = std::chrono::sys_days{year / ymd.month() / 1};
  const auto next_start =
      month == 12
          ? std::chrono::sys_days{(year + std::chrono::years{1}) / 1 / 1}
          : std::chrono::sys_days{year / std::chrono::month{month + 1} / 1};
  const auto centre = TimePoint{month_start.time_since_epoch()} +
                      std::chrono::duration_cast<TimePoint::duration>(
                          std::chrono::duration_cast<std::chrono::seconds>(
                              next_start - month_start) /
                          2);

  TimePoint first_centre;
  TimePoint second_centre;
  std::size_t first;
  std::size_t second;
  if (time >= centre) {
    first = month - 1;
    second = month % 12;
    first_centre = centre;
    const auto following_start =
        month >= 11
            ? std::chrono::sys_days{(year + std::chrono::years{1}) /
                                    std::chrono::month{(month + 2) % 12} / 1}
            : std::chrono::sys_days{year / std::chrono::month{month + 2} / 1};
    second_centre = TimePoint{next_start.time_since_epoch()} +
                    std::chrono::duration_cast<TimePoint::duration>(
                        std::chrono::duration_cast<std::chrono::seconds>(
                            following_start - next_start) /
                        2);
  } else {
    second = month - 1;
    first = (month + 10) % 12;
    second_centre = centre;
    const auto previous_start =
        month == 1
            ? std::chrono::sys_days{(year - std::chrono::years{1}) / 12 / 1}
            : std::chrono::sys_days{year / std::chrono::month{month - 1} / 1};
    first_centre = TimePoint{previous_start.time_since_epoch()} +
                   std::chrono::duration_cast<TimePoint::duration>(
                       std::chrono::duration_cast<std::chrono::seconds>(
                           month_start - previous_start) /
                       2);
  }
  const double numerator =
      std::chrono::duration<double>(time - first_centre).count();
  const double denominator =
      std::chrono::duration<double>(second_centre - first_centre).count();
  return {first, second, std::clamp(numerator / denominator, 0.0, 1.0)};
}

class ResidualReader {
public:
  ResidualReader(std::shared_ptr<RandomAccessSource> source,
                 const OuterHeader& outer, const Component& component,
                 const SecureKey& package, XtdReaderOptions options)
      : source_(std::move(source)),
        outer_(outer),
        component_(component),
        key_(DeriveComponentKey(package, component)),
        capacity_(options.tile_cache_capacity),
        max_bytes_(options.tile_cache_max_bytes) {
    metadata_raw_ =
        source_->Read(component.metadata_offset, component.metadata_length,
                      "residual metadata");
    metadata_ = ParseJson(metadata_raw_, "residual metadata");
    grid_ = ParseGrid(metadata_);
    fields_ = component.representation == kRepHarmonic2   ? 10
              : component.representation == kRepMonthly12 ? 24
                                                          : 0;
    if (!fields_) Invalid("unsupported residual representation");
    index_raw_ = source_->Read(component.index_offset, component.index_length,
                               "residual index");
    index_ = ParseTileIndex(index_raw_, component, grid_, outer_);
  }
  const Json::Value& metadata() const { return metadata_; }
  XtdStatistics stats() const { return stats_; }
  void Clear() {
    cache_.clear();
    lru_.clear();
    cache_bytes_ = 0;
  }
  XtdStatistics Verify() {
    for (std::uint32_t i = 0; i < index_.size(); ++i) (void)Get(i);
    return stats_;
  }

  std::vector<CurrentGrid> Evaluate(const RegularGrid& output,
                                    const std::vector<TimePoint>& times) {
    const auto started = Clock::now();
    const auto load_before = stats_.load_ms;
    const auto points = output.size();
    std::vector<std::vector<double>> sampled(
        fields_,
        std::vector<double>(points, std::numeric_limits<double>::quiet_NaN()));
    std::vector<std::uint8_t> mask(points, 0);
    for (std::size_t y = 0; y < output.ny(); ++y)
      for (std::size_t x = 0; x < output.nx(); ++x) {
        const auto p = y * output.nx() + x;
        const auto bx = LonBracket(output.longitudes[x], grid_.lon0,
                                   grid_.lon_step, grid_.nx);
        const auto by = LatBracket(output.latitudes[y], grid_.lat0,
                                   grid_.lat_step, grid_.ny);
        if (!bx.valid || !by.valid) continue;
        const std::array<std::uint32_t, 4> xs{bx.a, bx.b, bx.a, bx.b},
            ys{by.a, by.a, by.b, by.b};
        const std::array<double, 4> w{(1 - bx.f) * (1 - by.f),
                                      bx.f * (1 - by.f), (1 - bx.f) * by.f,
                                      bx.f * by.f};
        bool valid = true;
        std::array<std::shared_ptr<ResidualTile>, 4> tiles;
        std::array<std::size_t, 4> locals{};
        for (int k = 0; k < 4; ++k) {
          auto tx = xs[k] / grid_.tile_width, ty = ys[k] / grid_.tile_height,
               id = ty * grid_.columns + tx;
          tiles[k] = Get(id);
          locals[k] = (ys[k] - ty * grid_.tile_height) * index_[id].width +
                      (xs[k] - tx * grid_.tile_width);
          if (tiles[k]->empty ||
              !(tiles[k]->mask[locals[k] / 8] & (1U << (locals[k] % 8))))
            valid = false;
        }
        if (!valid) continue;
        mask[p] = 1;
        for (std::uint32_t f = 0; f < fields_; ++f) {
          double v = 0;
          for (int k = 0; k < 4; ++k) {
            const auto cells =
                static_cast<std::size_t>(tiles[k]->width) * tiles[k]->height;
            v += w[k] * tiles[k]->values[f * cells + locals[k]] *
                 tiles[k]->scales[f];
          }
          sampled[f][p] = v;
        }
      }
    std::vector<CurrentGrid> result;
    result.reserve(times.size());
    for (const auto time : times) {
      CurrentGrid g;
      g.time = time;
      g.grid = output;
      g.mask = mask;
      g.u_mps.resize(points, std::numeric_limits<double>::quiet_NaN());
      g.v_mps.resize(points, std::numeric_limits<double>::quiet_NaN());
      const double phi = YearPhase(time);
      for (std::size_t p = 0; p < points; ++p)
        if (mask[p]) {
          if (fields_ == 10) {
            g.u_mps[p] = sampled[0][p] + sampled[1][p] * std::cos(phi) +
                         sampled[2][p] * std::sin(phi) +
                         sampled[3][p] * std::cos(2 * phi) +
                         sampled[4][p] * std::sin(2 * phi);
            g.v_mps[p] = sampled[5][p] + sampled[6][p] * std::cos(phi) +
                         sampled[7][p] * std::sin(phi) +
                         sampled[8][p] * std::cos(2 * phi) +
                         sampled[9][p] * std::sin(2 * phi);
          } else {
            const auto interpolation = MonthCentreInterpolation(time);
            g.u_mps[p] =
                (1 - interpolation.fraction) * sampled[interpolation.first][p] +
                interpolation.fraction * sampled[interpolation.second][p];
            g.v_mps[p] =
                (1 - interpolation.fraction) *
                    sampled[12 + interpolation.first][p] +
                interpolation.fraction * sampled[12 + interpolation.second][p];
          }
        }
      result.push_back(std::move(g));
    }
    const auto total =
        std::chrono::duration<double, std::milli>(Clock::now() - started)
            .count();
    stats_.interpolation_ms +=
        std::max(0.0, total - (stats_.load_ms - load_before));
    return result;
  }

private:
  struct Item {
    std::shared_ptr<ResidualTile> tile;
    std::list<std::uint32_t>::iterator pos;
    std::uint64_t bytes;
  };
  std::shared_ptr<ResidualTile> Get(std::uint32_t id) {
    auto found = cache_.find(id);
    if (found != cache_.end()) {
      ++stats_.cache_hits;
      lru_.splice(lru_.begin(), lru_, found->second.pos);
      return found->second.tile;
    }
    const auto begin = Clock::now();
    const auto& e = index_.at(id);
    std::shared_ptr<ResidualTile> tile;
    if (e.flags & kEmptyTile) {
      tile = std::make_shared<ResidualTile>();
      tile->empty = true;
      tile->width = e.width;
      tile->height = e.height;
    } else {
      auto encrypted = source_->Read(e.offset, e.encrypted_length,
                                     "encrypted residual tile");
      Bytes compressed(e.encrypted_length -
                       crypto_aead_xchacha20poly1305_ietf_ABYTES);
      std::array<unsigned char, 81> aad{};
      std::copy_n("XTD2-TILE", 9, aad.begin());
      std::copy(outer_.package_id.begin(), outer_.package_id.end(),
                aad.begin() + 9);
      std::copy(component_.id.begin(), component_.id.end(), aad.begin() + 25);
      std::copy(e.aad.begin(), e.aad.end(), aad.begin() + 41);
      unsigned long long length = 0;
      if (crypto_aead_xchacha20poly1305_ietf_decrypt(
              compressed.data(), &length, nullptr, encrypted.data(),
              encrypted.size(), aad.data(), aad.size(), e.nonce.data(),
              key_.bytes.data()) ||
          length != compressed.size())
        Invalid("residual tile " + std::to_string(id) +
                " authentication failed");
      Bytes plain(e.plaintext_length);
      const auto n = ZSTD_decompress(plain.data(), plain.size(),
                                     compressed.data(), compressed.size());
      if (ZSTD_isError(n) || n != plain.size())
        Invalid("residual tile " + std::to_string(id) +
                " decompression failed");
      stats_.encrypted_bytes += encrypted.size();
      stats_.compressed_bytes += compressed.size();
      stats_.decompressed_bytes += plain.size();
      tile = ParseResidualTile(plain, e, fields_);
    }
    ++stats_.tiles_loaded;
    stats_.load_ms +=
        std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    const auto bytes = tile->ByteSize();
    if (capacity_ && max_bytes_ && bytes <= max_bytes_) {
      lru_.push_front(id);
      cache_.emplace(id, Item{tile, lru_.begin(), bytes});
      cache_bytes_ += bytes;
      while (cache_.size() > capacity_ || cache_bytes_ > max_bytes_) {
        auto old = lru_.back();
        cache_bytes_ -= cache_.at(old).bytes;
        cache_.erase(old);
        lru_.pop_back();
      }
      stats_.peak_cache_bytes = std::max(stats_.peak_cache_bytes, cache_bytes_);
    }
    return tile;
  }
  std::shared_ptr<RandomAccessSource> source_;
  OuterHeader outer_;
  Component component_;
  SecureKey key_;
  Json::Value metadata_;
  GridInfo grid_;
  std::uint32_t fields_{};
  std::vector<TileIndex> index_;
  Bytes metadata_raw_, index_raw_;
  std::size_t capacity_{};
  std::uint64_t max_bytes_{}, cache_bytes_{};
  XtdStatistics stats_;
  std::list<std::uint32_t> lru_;
  std::unordered_map<std::uint32_t, Item> cache_;
};

void ValidateUncertaintyTile(const Bytes& raw, const TileIndex& entry) {
  constexpr std::uint32_t kContinuousFields = 4;
  constexpr std::uint32_t kQualityBytesPerCell = 7;
  if (raw.size() < 32 ||
      !std::equal(raw.begin(), raw.begin() + 4,
                  std::array<unsigned char, 4>{'X', 'C', 'U', '1'}.begin())) {
    Invalid("uncertainty tile magic is invalid");
  }
  const auto version = ReadLe<std::uint16_t>(&raw[4]);
  const auto width = ReadLe<std::uint16_t>(&raw[8]);
  const auto height = ReadLe<std::uint16_t>(&raw[10]);
  const auto fields = ReadLe<std::uint16_t>(&raw[12]);
  const auto cells = ReadLe<std::uint32_t>(&raw[16]);
  const auto mask_bytes = ReadLe<std::uint32_t>(&raw[20]);
  const auto encoding = ReadLe<std::uint32_t>(&raw[24]);
  const auto declared = ReadLe<std::uint32_t>(&raw[28]);
  const auto expected_cells = static_cast<std::uint32_t>(entry.width) *
                              static_cast<std::uint32_t>(entry.height);
  const auto expected_mask = (expected_cells + 7) / 8;
  const auto expected = Add(Add(Add(32, expected_mask, "uncertainty mask"),
                                Mul(kContinuousFields, 4, "uncertainty scales"),
                                "uncertainty continuous header"),
                            Add(Mul(Mul(kContinuousFields, expected_cells,
                                        "uncertainty continuous values"),
                                    2, "uncertainty continuous bytes"),
                                Mul(kQualityBytesPerCell, expected_cells,
                                    "uncertainty quality bytes"),
                                "uncertainty fields"),
                            "uncertainty tile");
  if (version != 1 || ReadLe<std::uint16_t>(&raw[6]) != 0 ||
      width != entry.width || height != entry.height ||
      fields != kContinuousFields || ReadLe<std::uint16_t>(&raw[14]) != 0 ||
      cells != expected_cells || mask_bytes != expected_mask || encoding > 1 ||
      declared != raw.size() || expected != raw.size()) {
    Invalid("uncertainty tile header is inconsistent");
  }
  if (cells % 8 != 0 &&
      (raw[32 + mask_bytes - 1] & ~((1U << (cells % 8)) - 1U))) {
    Invalid("uncertainty mask padding is nonzero");
  }
  std::size_t cursor = 32 + mask_bytes;
  for (std::uint32_t field = 0; field < kContinuousFields; ++field) {
    const float scale = ReadF32(&raw[cursor]);
    if (!std::isfinite(scale) || scale < 0.0F) {
      Invalid("uncertainty scale is invalid");
    }
    cursor += 4;
  }
}

class TiledComponentVerifier {
public:
  TiledComponentVerifier(std::shared_ptr<RandomAccessSource> source,
                         const OuterHeader& outer, const Component& component,
                         const SecureKey& package_key)
      : source_(std::move(source)),
        outer_(outer),
        component_(component),
        key_(DeriveComponentKey(package_key, component)) {
    metadata_raw_ =
        source_->Read(component.metadata_offset, component.metadata_length,
                      "uncertainty metadata");
    metadata_ = ParseJson(metadata_raw_, "uncertainty metadata");
    grid_ = ParseGrid(metadata_);
    index_raw_ = source_->Read(component.index_offset, component.index_length,
                               "uncertainty index");
    index_ = ParseTileIndex(index_raw_, component, grid_, outer_);
  }

  XtdStatistics Verify() {
    for (const auto& entry : index_) {
      if ((entry.flags & kEmptyTile) != 0) continue;
      const auto begin = Clock::now();
      auto encrypted = source_->Read(entry.offset, entry.encrypted_length,
                                     "encrypted uncertainty tile");
      Bytes compressed(entry.encrypted_length -
                       crypto_aead_xchacha20poly1305_ietf_ABYTES);
      std::array<unsigned char, 81> aad{};
      std::copy_n("XTD2-TILE", 9, aad.begin());
      std::copy(outer_.package_id.begin(), outer_.package_id.end(),
                aad.begin() + 9);
      std::copy(component_.id.begin(), component_.id.end(), aad.begin() + 25);
      std::copy(entry.aad.begin(), entry.aad.end(), aad.begin() + 41);
      unsigned long long decrypted_length = 0;
      if (crypto_aead_xchacha20poly1305_ietf_decrypt(
              compressed.data(), &decrypted_length, nullptr, encrypted.data(),
              encrypted.size(), aad.data(), aad.size(), entry.nonce.data(),
              key_.bytes.data()) != 0 ||
          decrypted_length != compressed.size()) {
        Invalid("uncertainty tile " + std::to_string(entry.id) +
                " authentication failed");
      }
      Bytes plaintext(entry.plaintext_length);
      const auto size = ZSTD_decompress(plaintext.data(), plaintext.size(),
                                        compressed.data(), compressed.size());
      if (ZSTD_isError(size) || size != plaintext.size()) {
        Invalid("uncertainty tile " + std::to_string(entry.id) +
                " decompression failed");
      }
      ValidateUncertaintyTile(plaintext, entry);
      ++stats_.tiles_loaded;
      stats_.encrypted_bytes += encrypted.size();
      stats_.compressed_bytes += compressed.size();
      stats_.decompressed_bytes += plaintext.size();
      stats_.load_ms +=
          std::chrono::duration<double, std::milli>(Clock::now() - begin)
              .count();
      sodium_memzero(plaintext.data(), plaintext.size());
      sodium_memzero(compressed.data(), compressed.size());
    }
    return stats_;
  }

  XtdStatistics statistics() const noexcept { return stats_; }

private:
  std::shared_ptr<RandomAccessSource> source_;
  OuterHeader outer_;
  Component component_;
  SecureKey key_;
  Json::Value metadata_;
  GridInfo grid_;
  Bytes metadata_raw_;
  Bytes index_raw_;
  std::vector<TileIndex> index_;
  XtdStatistics stats_;
};

std::array<unsigned char, 32> HashRange(
    const std::shared_ptr<RandomAccessSource>& source, std::uint64_t offset,
    std::uint64_t length, const std::string& description) {
  crypto_hash_sha256_state state;
  crypto_hash_sha256_init(&state);
  constexpr std::size_t kChunk = 8 * 1024 * 1024;
  std::uint64_t consumed = 0;
  while (consumed < length) {
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(kChunk, length - consumed));
    const auto bytes = source->Read(offset + consumed, count, description);
    crypto_hash_sha256_update(&state, bytes.data(), bytes.size());
    consumed += count;
  }
  std::array<unsigned char, 32> digest{};
  crypto_hash_sha256_final(&state, digest.data());
  return digest;
}

}  // namespace

std::string OfflineCurrentModeId(OfflineCurrentMode mode) {
  return mode == OfflineCurrentMode::kAstronomicalTideOnly
             ? "tide-only"
             : "tide-expected-seasonal";
}
OfflineCurrentMode ParseOfflineCurrentMode(const std::string& value) {
  if (value.empty() || value == "tide-only")
    return OfflineCurrentMode::kAstronomicalTideOnly;
  if (value == "tide-expected-seasonal")
    return OfflineCurrentMode::kTideAndExpectedSeasonalCirculation;
  throw ValidationError("unsupported offline current mode: " + value);
}

class XtdPackageReader::Impl {
public:
  Impl(const std::filesystem::path& path, XtdReaderOptions options)
      : source_(OpenFileSource(path)), options_(options) {
    status_.path = path;
    const auto begin = Clock::now();
    auto magic = source_->Read(0, 8, "XTD package magic");
    if (std::equal(kV1Magic.begin(), kV1Magic.end(), magic.begin())) {
      status_.format_version = 1;
      tide_ = std::make_unique<XtdReader>(source_, options);
      status_.metadata = tide_->metadata();
      status_.authenticated = tide_->status().authenticated;
      status_.package_id = Hex(tide_->status().package_id.data(),
                               tide_->status().package_id.size());
      status_.tide_available = true;
    } else if (std::equal(kV2Magic.begin(), kV2Magic.end(), magic.begin())) {
      OpenV2();
    } else
      throw ValidationError("invalid XTD package: bad magic");
    status_.validation_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
  }
  void OpenV2() {
    xtd_internal::EnsureSodium();
    header_raw_ = source_->Read(0, kHeaderSize, "XTD v2 header");
    outer_ = ParseOuterHeader(header_raw_, source_->size());
    metadata_raw_ = source_->Read(outer_.metadata_offset,
                                  outer_.metadata_length, "XTD v2 metadata");
    directory_raw_ =
        source_->Read(outer_.directory_offset, outer_.directory_length,
                      "XTD v2 component directory");
    UnwrapAndAuthenticate();
    components_ = ParseDirectory(directory_raw_, outer_);
    status_.format_version = 2;
    status_.metadata = ParseJson(metadata_raw_, "outer metadata");
    status_.authenticated = true;
    status_.package_id =
        Hex(outer_.package_id.data(), outer_.package_id.size());
    status_.package_hash = status_.metadata.get("package_hash", "").asString();
    status_.parent_package_hash =
        status_.metadata.get("parent_package_hash", "").asString();
    const Component* residual = nullptr;
    const Component* uncertainty = nullptr;
    for (const auto& c : components_) {
      if (c.type == kTypeTide) {
        if (c.representation != kRepEmbeddedV1)
          Invalid("unsupported deterministic tide representation");
        if (c.logical_hash != c.stored_hash)
          Invalid("embedded deterministic tide hashes disagree");
        if (!status_.parent_package_hash.empty() &&
            Hex(c.stored_hash.data(), c.stored_hash.size()) !=
                status_.parent_package_hash)
          Invalid(
              "parent package hash does not match deterministic tide "
              "component");
        auto nested =
            MakeBoundedSource(source_, c.nested_offset, c.nested_length,
                              "embedded deterministic tide");
        tide_ = std::make_unique<XtdReader>(nested, options_);
        status_.tide_available = true;
      } else if (c.type == kTypeResidual) {
        if (c.representation != kRepHarmonic2 &&
            c.representation != kRepMonthly12)
          Invalid("unsupported climatological residual representation");
        residual = &c;
        status_.residual_representation =
            c.representation == kRepHarmonic2 ? "harmonic2" : "monthly12";
      } else if (c.type == kTypeUncertainty) {
        if (c.representation != kRepUncertainty1)
          Invalid("unsupported climatological uncertainty representation");
        uncertainty = &c;
      } else if (c.flags & kFlagRequired)
        Invalid("unknown required component");
    }
    if (!tide_) Invalid("deterministic tide component missing");
    if (residual) {
      residual_ = std::make_unique<ResidualReader>(source_, outer_, *residual,
                                                   package_key_, options_);
      status_.climatology_available = true;
    }
    if (uncertainty) {
      uncertainty_ = std::make_unique<TiledComponentVerifier>(
          source_, outer_, *uncertainty, package_key_);
      status_.uncertainty_available = true;
    }
  }
  void UnwrapAndAuthenticate() {
    auto root = xtd_internal::RuntimeRootKey();
    std::array<unsigned char, 28> aad{};
    std::copy(kV2Magic.begin(), kV2Magic.end(), aad.begin());
    aad[8] = 2;
    std::copy(outer_.package_id.begin(), outer_.package_id.end(),
              aad.begin() + 12);
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            package_key_.bytes.data(), &length, nullptr,
            outer_.wrapped_key.data(), outer_.wrapped_key.size(), aad.data(),
            aad.size(), outer_.wrap_nonce.data(), root.data()) ||
        length != package_key_.bytes.size()) {
      sodium_memzero(root.data(), root.size());
      Invalid("package key authentication failed");
    }
    sodium_memzero(root.data(), root.size());
    auto normalized = header_raw_;
    std::fill(normalized.begin() + 168, normalized.begin() + 200, 0);
    std::array<unsigned char, 32> actual{};
    crypto_generichash_state state;
    if (crypto_generichash_init(&state, package_key_.bytes.data(),
                                package_key_.bytes.size(), actual.size()) ||
        crypto_generichash_update(&state, normalized.data(),
                                  normalized.size()) ||
        crypto_generichash_update(&state, metadata_raw_.data(),
                                  metadata_raw_.size()) ||
        crypto_generichash_update(&state, directory_raw_.data(),
                                  directory_raw_.size()) ||
        crypto_generichash_final(&state, actual.data(), actual.size()) ||
        sodium_memcmp(actual.data(), outer_.public_mac.data(), actual.size()))
      Invalid("public-region authentication failed");
    sodium_memzero(actual.data(), actual.size());
  }
  std::vector<CurrentGrid> Predict(const RegularGrid& grid,
                                   const std::vector<TimePoint>& times,
                                   OfflineCurrentMode mode, bool infer) {
    if (!tide_)
      throw ValidationError("XTD package has no deterministic tide component");
    auto cache = tide_->LoadRegion(grid);
    auto result = PredictTpxoCache(cache, times, infer);
    if (mode == OfflineCurrentMode::kAstronomicalTideOnly) return result;
    if (!residual_)
      throw ValidationError(
          "selected XTD package does not contain expected seasonal "
          "circulation");
    auto clim = residual_->Evaluate(grid, times);
    if (clim.size() != result.size())
      throw ValidationError("XTD residual timestamp count mismatch");
    for (std::size_t t = 0; t < result.size(); ++t)
      for (std::size_t p = 0; p < grid.size(); ++p) {
        const bool valid = (!result[t].has_mask() || result[t].mask[p]) &&
                           (!clim[t].has_mask() || clim[t].mask[p]) &&
                           std::isfinite(result[t].u_mps[p]) &&
                           std::isfinite(result[t].v_mps[p]) &&
                           std::isfinite(clim[t].u_mps[p]) &&
                           std::isfinite(clim[t].v_mps[p]);
        if (!valid) {
          if (result[t].mask.empty()) result[t].mask.assign(grid.size(), 1);
          result[t].mask[p] = 0;
          result[t].u_mps[p] = result[t].v_mps[p] =
              std::numeric_limits<double>::quiet_NaN();
        } else {
          result[t].u_mps[p] += clim[t].u_mps[p];
          result[t].v_mps[p] += clim[t].v_mps[p];
        }
      }
    return result;
  }
  Json::Value Verify() {
    Json::Value v(Json::objectValue);
    v["format_version"] = status_.format_version;
    if (status_.format_version == 2) {
      for (const auto& c : components_) {
        const auto offset =
            c.type == kTypeTide ? c.nested_offset : c.metadata_offset;
        const auto length = c.type == kTypeTide
                                ? c.nested_length
                                : Add(Add(c.metadata_length, c.index_length,
                                          "component hash range"),
                                      c.payload_length, "component hash range");
        const auto actual =
            HashRange(source_, offset, length, "component stored hash");
        if (actual != c.stored_hash)
          Invalid("stored hash mismatch for component " +
                  std::to_string(c.type));
      }
      v["stored_hashes_valid"] = true;
    }
    v["tide"]["tiles_loaded"] =
        Json::UInt64(tide_->VerifyAllTiles().tiles_loaded);
    if (residual_)
      v["climatological_residual"]["tiles_loaded"] =
          Json::UInt64(residual_->Verify().tiles_loaded);
    if (uncertainty_)
      v["climatological_uncertainty"]["tiles_loaded"] =
          Json::UInt64(uncertainty_->Verify().tiles_loaded);
    v["valid"] = true;
    return v;
  }
  XtdPackageStatistics stats() const {
    XtdPackageStatistics s;
    if (tide_) s.tide = tide_->statistics();
    if (residual_) s.residual = residual_->stats();
    if (uncertainty_) s.uncertainty = uncertainty_->statistics();
    s.outer_bytes_read = source_->bytes_read();
    return s;
  }
  void Clear() {
    if (tide_) tide_->ClearTileCache();
    if (residual_) residual_->Clear();
  }
  XtdPackageStatus status_;
  std::shared_ptr<RandomAccessSource> source_;
  XtdReaderOptions options_;
  Bytes header_raw_, metadata_raw_, directory_raw_;
  OuterHeader outer_;
  std::vector<Component> components_;
  SecureKey package_key_;
  std::unique_ptr<XtdReader> tide_;
  std::unique_ptr<ResidualReader> residual_;
  std::unique_ptr<TiledComponentVerifier> uncertainty_;
};

XtdPackageReader::XtdPackageReader(const std::filesystem::path& p,
                                   XtdReaderOptions o)
    : impl_(std::make_unique<Impl>(p, o)) {}
XtdPackageReader::~XtdPackageReader() = default;
XtdPackageReader::XtdPackageReader(XtdPackageReader&&) noexcept = default;
XtdPackageReader& XtdPackageReader::operator=(XtdPackageReader&&) noexcept =
    default;
const XtdPackageStatus& XtdPackageReader::status() const noexcept {
  return impl_->status_;
}
XtdPackageStatistics XtdPackageReader::statistics() const noexcept {
  return impl_->stats();
}
void XtdPackageReader::ClearTileCaches() noexcept { impl_->Clear(); }
std::vector<CurrentGrid> XtdPackageReader::Predict(
    const RegularGrid& g, const std::vector<TimePoint>& t, OfflineCurrentMode m,
    bool i) {
  return impl_->Predict(g, t, m, i);
}
Json::Value XtdPackageReader::VerifyAllComponents() { return impl_->Verify(); }
Json::Value InspectXtdPackage(const std::filesystem::path& p) {
  XtdPackageReader r(p);
  Json::Value v = r.status().metadata;
  v["valid"] = true;
  v["authenticated"] = r.status().authenticated;
  v["format_version"] = r.status().format_version;
  v["capabilities"]["astronomical_tide_only"] = r.status().tide_available;
  v["capabilities"]["tide_expected_seasonal"] =
      r.status().climatology_available;
  v["capabilities"]["uncertainty"] = r.status().uncertainty_available;
  v["residual_representation"] = r.status().residual_representation;
  v["package_id"] = r.status().package_id;
  v["validation_ms"] = r.status().validation_ms;
  return v;
}
Json::Value VerifyXtdPackage(const std::filesystem::path& p,
                             XtdReaderOptions o) {
  XtdPackageReader r(p, o);
  auto v = r.VerifyAllComponents();
  v["inspection"] = InspectXtdPackage(p);
  return v;
}

}  // namespace environmental_grib
