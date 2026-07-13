#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "environmental_grib/error.h"
#include "environmental_grib/environment.h"
#include "environmental_grib/geo.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/model.h"
#include "environmental_grib/xtd.h"
#include "xtd_test_support.h"

namespace eg = environmental_grib;
namespace test = environmental_grib::test;

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

template <typename Callable>
void CheckRejected(Callable&& callable, const std::string& message) {
  try {
    callable();
    Check(false, message + " was accepted");
  } catch (const eg::ValidationError&) {
  }
}

std::filesystem::path TempPath(const std::string& name) {
  return std::filesystem::temp_directory_path() /
         ("environmental-grib-xtd-" + name + ".xtd");
}

eg::RegularGrid PointGrid(double longitude, double latitude) {
  eg::RegularGrid grid;
  grid.longitudes = {longitude};
  grid.latitudes = {latitude};
  return grid;
}

std::uint64_t ReadLe64At(const std::filesystem::path& path,
                         std::uint64_t offset) {
  std::ifstream input(path, std::ios::binary);
  input.seekg(static_cast<std::streamoff>(offset));
  unsigned char bytes[8]{};
  input.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
  if (!input) throw std::runtime_error("could not read XTD fixture offset");
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < sizeof(bytes); ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
  }
  return value;
}

void TestValidPackageAndInterpolation() {
  const auto path = TempPath("valid");
  test::WriteXtdFixture(path);
  eg::XtdReader reader(path, {.tile_cache_capacity = 2});
  Check(reader.status().authenticated, "valid package is authenticated");
  Check(reader.status().nx == 8 && reader.status().ny == 5,
        "valid package dimensions are exposed");
  Check(reader.status().constituents.size() == 2,
        "constituent metadata is exposed");

  const auto cache = reader.LoadRegion(PointGrid(67.5, -0.5));
  Check(cache.u_cm_s.size() == 2 && cache.v_cm_s.size() == 2,
        "one-point region contains both constituents");
  Check(std::isfinite(cache.u_cm_s[0].real()),
        "U coefficient is bilinearly interpolated");
  Check(std::isfinite(cache.v_cm_s[0].imag()),
        "V coefficient is bilinearly interpolated");
  Check(reader.statistics().tiles_loaded > 0,
        "regional query loads required tiles");
  std::filesystem::remove(path);
}

