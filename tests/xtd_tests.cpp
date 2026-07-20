#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "environmental_grib/error.h"
#include "environmental_grib/environment.h"
#include "environmental_grib/geo.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/model.h"
#include "environmental_grib/random_access.h"
#include "environmental_grib/xtd.h"
#include "environmental_grib/xtd_package.h"
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

std::vector<std::filesystem::path> deferred_cleanup;

void RemoveTestFile(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error && std::find(deferred_cleanup.begin(), deferred_cleanup.end(),
                         path) == deferred_cleanup.end()) {
    deferred_cleanup.push_back(path);
  }
}

void FinishDeferredCleanup() {
  for (const auto& path : deferred_cleanup) {
    std::error_code error;
    std::filesystem::remove(path, error);
    Check(!error, "deferred XTD fixture cleanup: " + path.string());
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

std::vector<unsigned char> ReadBytesAt(const std::filesystem::path& path,
                                       std::uint64_t offset,
                                       std::uint64_t length) {
  std::ifstream input(path, std::ios::binary);
  if (length > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamsize>::max()))
    throw std::runtime_error("fixture byte range is too large");
  input.seekg(static_cast<std::streamoff>(offset));
  std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
  input.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!input) throw std::runtime_error("could not read fixture byte range");
  return bytes;
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
  RemoveTestFile(path);
}

