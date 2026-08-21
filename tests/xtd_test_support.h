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
  std::function<bool(bool u_component, std::uint32_t x, std::uint32_t y)> valid;
};

enum class XtdV2ResidualRepresentation {
  kHarmonic2,
  kMonthly12,
};

struct XtdV2FixtureOptions {
  XtdFixtureOptions tide;
  Json::Value outer_metadata{Json::objectValue};
  bool randomize_crypto{false};
  bool include_tide{true};
  bool include_residual{true};
  bool include_uncertainty{true};
  XtdV2ResidualRepresentation representation{
      XtdV2ResidualRepresentation::kHarmonic2};
  std::uint32_t tile_width{2};
  std::uint32_t tile_height{2};
  float quantization_scale{0.001F};
  std::function<double(std::size_t field, std::uint32_t x, std::uint32_t y)>
      residual_value;
  std::function<bool(std::uint32_t x, std::uint32_t y)> valid;
  bool include_height{false};
  std::vector<std::string> height_constituents{"m2", "s2", "k1", "o1",
                                                "n2", "p1", "k2", "q1"};
  double height_reference_level_m{5.0};
  std::string height_datum_id{"test-chart-datum"};
  std::string height_datum_name{"Synthetic chart datum"};
  Json::Value height_metadata{Json::objectValue};
  std::function<double(std::size_t field, std::uint32_t x, std::uint32_t y)>
      height_value;
  bool include_height_quality{false};
  Json::Value height_quality_metadata{Json::objectValue};
  std::function<double(std::size_t field, std::uint32_t x, std::uint32_t y)>
      height_quality_value;
  std::function<std::uint8_t(std::uint32_t x, std::uint32_t y)>
      height_support_class;
  std::function<std::uint16_t(std::uint32_t x, std::uint32_t y)>
      height_observation_count;
  bool include_vertical_datum{false};
  std::string vertical_datum_source_id{"model-mean-sea-level"};
  std::string vertical_datum_source_name{"Model mean sea level"};
  std::string vertical_datum_target_id{"chart-datum"};
  std::string vertical_datum_target_name{"Local Chart Datum"};
  std::string vertical_datum_epoch{"2026"};
  Json::Value vertical_datum_metadata{Json::objectValue};
  std::function<double(std::size_t field, std::uint32_t x, std::uint32_t y)>
      vertical_datum_value;
  std::function<bool(std::uint32_t x, std::uint32_t y)>
      vertical_datum_valid;
  std::function<std::uint8_t(std::uint32_t x, std::uint32_t y)>
      vertical_datum_realization_class;
  std::function<std::uint8_t(std::uint32_t x, std::uint32_t y)>
      vertical_datum_support_class;
  std::function<std::uint16_t(std::uint32_t x, std::uint32_t y)>
      vertical_datum_station_count;
};

void WriteXtdFixture(const std::filesystem::path& path,
                     const XtdFixtureOptions& options = {});
void WriteXtdV2Fixture(const std::filesystem::path& path,
                       const XtdV2FixtureOptions& options = {});
void CorruptXtdFixtureByte(const std::filesystem::path& path,
                           std::uint64_t offset, unsigned char xor_mask = 1);
void TruncateXtdFixture(const std::filesystem::path& path,
                        std::uint64_t length);

}  // namespace environmental_grib::test