void TestTileBoundaryAndCacheBound() {
  const auto path = TempPath("boundary");
  test::XtdFixtureOptions options;
  options.tile_width = 2;
  options.tile_height = 2;
  test::WriteXtdFixture(path, options);
  eg::XtdReader reader(path, {.tile_cache_capacity = 2,
                              .tile_cache_max_bytes = 4'000});
  const auto cache = reader.LoadRegion(PointGrid(90.0, 0.0));
  Check(std::isfinite(cache.u_cm_s[0].real()),
        "interpolation across tile boundaries is valid");
  Check(reader.statistics().peak_cache_bytes > 0,
        "bounded cache reports peak memory");
  Check(reader.statistics().peak_cache_bytes <= 4'000,
        "bounded cache remains within its byte limit");
  reader.ClearTileCache();
  std::filesystem::remove(path);
}

void TestSupportedCoefficientEncodings() {
  const auto int12_path = TempPath("int12");
  const auto int16_path = TempPath("int16");
  test::XtdFixtureOptions int12;
  int12.coefficient_bits = 12;
  test::XtdFixtureOptions int16 = int12;
  int16.coefficient_bits = 16;
  test::WriteXtdFixture(int12_path, int12);
  test::WriteXtdFixture(int16_path, int16);
  eg::XtdReader int12_reader(int12_path);
  eg::XtdReader int16_reader(int16_path);
  const auto int12_cache = int12_reader.LoadRegion(PointGrid(67.5, -0.5));
  const auto int16_cache = int16_reader.LoadRegion(PointGrid(67.5, -0.5));
  Check(std::abs(int12_cache.u_cm_s[0].real() -
                 int16_cache.u_cm_s[0].real()) < 1e-9,
        "12-bit and 16-bit tile decoders reconstruct matching coefficients");
  Check(std::abs(int12_cache.v_cm_s[1].imag() -
                 int16_cache.v_cm_s[1].imag()) < 1e-9,
        "both precision encodings preserve component ordering");
  std::filesystem::remove(int12_path);
  std::filesystem::remove(int16_path);
}

void TestLongitudeSeam() {
  const auto path = TempPath("seam");
  test::WriteXtdFixture(path);
  eg::XtdReader reader(path);
  const auto west = reader.LoadRegion(PointGrid(-0.25, 0.0));
  const auto east = reader.LoadRegion(PointGrid(359.75, 0.0));
  Check(std::abs(west.u_cm_s[0].real() - east.u_cm_s[0].real()) < 1e-9,
        "longitude seam wraps deterministically");
  std::filesystem::remove(path);
}

void TestMaskAndOutsideCoverage() {
  const auto path = TempPath("mask");
  test::XtdFixtureOptions options;
  options.valid = [](bool, std::uint32_t x, std::uint32_t y) {
    return !(x == 1 && y == 1);
  };
  test::WriteXtdFixture(path, options);
  eg::XtdReader reader(path);
  const auto masked = reader.LoadRegion(PointGrid(45.0, -1.0));
  Check(!std::isfinite(masked.u_cm_s[0].real()),
        "masked interpolation corner remains missing");
  const auto outside = reader.LoadRegion(PointGrid(45.0, 91.0));
  Check(!std::isfinite(outside.u_cm_s[0].real()),
        "coordinate outside coverage remains missing");
  std::filesystem::remove(path);
}

void TestMalformedPackages() {
  {
    const auto path = TempPath("magic");
    test::WriteXtdFixture(path);
    test::CorruptXtdFixtureByte(path, 0);
    CheckRejected([&] { eg::XtdReader reader(path); }, "corrupt magic");
    std::filesystem::remove(path);
  }
  {
    const auto path = TempPath("version");
    test::WriteXtdFixture(path);
    test::CorruptXtdFixtureByte(path, 8, 0x02);
    CheckRejected([&] { eg::XtdReader reader(path); }, "unsupported version");
    std::filesystem::remove(path);
  }
  {
    const auto path = TempPath("truncated");
    test::WriteXtdFixture(path);
    test::TruncateXtdFixture(path, 200);
    CheckRejected([&] { eg::XtdReader reader(path); }, "truncated package");
    std::filesystem::remove(path);
  }
  {
    const auto path = TempPath("index-auth");
    test::WriteXtdFixture(path);
    const auto index_offset = ReadLe64At(path, 72);
    test::CorruptXtdFixtureByte(path, index_offset);
    CheckRejected([&] { eg::XtdReader reader(path); }, "invalid tile index");
    std::filesystem::remove(path);
  }
  {
    const auto path = TempPath("public-auth");
    test::WriteXtdFixture(path);
    test::CorruptXtdFixtureByte(path, 520);
    CheckRejected([&] { eg::XtdReader reader(path); },
                  "modified authenticated public metadata");
    std::filesystem::remove(path);
  }
  {
    const auto path = TempPath("dimensions");
    test::WriteXtdFixture(path);
    test::CorruptXtdFixtureByte(path, 24, 0xff);
    CheckRejected([&] { eg::XtdReader reader(path); },
                  "invalid or excessive grid dimensions");
    std::filesystem::remove(path);
  }
  {
    const auto path = TempPath("offset-overflow");
    test::WriteXtdFixture(path);
    for (std::uint64_t offset = 88; offset < 96; ++offset) {
      test::CorruptXtdFixtureByte(path, offset, 0xff);
    }
    CheckRejected([&] { eg::XtdReader reader(path); },
                  "overflowing payload offset");
    std::filesystem::remove(path);
  }
  {
    const auto path = TempPath("tile-auth");
    test::WriteXtdFixture(path);
    const auto size = std::filesystem::file_size(path);
    test::CorruptXtdFixtureByte(path, size - 1);
    eg::XtdReader reader(path);
    CheckRejected([&] { (void)reader.LoadRegion(PointGrid(315.0, 2.0)); },
                  "modified encrypted tile");
    std::filesystem::remove(path);
  }
}

void TestEmptyTile() {
  const auto path = TempPath("empty");
  test::XtdFixtureOptions options;
  options.empty_tiles = {0};
  test::WriteXtdFixture(path, options);
  eg::XtdReader reader(path);
  const auto cache = reader.LoadRegion(PointGrid(22.5, -1.5));
  Check(!std::isfinite(cache.u_cm_s[0].real()),
        "empty tile remains missing");
  std::filesystem::remove(path);
}

void TestEnvironmentGeneration() {
  const auto package = TempPath("environment");
  const auto output = std::filesystem::temp_directory_path() /
                      "environmental-grib-offline-tidal.grb";
  test::WriteXtdFixture(package);
  std::filesystem::remove(output);

  eg::EnvironmentRequest request;
  request.bbox = {0.0, -1.0, 90.0, 1.0};
  request.start = eg::ParseUtcDateTime("2026-07-13T00:00:00Z");
  request.hours = 3;
  request.step_hours = 1;
  request.weather_provider = "none";
  request.current_source = "offline-tidal";
  request.offline_tidal_file = package;
  request.current_grid_spacing_deg = 1.0;
  request.output = output;
  request.overwrite = true;
  const auto result = eg::GenerateEnvironment(request);
  const auto scan = eg::ScanGribMessages(output);
  const auto inspection = eg::InspectGrib(output);
  Check(result.current_source == "offline-tidal",
        "environment result identifies Offline Tidal");
  Check(result.diagnostics["offline_tidal"]["tiles_loaded"].asUInt64() > 0,
        "environment result exposes XTD tile diagnostics");
  Check(scan.message_count == 8,
        "four timestamps produce eastward and northward current messages");
  Check(inspection["current_component_counts"]["u_49"].asUInt64() == 4,
        "Offline Tidal writes four eastward current messages");
  Check(inspection["current_component_counts"]["v_50"].asUInt64() == 4,
        "Offline Tidal writes four northward current messages");

  std::filesystem::remove(package);
  std::filesystem::remove(output);
}

}  // namespace

int main() {
  TestValidPackageAndInterpolation();
  TestTileBoundaryAndCacheBound();
  TestSupportedCoefficientEncodings();
  TestLongitudeSeam();
  TestMaskAndOutsideCoverage();
  TestMalformedPackages();
  TestEmptyTile();
  TestEnvironmentGeneration();
  std::cout << "environmental_grib_xtd_tests failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
