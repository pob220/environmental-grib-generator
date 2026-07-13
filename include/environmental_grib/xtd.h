#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <json/json.h>

#include "environmental_grib/tpxo.h"
#include "environmental_grib/random_access.h"

namespace environmental_grib {

struct XtdReaderOptions {
  std::size_t tile_cache_capacity{32};
  std::uint64_t tile_cache_max_bytes{64ULL * 1024 * 1024};
};

struct XtdStatistics {
  std::uint64_t tiles_loaded{};
  std::uint64_t cache_hits{};
  std::uint64_t encrypted_bytes{};
  std::uint64_t compressed_bytes{};
  std::uint64_t decompressed_bytes{};
  std::uint64_t peak_cache_bytes{};
  double load_ms{};
  double interpolation_ms{};
};

struct XtdStatus {
  std::filesystem::path path;
  Json::Value metadata;
  BoundingBox bbox;
  std::vector<std::string> constituents;
  std::array<std::uint8_t, 16> package_id{};
  std::uint32_t nx{};
  std::uint32_t ny{};
  std::uint32_t tile_width{};
  std::uint32_t tile_height{};
  std::uint32_t tile_columns{};
  std::uint32_t tile_rows{};
  std::uint64_t file_length{};
  double lon_u0{};
  double lon_step{};
  double lat_u0{};
  double lat_step{};
  double lon_v0{};
  double lat_v0{};
  double validation_ms{};
  bool authenticated{};
};

class XtdReader {
 public:
  explicit XtdReader(const std::filesystem::path& path,
                     XtdReaderOptions options = {});
  explicit XtdReader(std::shared_ptr<RandomAccessSource> source,
                     XtdReaderOptions options = {});
  ~XtdReader();

  XtdReader(XtdReader&&) noexcept;
  XtdReader& operator=(XtdReader&&) noexcept;
  XtdReader(const XtdReader&) = delete;
  XtdReader& operator=(const XtdReader&) = delete;

  [[nodiscard]] const XtdStatus& status() const noexcept;
  [[nodiscard]] const Json::Value& metadata() const noexcept;
  [[nodiscard]] XtdStatistics statistics() const noexcept;
  void ResetStatistics() noexcept;
  void ClearTileCache() noexcept;
  XtdStatistics VerifyAllTiles();

  TpxoCache LoadRegion(const RegularGrid& output_grid);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

TpxoCache LoadXtdCache(const std::filesystem::path& path,
                       const RegularGrid& output_grid,
                       XtdReaderOptions options = {});
Json::Value InspectXtd(const std::filesystem::path& path);
Json::Value VerifyXtd(const std::filesystem::path& path,
                      XtdReaderOptions options = {});

}  // namespace environmental_grib
