#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <json/json.h>

namespace environmental_grib::test {

struct XtdFixtureOptions {
  std::uint32_t nx{8};
  std::uint32_t ny{5};
  std::uint32_t tile_width{4};
  std::uint32_t tile_height{3};
  double lon_u0{0.0};
  double lon_step{45.0};
  double lat_u0{-2.0};
  double lat_step{1.0};
  double lon_v0{0.0};
  double lat_v0{-2.0};
  double west{0.0};
  double south{-2.0};
  double east{360.0};
  double north{2.0};
  float quantization_scale{0.01F};
  int zstd_level{3};
  std::uint32_t coefficient_bits{12};
  std::vector<std::string> constituents{"m2", "s2"};
  std::vector<std::uint32_t> empty_tiles;
  Json::Value metadata;

  // component is 0=Ure, 1=Uim, 2=Vre, 3=Vim.
  std::function<double(std::size_t constituent, std::size_t component,
                       std::uint32_t x, std::uint32_t y)>
      value;
  std::function<bool(bool u_component, std::uint32_t x, std::uint32_t y)>
      valid;
};

void WriteXtdFixture(const std::filesystem::path& path,
                     const XtdFixtureOptions& options = {});
void CorruptXtdFixtureByte(const std::filesystem::path& path,
                           std::uint64_t offset,
                           unsigned char xor_mask = 1);
void TruncateXtdFixture(const std::filesystem::path& path,
                        std::uint64_t length);

}  // namespace environmental_grib::test