void TestTileBoundaryAndCacheBound() {
  const auto path = TempPath("boundary");
  test::XtdFixtureOptions options;
  options.tile_width = 2;
  options.tile_height = 2;
  test::WriteXtdFixture(path, options);
  eg::XtdReader reader(
      path, {.tile_cache_capacity = 2, .tile_cache_max_bytes = 4'000});
  const auto cache = reader.LoadRegion(PointGrid(90.0, 0.0));
  Check(std::isfinite(cache.u_cm_s[0].real()),
        "interpolation across tile boundaries is valid");
  Check(reader.statistics().peak_cache_bytes > 0,
        "bounded cache reports peak memory");
  Check(reader.statistics().peak_cache_bytes <= 4'000,
        "bounded cache remains within its byte limit");
  reader.ClearTileCache();
  RemoveTestFile(path);
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
  Check(std::abs(int12_cache.u_cm_s[0].real() - int16_cache.u_cm_s[0].real()) <
            1e-9,
        "12-bit and 16-bit tile decoders reconstruct matching coefficients");
  Check(std::abs(int12_cache.v_cm_s[1].imag() - int16_cache.v_cm_s[1].imag()) <
            1e-9,
        "both precision encodings preserve component ordering");
  RemoveTestFile(int12_path);
  RemoveTestFile(int16_path);
}

void TestLongitudeSeam() {
  const auto path = TempPath("seam");
  test::WriteXtdFixture(path);
  eg::XtdReader reader(path);
  const auto west = reader.LoadRegion(PointGrid(-0.25, 0.0));
  const auto east = reader.LoadRegion(PointGrid(359.75, 0.0));
  Check(std::abs(west.u_cm_s[0].real() - east.u_cm_s[0].real()) < 1e-9,
        "longitude seam wraps deterministically");
  RemoveTestFile(path);
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
  RemoveTestFile(path);
}

void TestMalformedPackages() {
  {
    const auto path = TempPath("magic");
    test::WriteXtdFixture(path);
    test::CorruptXtdFixtureByte(path, 0);
    CheckRejected([&] { eg::XtdReader reader(path); }, "corrupt magic");
    RemoveTestFile(path);
  }
  {
    const auto path = TempPath("version");
    test::WriteXtdFixture(path);
    test::CorruptXtdFixtureByte(path, 8, 0x02);
    CheckRejected([&] { eg::XtdReader reader(path); }, "unsupported version");
    RemoveTestFile(path);
  }
  {
    const auto path = TempPath("truncated");
    test::WriteXtdFixture(path);
    test::TruncateXtdFixture(path, 200);
    CheckRejected([&] { eg::XtdReader reader(path); }, "truncated package");
    RemoveTestFile(path);
  }
  {
    const auto path = TempPath("index-auth");
    test::WriteXtdFixture(path);
    const auto index_offset = ReadLe64At(path, 72);
    test::CorruptXtdFixtureByte(path, index_offset);
    CheckRejected([&] { eg::XtdReader reader(path); }, "invalid tile index");
    RemoveTestFile(path);
  }
  {
    const auto path = TempPath("public-auth");
    test::WriteXtdFixture(path);
    test::CorruptXtdFixtureByte(path, 520);
    CheckRejected([&] { eg::XtdReader reader(path); },
                  "modified authenticated public metadata");
    RemoveTestFile(path);
  }
  {
    const auto path = TempPath("dimensions");
    test::WriteXtdFixture(path);
    test::CorruptXtdFixtureByte(path, 24, 0xff);
    CheckRejected([&] { eg::XtdReader reader(path); },
                  "invalid or excessive grid dimensions");
    RemoveTestFile(path);
  }
  {
    const auto path = TempPath("offset-overflow");
    test::WriteXtdFixture(path);
    for (std::uint64_t offset = 88; offset < 96; ++offset) {
      test::CorruptXtdFixtureByte(path, offset, 0xff);
    }
    CheckRejected([&] { eg::XtdReader reader(path); },
                  "overflowing payload offset");
    RemoveTestFile(path);
  }
  {
    const auto path = TempPath("tile-auth");
    test::WriteXtdFixture(path);
    const auto size = std::filesystem::file_size(path);
    test::CorruptXtdFixtureByte(path, size - 1);
    eg::XtdReader reader(path);
    CheckRejected([&] { (void)reader.LoadRegion(PointGrid(315.0, 2.0)); },
                  "modified encrypted tile");
    RemoveTestFile(path);
  }
}

void TestEmptyTile() {
  const auto path = TempPath("empty");
  test::XtdFixtureOptions options;
  options.empty_tiles = {0};
  test::WriteXtdFixture(path, options);
  eg::XtdReader reader(path);
  const auto cache = reader.LoadRegion(PointGrid(22.5, -1.5));
  Check(!std::isfinite(cache.u_cm_s[0].real()), "empty tile remains missing");
  RemoveTestFile(path);
}

void TestEnvironmentGeneration() {
  const auto package = TempPath("environment");
  const auto output = std::filesystem::temp_directory_path() /
                      "environmental-grib-offline-tidal.grb";
  test::WriteXtdFixture(package);
  RemoveTestFile(output);

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

  RemoveTestFile(package);
  RemoveTestFile(output);

  const auto v2_package = TempPath("environment-v2");
  const auto v2_output = std::filesystem::temp_directory_path() /
                         "environmental-grib-offline-current-v2.grb";
  test::WriteXtdV2Fixture(v2_package);
  request.offline_tidal_file = v2_package;
  request.offline_current_mode = "tide-expected-seasonal";
  request.output = v2_output;
  const auto v2_result = eg::GenerateEnvironment(request);
  const auto v2_inspection = eg::InspectGrib(v2_output);
  Check(
      v2_result.diagnostics["offline_tidal"]["format_version"].asUInt() == 2 &&
          v2_result.diagnostics["offline_tidal"]["mode"].asString() ==
              "tide-expected-seasonal",
      "v2 environment generation records explicit package mode");
  Check(v2_result.diagnostics["offline_tidal"]["residual_tiles_loaded"]
                .asUInt64() > 0,
        "v2 total generation loads regional residual tiles");
  Check(v2_inspection["current_component_counts"]["u_49"].asUInt64() == 4 &&
            v2_inspection["current_component_counts"]["v_50"].asUInt64() == 4,
        "v2 total generation preserves GRIB current parameter 49/50 output");
  Check(v2_inspection["message_count"].asUInt64() == 8,
        "v2 masked coastal output remains encodable as GRIB currents");

  const auto weather = std::filesystem::temp_directory_path() /
                       "environmental-grib-xtd-weather-wave.grb2";
  const auto mixed = std::filesystem::temp_directory_path() /
                     "environmental-grib-offline-current-mixed.grb";
  eg::RegularGrid weather_grid;
  weather_grid.longitudes = {0.0, 1.0};
  weather_grid.latitudes = {-1.0, 0.0};
  std::vector<eg::Grib2Field> weather_fields;
  for (const auto& name : {"10u", "10v", "swh", "perpw", "dirpw"})
    weather_fields.push_back(
        {0, name, std::vector<double>(weather_grid.size(), 1.0), {}});
  eg::WriteRegularLatLonGrib2(weather_grid, request.start, weather_fields,
                              weather);
  request.weather_provider = "existing-file";
  request.weather_file = weather;
  request.output = mixed;
  const auto mixed_result = eg::GenerateEnvironment(request);
  const auto mixed_inspection = eg::InspectGrib(mixed);
  Check(mixed_result.message_count == 13 &&
            mixed_inspection["current_component_counts"]["u_49"].asUInt64() ==
                4 &&
            mixed_inspection["short_name_counts"]["10u"].asUInt64() == 1 &&
            mixed_inspection["short_name_counts"]["swh"].asUInt64() == 1,
        "v2 currents merge without displacing weather or wave messages");
  RemoveTestFile(v2_package);
  RemoveTestFile(v2_output);
  RemoveTestFile(weather);
  RemoveTestFile(mixed);
}

void TestBoundedRandomAccess() {
  const auto path = TempPath("bounded-source");
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "0123456789abcdef";
  }
  auto file = eg::OpenFileSource(path);
  auto bounded = eg::MakeBoundedSource(file, 4, 8, "test substream");
  const auto bytes = bounded->Read(2, 3, "fixture bytes");
  Check(std::string(bytes.begin(), bytes.end()) == "678",
        "bounded random access translates relative offsets");
  CheckRejected([&] { (void)bounded->Read(7, 2, "overrun"); },
                "bounded random access overrun");
  CheckRejected(
      [&] {
        (void)bounded->Read(std::numeric_limits<std::uint64_t>::max(), 1,
                            "overflow");
      },
      "bounded random access overflow");
  RemoveTestFile(path);
}

void TestPackageDispatchAndExactTideParity() {
  const auto v1_path = TempPath("dispatch-v1");
  const auto v2_path = TempPath("dispatch-v2");
  test::XtdV2FixtureOptions options;
  test::WriteXtdFixture(v1_path, options.tide);
  test::WriteXtdV2Fixture(v2_path, options);

  const auto grid = PointGrid(90.0, 0.0);
  const std::vector<eg::TimePoint> times{
      eg::ParseUtcDateTime("2026-01-01T00:00:00Z"),
      eg::ParseUtcDateTime("2026-07-01T12:00:00Z")};
  eg::XtdPackageReader v1(v1_path);
  eg::XtdPackageReader v2(v2_path);
  const auto directory_offset = ReadLe64At(v2_path, 48);
  const auto nested_offset = ReadLe64At(v2_path, directory_offset + 96);
  const auto nested_length = ReadLe64At(v2_path, directory_offset + 104);
  Check(ReadBytesAt(v1_path, 0, std::filesystem::file_size(v1_path)) ==
            ReadBytesAt(v2_path, nested_offset, nested_length),
        "embedded v1 bytes are preserved exactly");
  const auto direct =
      v1.Predict(grid, times, eg::OfflineCurrentMode::kAstronomicalTideOnly);
  const auto nested =
      v2.Predict(grid, times, eg::OfflineCurrentMode::kAstronomicalTideOnly);
  Check(v1.status().format_version == 1 && !v1.status().climatology_available,
        "v1 package dispatch preserves tide-only capability");
  Check(!v1.status().package_id.empty(),
        "v1 package dispatch preserves the authenticated package id");
  Check(v2.status().format_version == 2 && v2.status().climatology_available &&
            v2.status().uncertainty_available,
        "v2 package dispatch exposes residual and uncertainty capabilities");
  for (std::size_t i = 0; i < times.size(); ++i) {
    Check(direct[i].u_mps == nested[i].u_mps &&
              direct[i].v_mps == nested[i].v_mps &&
              direct[i].mask == nested[i].mask,
          "embedded v1 produces exact tide output parity");
  }
  CheckRejected(
      [&] {
        (void)v1.Predict(
            grid, times,
            eg::OfflineCurrentMode::kTideAndExpectedSeasonalCirculation);
      },
      "v1 total mode without residual");

  RemoveTestFile(v1_path);
  RemoveTestFile(v2_path);
}

void TestHarmonicResidualAndRegionalReads() {
  const auto path = TempPath("v2-harmonic");
  test::XtdV2FixtureOptions options;
  options.residual_value = [](std::size_t field, std::uint32_t, std::uint32_t) {
    constexpr double values[10]{0.20,  0.03, -0.02, 0.01,  -0.005,
                                -0.10, 0.04, 0.01,  -0.02, 0.005};
    return values[field];
  };
  test::WriteXtdV2Fixture(path, options);
  eg::XtdPackageReader reader(
      path, {.tile_cache_capacity = 2, .tile_cache_max_bytes = 8'000});
  const auto grid = PointGrid(90.0, 0.0);
  const std::vector<eg::TimePoint> times{
      eg::ParseUtcDateTime("2026-01-01T00:00:00Z")};
  const auto tide = reader.Predict(
      grid, times, eg::OfflineCurrentMode::kAstronomicalTideOnly);
  const auto total = reader.Predict(
      grid, times, eg::OfflineCurrentMode::kTideAndExpectedSeasonalCirculation);
  Check(std::abs((total[0].u_mps[0] - tide[0].u_mps[0]) - 0.24) < 1e-6,
        "harmonic residual evaluates mean, annual and semiannual U");
  Check(std::abs((total[0].v_mps[0] - tide[0].v_mps[0]) + 0.08) < 1e-6,
        "harmonic residual evaluates mean, annual and semiannual V");
  const auto statistics = reader.statistics();
  Check(statistics.outer_bytes_read < std::filesystem::file_size(path),
        "regional query does not read or decrypt the complete v2 package");
  Check(statistics.residual.tiles_loaded <= 4,
        "regional query loads only interpolation-neighbour residual tiles");
  Check(statistics.uncertainty.tiles_loaded == 0,
        "current calculation does not load uncertainty tiles");
  RemoveTestFile(path);
}

void TestMonthlyCentreInterpolationAndMask() {
  const auto path = TempPath("v2-monthly");
  test::XtdV2FixtureOptions options;
  options.representation = test::XtdV2ResidualRepresentation::kMonthly12;
  options.residual_value = [](std::size_t field, std::uint32_t, std::uint32_t) {
    return field < 12 ? 0.01 * static_cast<double>(field + 1)
                      : -0.01 * static_cast<double>(field - 11);
  };
  options.valid = [](std::uint32_t x, std::uint32_t y) {
    return !(x == 1 && y == 1);
  };
  test::WriteXtdV2Fixture(path, options);
  eg::XtdPackageReader reader(path);
  const std::vector<eg::TimePoint> times{
      eg::ParseUtcDateTime("2026-01-16T12:00:00Z"),
      eg::ParseUtcDateTime("2026-02-15T00:00:00Z")};
  const auto valid_grid = PointGrid(90.0, 0.0);
  const auto tide = reader.Predict(
      valid_grid, times, eg::OfflineCurrentMode::kAstronomicalTideOnly);
  const auto total = reader.Predict(
      valid_grid, times,
      eg::OfflineCurrentMode::kTideAndExpectedSeasonalCirculation);
  Check(std::abs((total[0].u_mps[0] - tide[0].u_mps[0]) - 0.01) < 1e-6,
        "monthly residual equals January field at January month centre");
  Check(std::abs((total[1].u_mps[0] - tide[1].u_mps[0]) - 0.02) < 1e-6,
        "monthly residual equals February field at February month centre");
  const auto masked = reader.Predict(
      PointGrid(45.0, -1.0), {times.front()},
      eg::OfflineCurrentMode::kTideAndExpectedSeasonalCirculation);
  Check(masked[0].has_mask() && masked[0].mask[0] &&
            !std::isfinite(masked[0].u_mps[0]),
        "missing residual interpolation remains missing rather than zero");
  RemoveTestFile(path);
}

void TestV2VerificationAndCorruption() {
  {
    const auto path = TempPath("v2-verify");
    test::WriteXtdV2Fixture(path);
    const auto result = eg::VerifyXtdPackage(
        path, {.tile_cache_capacity = 2, .tile_cache_max_bytes = 8'000});
    Check(result["valid"].asBool() && result["stored_hashes_valid"].asBool(),
          "full v2 verification checks component hashes");
    Check(result["climatological_uncertainty"]["tiles_loaded"].asUInt64() > 0,
          "full v2 verification authenticates uncertainty tiles");
    RemoveTestFile(path);
  }
  {
    const auto path = TempPath("v2-version");
    test::WriteXtdV2Fixture(path);
    test::CorruptXtdFixtureByte(path, 8, 0x03);
    CheckRejected([&] { eg::XtdPackageReader reader(path); },
                  "unsupported v2 version");
    RemoveTestFile(path);
  }
  {
    const auto path = TempPath("v2-outer-auth");
    test::WriteXtdV2Fixture(path);
    test::CorruptXtdFixtureByte(path, 520);
    CheckRejected([&] { eg::XtdPackageReader reader(path); },
                  "modified v2 public metadata");
    RemoveTestFile(path);
  }
  {
    const auto path = TempPath("v2-reserved");
    test::WriteXtdV2Fixture(path);
    test::CorruptXtdFixtureByte(path, 300);
    CheckRejected([&] { eg::XtdPackageReader reader(path); },
                  "nonzero v2 reserved header byte");
    RemoveTestFile(path);
  }
  {
    const auto path = TempPath("v2-residual-auth");
    test::WriteXtdV2Fixture(path);
    const auto directory_offset = ReadLe64At(path, 48);
    const auto residual_index = ReadLe64At(path, directory_offset + 256 + 64);
    // Point (90, 0) uses source cell (2, 2), which is tile id 5 for the
    // fixture's 2x2 tile layout.
    const auto selected_tile_payload =
        ReadLe64At(path, residual_index + 5 * 64 + 24);
    test::CorruptXtdFixtureByte(path, selected_tile_payload);
    eg::XtdPackageReader reader(path);
    const auto tide = reader.Predict(
        PointGrid(90.0, 0.0),
        {eg::ParseUtcDateTime("2026-01-01T00:00:00Z")},
        eg::OfflineCurrentMode::kAstronomicalTideOnly);
    Check(std::isfinite(tide.front().u_mps.front()) &&
              std::isfinite(tide.front().v_mps.front()),
          "corrupt residual does not prevent explicit tide-only use");
    CheckRejected(
        [&] {
          (void)reader.Predict(
              PointGrid(90.0, 0.0),
              {eg::ParseUtcDateTime("2026-01-01T00:00:00Z")},
              eg::OfflineCurrentMode::kTideAndExpectedSeasonalCirculation);
        },
        "modified residual tile");
    RemoveTestFile(path);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--write-v1-fixture") {
    test::WriteXtdFixture(argv[2]);
    return 0;
  }
  TestValidPackageAndInterpolation();
  TestTileBoundaryAndCacheBound();
  TestSupportedCoefficientEncodings();
  TestLongitudeSeam();
  TestMaskAndOutsideCoverage();
  TestMalformedPackages();
  TestEmptyTile();
  TestBoundedRandomAccess();
  TestEnvironmentGeneration();
  TestPackageDispatchAndExactTideParity();
  TestHarmonicResidualAndRegionalReads();
  TestMonthlyCentreInterpolationAndMask();
  TestV2VerificationAndCorruption();
  FinishDeferredCleanup();
  std::cout << "environmental_grib_xtd_tests failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
