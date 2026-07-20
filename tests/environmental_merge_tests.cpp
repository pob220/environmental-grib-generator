#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <json/json.h>

#include "environmental_grib/geo.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/model.h"

namespace eg = environmental_grib;

namespace {

void Check(bool condition, const std::string& message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void WriteJson(const std::filesystem::path& path, const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "  ";
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  Check(static_cast<bool>(output), "create " + path.string());
  output << Json::writeString(builder, value) << '\n';
  Check(static_cast<bool>(output), "write " + path.string());
}

eg::RegularGrid Grid(double west = -6.3, double south = 53.0) {
  eg::RegularGrid grid;
  grid.longitudes = {west, west + 0.1, west + 0.2};
  grid.latitudes = {south, south + 0.1};
  grid.spacing_deg = 0.1;
  grid.latitude_spacing_deg = 0.1;
  grid.longitude_spacing_deg = 0.1;
  return grid;
}

eg::CurrentGrid Current(const eg::RegularGrid& grid, eg::TimePoint time,
                        double offset) {
  eg::CurrentGrid current;
  current.grid = grid;
  current.time = time;
  for (std::size_t i = 0; i < grid.size(); ++i) {
    current.u_mps.push_back(offset + 0.1 * static_cast<double>(i + 1));
    current.v_mps.push_back(-offset - 0.2 * static_cast<double>(i + 1));
  }
  return current;
}

const Json::Value* FindMessage(const Json::Value& inspection,
                               const std::string& short_name,
                               const std::string& valid_time) {
  for (const auto& message : inspection["messages"]) {
    if (message.get("short_name", "").asString() == short_name &&
        message.get("valid_time", "").asString() == valid_time)
      return &message;
  }
  return nullptr;
}

bool Near(double actual, double expected, double tolerance = 1e-4) {
  return std::abs(actual - expected) <= tolerance;
}

}  // namespace

int main(int argc, char** argv) {
  Check(argc == 2, "usage: environmental_grib_merge_tests OUTPUT_DIRECTORY");
  const std::filesystem::path root = argv[1];
  std::filesystem::create_directories(root);

  const auto start = eg::ParseUtcDateTime("2026-07-12T00:00:00Z");
  const auto grid = Grid();
  const auto wind = root / "wind-known.grb2";
  const auto current_matching = root / "current-matching.grb";
  const auto current_differing = root / "current-differing.grb";
  const auto combined = root / "combined-known.grb2";

  const std::vector<eg::Grib2Field> wind_fields{
      {0, "10u", {1, 2, 3, 4, 5, 6}, {}},
      {0, "10v", {-1, -2, -3, -4, -5, -6}, {}},
      {3, "10u", {11, 12, 13, 14, 15, 16}, {}},
      {3, "10v", {-11, -12, -13, -14, -15, -16}, {}}};
  eg::WriteRegularLatLonGrib2(grid, start, wind_fields, wind);
  eg::WriteGrib1Currents({Current(grid, start, 0.0),
                          Current(grid, start + std::chrono::hours(3), 1.0)},
                         current_matching);
  eg::WriteGrib1Currents({Current(grid, start, 0.0),
                          Current(grid, start + std::chrono::hours(3), 1.0),
                          Current(grid, start + std::chrono::hours(6), 2.0)},
                         current_differing);

  eg::EnvironmentalMergeRequest request;
  request.weather = wind;
  request.current = current_differing;
  request.output = combined;
  request.overwrite = true;
  const auto result = eg::MergeEnvironmentalGribs(request);
  Check(result.success, "known wind/current merge succeeds");
  Check(result.output_message_count == 10,
        "combined output retains four wind and six current messages");
  Check(result.warnings.size() == 1,
        "differing compatible time records produce an explicit warning");
  Check(result.output_inspection["message_count"].asUInt64() == 10,
        "combined output reopens with ecCodes");
  Check(
      result.output_inspection["short_name_counts"]["10u"].asUInt64() == 2 &&
          result.output_inspection["short_name_counts"]["10v"].asUInt64() == 2,
      "both wind components and times are retained");
  Check(
      result.output_inspection["current_component_counts"]["u_49"].asUInt64() ==
              3 &&
          result.output_inspection["current_component_counts"]["v_50"]
                  .asUInt64() == 3,
      "both current components and all three times are retained");
  Check(result.output_inspection["valid_times"].size() == 3 &&
            result.output_inspection["first_valid_time"].asString() ==
                "20260712T0000" &&
            result.output_inspection["last_valid_time"].asString() ==
                "20260712T0600",
        "combined valid-time union is coherent");
  const auto& coverage = result.output_inspection["coverage"];
  Check(Near(coverage["west"].asDouble(), -6.3) &&
            Near(coverage["east"].asDouble(), -6.1) &&
            Near(coverage["south"].asDouble(), 53.0) &&
            Near(coverage["north"].asDouble(), 53.1),
        "combined geographic extent matches deterministic fixtures");

  const auto* first_u =
      FindMessage(result.output_inspection, "10u", "20260712T0000");
  const auto* later_u =
      FindMessage(result.output_inspection, "10u", "20260712T0300");
  Check(first_u && later_u, "known wind messages are inspectable");
  Check(Near((*first_u)["values"]["minimum"].asDouble(), 1.0) &&
            Near((*first_u)["values"]["maximum"].asDouble(), 6.0) &&
            Near((*first_u)["values"]["mean"].asDouble(), 3.5) &&
            Near((*later_u)["values"]["sample"][0].asDouble(), 11.0),
        "known wind values survive the merge");

  std::set<std::string> identities;
  for (const auto& message : result.output_inspection["messages"]) {
    std::string field = message.get("short_name", "").asString();
    if (field.empty() || field == "unknown")
      field = "parameter-" +
              std::to_string(message.get("parameter_number", 0).asInt64());
    identities.insert(field + "|" +
                      message.get("valid_time", "unknown").asString());
    Check(message["values"]["count"].asUInt64() == grid.size(),
          "every combined message has the complete grid");
    Check(message["values"]["missing_count"].asUInt64() == 0,
          "deterministic combined output has no missing values");
  }
  Check(identities.size() == 10,
        "combined output contains no unintended duplicate fields");

  eg::EnvironmentalMergeRequest matching_request = request;
  matching_request.current = current_matching;
  matching_request.output = root / "combined-matching.grb2";
  const auto matching = eg::MergeEnvironmentalGribs(matching_request);
  Check(matching.success && matching.output_message_count == 8 &&
            matching.warnings.empty(),
        "matching wind/current time records merge without warnings");

  eg::EnvironmentalMergeRequest wind_only;
  wind_only.weather = wind;
  wind_only.output = root / "wind-only-combined.grb2";
  wind_only.overwrite = true;
  Check(eg::MergeEnvironmentalGribs(wind_only).success,
        "weather-only input is supported by the production merge service");

  eg::EnvironmentalMergeRequest current_only;
  current_only.current = current_differing;
  current_only.output = root / "current-only-combined.grb";
  current_only.overwrite = true;
  Check(eg::MergeEnvironmentalGribs(current_only).success,
        "current-only input is supported by the production merge service");

  const auto corrupt = root / "corrupt.grb";
  {
    std::ofstream output(corrupt, std::ios::binary | std::ios::trunc);
    output << "not a GRIB";
  }
  eg::EnvironmentalMergeRequest invalid;
  invalid.weather = corrupt;
  invalid.output = root / "invalid-output.grb";
  invalid.overwrite = true;
  const auto invalid_result = eg::MergeEnvironmentalGribs(invalid);
  Check(!invalid_result.success && !invalid_result.errors.empty(),
        "corrupt input fails with structured diagnostics");

  eg::EnvironmentalMergeRequest wrong_role;
  wrong_role.weather = current_matching;
  wrong_role.output = root / "wrong-role.grb";
  wrong_role.overwrite = true;
  const auto wrong_role_result = eg::MergeEnvironmentalGribs(wrong_role);
  Check(!wrong_role_result.success && !wrong_role_result.errors.empty(),
        "current-only file is rejected in the weather role");

  const auto shifted_current = root / "current-incompatible-area.grb";
  const auto shifted_grid = Grid(20.0, 10.0);
  eg::WriteGrib1Currents({Current(shifted_grid, start, 0.0)}, shifted_current);
  eg::EnvironmentalMergeRequest incompatible_area = request;
  incompatible_area.current = shifted_current;
  incompatible_area.output = root / "incompatible-area.grb";
  const auto area_result = eg::MergeEnvironmentalGribs(incompatible_area);
  Check(!area_result.success && !area_result.errors.empty(),
        "non-overlapping geographic inputs are rejected");

  const auto late_current = root / "current-incompatible-time.grb";
  eg::WriteGrib1Currents({Current(grid, start + std::chrono::hours(9), 0.0),
                          Current(grid, start + std::chrono::hours(12), 1.0)},
                         late_current);
  eg::EnvironmentalMergeRequest incompatible_time = request;
  incompatible_time.current = late_current;
  incompatible_time.output = root / "incompatible-time.grb";
  const auto time_result = eg::MergeEnvironmentalGribs(incompatible_time);
  Check(!time_result.success && !time_result.errors.empty(),
        "non-overlapping valid-time inputs are rejected");

  Json::Value manifest(Json::objectValue);
  manifest["schema"] = "xgrib-deterministic-fixtures-v1";
  manifest["generated_by"] = "environmental_grib_merge_tests";
  manifest["reference_time"] = "2026-07-12T00:00:00Z";
  manifest["grid"]["west"] = -6.3;
  manifest["grid"]["south"] = 53.0;
  manifest["grid"]["east"] = -6.1;
  manifest["grid"]["north"] = 53.1;
  manifest["grid"]["nx"] = 3;
  manifest["grid"]["ny"] = 2;
  manifest["fixtures"]["wind"] = wind.filename().string();
  manifest["fixtures"]["current_matching"] =
      current_matching.filename().string();
  manifest["fixtures"]["current_differing"] =
      current_differing.filename().string();
  manifest["fixtures"]["corrupt"] = corrupt.filename().string();
  manifest["fixtures"]["combined"] = combined.filename().string();
  WriteJson(root / "fixture-manifest.json", manifest);
  WriteJson(root / "merge-result.json",
            eg::EnvironmentalMergeResultJson(result));

  std::cout << "deterministic environmental GRIB merge verified in " << root
            << '\n';
  return 0;
}
