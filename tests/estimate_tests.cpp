#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "environmental_grib/estimate.h"
#include "environmental_grib/error.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/sources.h"
#include "environmental_grib/platform.h"
#include <eccodes.h>
#include "environmental_grib/ukv.h"

namespace eg = environmental_grib;
void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
template<class F> void Invalid(F fn) {
  try { fn(); } catch (const eg::ValidationError&) { return; }
  throw std::runtime_error("expected validation error");
}
int main() {
  try {
    eg::EnvironmentRequest r;
    r.bbox = {-6, 53, -5, 54};
    r.start = eg::ParseUtcDateTime("2026-07-12T00:00:00Z");
    r.hours = 48; r.step_hours = 1;
    r.weather_provider = "none"; r.current_source = "synthetic";
    r.current_grid_spacing_deg = 0.05;
    auto e = eg::EstimateEnvironment(r);
    Check(e["complete"].asBool(), "current plan complete");
    Check(e["knownRecords"].asUInt64() == 98, "two current components including endpoints");
    Check(e["decodedBytes"].asUInt64() == 98 * 441 * 8, "numeric bytes");
    const auto base = e["decodedBytes"].asUInt64();
    r.step_hours = 3;
    Check(eg::EstimateEnvironment(r)["knownRecords"].asUInt64() == 34, "cadence");
    r.current_grid_spacing_deg = 0.025;
    Check(eg::EstimateEnvironment(r)["decodedBytes"].asUInt64() > base, "resolution");
    r.current_source = "tpxo-cache";
    Check(!eg::EstimateEnvironment(r)["complete"].asBool(), "existing cache uses its own grid");
    r.current_source = "offline-tidal";
    Check(eg::EstimateEnvironment(r)["complete"].asBool(), "offline plan does not open model");
    r.weather_provider = "ukmo_ukv";
    r.weather_preset = "all";
    e = eg::EstimateEnvironment(r);
    Check(e["complete"].asBool(), "mixed grid plan");
    Check(e["knownRecords"].asUInt64() == 34 + 28 + 16 * 29, "UKV levels and absent hour-zero rain");
    r.step_hours = 1; r.hours = 60; r.current_source = "none";
    r.weather_preset = "minimal";
    Check(eg::EstimateEnvironment(r)["knownRecords"].asUInt64() == 57 * 2, "UKV hourly to 3-hourly handover");
    r.extend_forecast = true;
    e = eg::EstimateEnvironment(r);
    Check(!e["complete"].asBool() && !e.isMember("decodedBytes") &&
          !e.isMember("fileUpperBytes"), "extension not silently underestimated");
    r.extend_forecast = false; r.hours = 48; r.weather_provider = "gfs";
    e = eg::EstimateEnvironment(r);
    Check(e["approximate"].asBool() && !e.isMember("fileUpperBytes"), "GFS packing unknown");
    r.weather_preset = "all";
    Check(!eg::EstimateEnvironment(r)["complete"].asBool(), "GFS multi-level inventory unknown");
    r.weather_provider = "existing-file";
    r.weather_file = "/must-not-be-read.grb";
    Check(!eg::EstimateEnvironment(r)["complete"].asBool(), "estimate never reads existing streams");
    r.weather_provider = "none"; r.include_waves = true;
    Check(!eg::EstimateEnvironment(r)["complete"].asBool(), "unknown waves are not zero");
    r.include_waves = false;
    Check(eg::EstimateEnvironment(r)["decodedBytes"].asUInt64() == 0, "no components");
    for (const auto* provider : {"copernicus_nws", "copernicus_global",
                                 "copernicus_ibi", "copernicus_mediterranean"}) {
      r.current_source = provider;
      Check(eg::EstimateEnvironment(r)["knownRecords"].asUInt64() == 98,
            "Copernicus current output uses requested cadence, not native grid");
    }
    r.current_source = "none";
    r.include_waves = true; r.wave_provider = "copernicus_global_waves";
    r.weather_grid_spacing_deg = 0.1;
    e = eg::EstimateEnvironment(r);
    Check(e["complete"].asBool() && e["knownRecords"].asUInt64() == 51 &&
          e["decodedBytes"].asUInt64() == 51 * 121 * 8,
          "Copernicus wave writer grid and independent 3-hour cadence");
    r.hours = 240;
    Check(eg::EstimateEnvironment(r)["knownRecords"].asUInt64() == 243, "wave endpoint");
    r.hours = 243; Invalid([&] { eg::EstimateEnvironment(r); });
    r.hours = 4; Invalid([&] { eg::EstimateEnvironment(r); });
    r.hours = 48; r.wave_step_hours = 1;
    Invalid([&] { eg::EstimateEnvironment(r); });
    r.wave_step_hours = 3; r.include_waves = false;
    r.bbox.west = 179; r.bbox.east = -179;
    Invalid([&] { eg::EstimateEnvironment(r); });
    r.bbox = {-6, 53, -5, 54}; r.current_source = "synthetic";
    r.current_grid_spacing_deg = std::numeric_limits<double>::denorm_min();
    Invalid([&] { eg::EstimateEnvironment(r); });
    r.current_grid_spacing_deg = std::numeric_limits<double>::quiet_NaN();
    Invalid([&] { eg::EstimateEnvironment(r); });
    r.current_grid_spacing_deg = 0.05; r.hours = std::numeric_limits<int>::max();
    Invalid([&] { eg::EstimateEnvironment(r); });
    r.hours = 5; r.step_hours = 3;
    Invalid([&] { eg::EstimateEnvironment(r); });
    // Compare dimensions against the real grid writer, including non-integral
    // spans and endpoint rounding. No forecasts or network needed.
    for (double span : {0.91, 1.0, 1.02, 1.03}) {
      const eg::BoundingBox box{-6, 53, -6 + span, 54};
      auto [nx, ny] = eg::RegularGridDimensions(box, 0.05);
      auto grid = eg::BuildRegularGrid(box, 0.05);
      Check(nx == grid.nx() && ny == grid.ny(), "grid dimensions match writer");
    }
    // Exercise the real writer with variable, constant and partially masked
    // fields, then count decoded cells independently via ecCodes headers.
    r.hours = 6; r.step_hours = 3;
    auto grid = eg::BuildRegularGrid(r.bbox, r.current_grid_spacing_deg);
    const auto output = std::filesystem::temp_directory_path() /
        ("xgrib-estimate-test-" + std::to_string(eg::ProcessId()) + ".grb");
    Check(!std::filesystem::exists(output), "test output must be private");
    std::vector<eg::CurrentGrid> currents;
    for (auto time : eg::BuildTimeSequence(r.start, r.hours, r.step_hours)) {
      eg::CurrentGrid current{time, grid, std::vector<double>(grid.size()),
                             std::vector<double>(grid.size(), 0.5),
                             std::vector<std::uint8_t>(grid.size())};
      for (std::size_t i = 0; i < grid.size(); ++i) {
        current.u_mps[i] = std::sin(static_cast<double>(i));
        current.mask[i] = (i % 7 == 0);
      }
      currents.push_back(std::move(current));
    }
    eg::WriteGrib1Currents(currents, output);
    e = eg::EstimateEnvironment(r);
    eg::EnvironmentResult generated;
    generated.output = output;
    generated.inspection = eg::InspectGrib(output);
    generated.byte_count = std::filesystem::file_size(output);
    generated.message_count = currents.size() * 2;
    r.copernicus_username = "private-user";
    r.copernicus_password = "private-password";
    r.offline_tidal_file = "/private-path/model.xtd";
    r.metno_dataset_url = "https://private-url.invalid";
    auto report = eg::BuildSizeComparison(r, e, generated);
    Check(report["actual"]["numericComplete"].asBool() &&
          report["actual"]["decodedBytes"] == e["decodedBytes"] &&
          report["numericStatus"].asString() == "exact" &&
          report["fileStatus"].asString() == "within_upper_estimate",
          "report reuses actual inventory including missing cells");
    Check(report.toStyledString().find("private-") == std::string::npos,
          "report excludes credentials, local source paths and URLs");
    auto inaccurate = e;
    inaccurate["decodedBytes"] = Json::UInt64(1);
    inaccurate["fileUpperBytes"] = Json::UInt64(1);
    report = eg::BuildSizeComparison(r, inaccurate, generated);
    Check(report["numericStatus"].asString() == "underestimate" &&
          report["fileStatus"].asString() == "exceeds_upper_estimate", "underestimate visible");
    inaccurate["decodedBytes"] = Json::UInt64(e["decodedBytes"].asUInt64() + 1);
    Check(eg::BuildSizeComparison(r, inaccurate, generated)["numericStatus"].asString() ==
          "overestimate", "overestimate visible");
    r.extend_forecast = true;
    report = eg::BuildSizeComparison(r, eg::EstimateEnvironment(r), generated);
    Check(report["actual"]["numericComplete"].asBool() &&
          report["numericStatus"].asString() == "unknown",
          "complete actual inventory even when extension estimate is unknown");
    r.extend_forecast = false;
    generated.inspection["messages"][0].removeMember("values");
    report = eg::BuildSizeComparison(r, e, generated);
    Check(!report["actual"]["numericComplete"].asBool() &&
          !report["actual"].isMember("decodedBytes"), "partial inventory is not a total");
    generated.inspection["messages"] = Json::Value(Json::arrayValue);
    Check(!eg::BuildSizeComparison(r, e, generated)["actual"]["numericComplete"].asBool(),
          "missing inventory is not a zero total");
    Check(std::filesystem::file_size(output) <= e["fileUpperBytes"].asUInt64(),
          "actual current GRIB fits planning upper estimate");
    FILE* input = std::fopen(eg::PathToUtf8(output).c_str(), "rb");
    Check(input != nullptr, "open fixture");
    std::uint64_t bytes = 0, records = 0;
    int status = 0;
    while (auto* handle = codes_handle_new_from_file(nullptr, input, PRODUCT_GRIB, &status)) {
      std::size_t count = 0;
      Check(codes_get_size(handle, "values", &count) == 0, "independent record count");
      bytes += count * sizeof(double); ++records;
      codes_handle_delete(handle);
    }
    std::fclose(input);
    std::filesystem::remove(output);
    Check(bytes == e["decodedBytes"].asUInt64() &&
          records == e["knownRecords"].asUInt64(), "real writer matches numeric estimate");
    std::cout << "Estimate tests passed (components, grids, cadence, UKV levels, unknowns, overflow).\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
