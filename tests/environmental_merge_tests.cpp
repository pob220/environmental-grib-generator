#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <eccodes.h>
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

// Encode provider-style headers directly, including wrapped endpoints and
// reversed scanning which the local regular-grid writer does not produce.
void ProviderWind(const std::filesystem::path& path, double first,
                  double last, long ni, double increment,
                  bool negative_scan = false, int edition = 2) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  for (const char* field : {"10u", "10v"}) {
    auto* h = grib_handle_new_from_samples(
        nullptr, edition == 1 ? "regular_ll_sfc_grib1" : "regular_ll_sfc_grib2");
    Check(h != nullptr, "create provider-style fixture");
    const auto set = [&](const char* key, double value) {
      Check(codes_set_double(h, key, value) == 0, std::string("set ") + key);
    };
    std::size_t length = std::char_traits<char>::length(field);
    Check(codes_set_string(h, "shortName", field, &length) == 0, "set wind field");
    set("dataDate", 20260712);
    set("dataTime", 0);
    set("Ni", ni);
    set("Nj", 3);
    set("latitudeOfFirstGridPointInDegrees", -18);
    set("latitudeOfLastGridPointInDegrees", -22);
    set("longitudeOfFirstGridPointInDegrees", first);
    set("longitudeOfLastGridPointInDegrees", last);
    set("iDirectionIncrementInDegrees", increment);
    set("jDirectionIncrementInDegrees", 2);
    set("iScansNegatively", negative_scan);
    set("jScansPositively", 0);
    const std::vector<double> values(ni * 3, field[2] == 'u' ? 5.0 : -2.0);
    Check(codes_set_double_array(h, "values", values.data(), values.size()) == 0,
          "set provider wind values");
    const void* message = nullptr;
    Check(codes_get_message(h, &message, &length) == 0, "encode provider wind");
    out.write(static_cast<const char*>(message), length);
    codes_handle_delete(h);
  }
  Check(static_cast<bool>(out), "write provider wind fixture");
}

void CheckLongitudeCoverage(const std::filesystem::path& root,
                            eg::TimePoint start) {
  const auto wind = root / "provider-longitudes.grb";
  const auto current = root / "tonga-current.grb";
  eg::EnvironmentalMergeRequest request;
  request.weather = wind;
  request.current = current;
  request.output = root / "longitude-merge.grb";
  request.overwrite = true;
  const auto check_merge = [&](double longitude, double latitude,
                               bool expected, const std::string& label) {
    eg::WriteGrib1Currents({Current(Grid(longitude, latitude), start, 0)}, current);
    const auto result = eg::MergeEnvironmentalGribs(request);
    Check(result.success == expected, label);
    if (expected) {
      Check(result.output_message_count == 4, label + " preserves all fields");
      Check(Near(result.output_inspection["messages"][0]["values"]["mean"].asDouble(), 5),
            label + " preserves weather values");
    } else {
      Check(!result.errors.empty() && result.errors.front() ==
                "weather and current GRIB geographic coverage does not overlap",
            label + " rejects geographic mismatch");
    }
  };

  for (const int edition : {1, 2}) {
    for (const double origin : {0.0, 180.0}) {
      for (const bool reversed : {false, true}) {
        const double last = std::fmod(origin + (reversed ? 0.25 : 359.75), 360.0);
        ProviderWind(wind, origin, last, 1440, 0.25, reversed, edition);
        const auto coverage = eg::InspectGrib(wind)["coverage"];
        Check(Near(coverage["west"].asDouble(), -180) &&
                  Near(coverage["east"].asDouble(), 180),
              "cyclic global longitude coverage is the whole globe");
        check_merge(-179.9, -21, true, "global grid overlaps Tonga");
        check_merge(179.7, -21, true, "global grid overlaps Fiji side");
        check_merge(-0.1, -21, true, "global grid overlaps Greenwich");
        check_merge(-175, 20, false, "global longitude still checks latitude");
      }
    }
  }
  ProviderWind(wind, 180, 180, 1441, 0.25);
  check_merge(-179, -21, true, "global grid with duplicate seam endpoint");

  for (const bool reversed : {false, true}) {
    ProviderWind(wind, reversed ? 190 : 170, reversed ? 170 : 190,
                 81, 0.25, reversed);
    check_merge(-175, -21, true, "wrapped regional grid overlaps Tonga");
    check_merge(175, -21, true, "wrapped regional grid overlaps Fiji");
    check_merge(0, -21, false, "wrapped regional grid excludes Greenwich");
    check_merge(-160, -21, false, "wrapped regional grid excludes eastern gap");
  }
  ProviderWind(wind, 180, 190, 41, 0.25);
  check_merge(-179.9, -21, true, "regional grid starting at positive 180");
  check_merge(175, -21, false, "regional grid is not mistaken for global");
  ProviderWind(wind, 350, 10, 81, 0.25);
  check_merge(-5, -21, true, "0-to-360 regional grid crosses Greenwich");
  check_merge(-175, -21, false, "Greenwich region excludes Tonga");
  ProviderWind(wind, 10, 350, 1361, 0.25);
  check_merge(175, -21, true, "wide regional grid uses scan extent, not shortest arc");
  check_merge(0, -21, false, "wide regional grid preserves uncovered gap");
  ProviderWind(wind, 170, 180, 41, 0.25);
  check_merge(-180, -21, true, "positive and negative 180 denote same boundary");

  const auto other_wind = root / "other-region-wind.grb";
  const auto regional_wind = root / "two-region-wind.grb";
  ProviderWind(other_wind, 200, 210, 41, 0.25);
  eg::MergeGribStreams({{"west", wind}, {"east", other_wind}}, regional_wind, true);
  request.weather = regional_wind;
  check_merge(-175, -21, false, "separate grids do not fill their geographic gap");

  // Preserve a stable combined global/regional fixture for the production
  // xGRIB reader, and verify optional waves survive the same merge path.
  const auto tonga_grid = eg::BuildRegularGrid({-179.9, -22, -170, -18}, 0.1);
  eg::WriteGrib1Currents({Current(tonga_grid, start, 0)}, current);
  ProviderWind(wind, 180, 179.75, 1440, 0.25);
  const auto waves = root / "tonga-waves.grb2";
  eg::WriteRegularLatLonGrib2(tonga_grid, start,
      {{0, "swh", std::vector<double>(tonga_grid.size(), 2.0), {}}}, waves);
  request.weather = wind;
  request.waves = waves;
  request.output = root / "tonga-combined.grb";
  const auto tonga = eg::MergeEnvironmentalGribs(request);
  Check(tonga.success && tonga.output_message_count == 5 &&
            tonga.output_inspection["short_name_counts"]["swh"].asUInt64() == 1,
        "global weather, Tonga current and optional waves merge intact");
}

}  // namespace

int main(int argc, char** argv) {
  Check(argc == 2, "usage: environmental_grib_merge_tests OUTPUT_DIRECTORY");
  const std::filesystem::path root = argv[1];
  std::filesystem::create_directories(root);

  const auto start = eg::ParseUtcDateTime("2026-07-12T00:00:00Z");
  CheckLongitudeCoverage(root, start);
  const auto grid = Grid();
  const auto wind = root / "wind-known.grb2";
  const auto current_matching = root / "current-matching.grb";
  const auto current_differing = root / "current-differing.grb";
  const auto combined = root / "combined-known.grb2";
  const auto all_weather = root / "all-weather-known.grb2";
  const auto combined_all = root / "combined-all-known.grb2";
  const auto long_weather = root / "long-weather-known.grb2";
  const auto long_current = root / "long-current-known.grb";
  const auto combined_long = root / "combined-long-known.grb2";

  const std::vector<eg::Grib2Field> wind_fields{
      {0, "10u", {1, 2, 3, 4, 5, 6}, {}},
      {0, "10v", {-1, -2, -3, -4, -5, -6}, {}},
      {3, "10u", {11, 12, 13, 14, 15, 16}, {}},
      {3, "10v", {-11, -12, -13, -14, -15, -16}, {}}};
  eg::WriteRegularLatLonGrib2(grid, start, wind_fields, wind);
  const auto values = [](double value) {
    return std::vector<double>(6, value);
  };
  const std::vector<eg::Grib2Field> all_weather_fields{
      {0, "10u", values(4.0), {}},
      {0, "10v", values(-2.0), {}},
      {3, "10u", values(5.0), {}},
      {3, "10v", values(-3.0), {}},
      {0, "gust", values(8.0), {}, "surface", 0.0},
      {0, "tcc", values(65.0), {}, "entireAtmosphere", 0.0},
      {0, "cape", values(500.0), {}, "surface", 0.0},
      {0, "refc", values(24.0), {}, "entireAtmosphere", 0.0},
      {0, "2r", values(75.0), {}, "heightAboveGround", 2.0},
      {0, "prmsl", values(101325.0), {}, "meanSea", 0.0},
      {0, "2t", values(285.0), {}, "heightAboveGround", 2.0},
      {0, "u", values(12.0), {}, "isobaricInhPa", 850.0},
      {0, "v", values(-6.0), {}, "isobaricInhPa", 850.0},
      {0, "t", values(278.0), {}, "isobaricInhPa", 850.0},
      {0, "r", values(55.0), {}, "isobaricInhPa", 850.0},
      {0, "gh", values(1450.0), {}, "isobaricInhPa", 850.0},
      {3, "tp", values(2.5), {}, "surface", 0.0, "accum", 3},
  };
  eg::WriteRegularLatLonGrib2(grid, start, all_weather_fields, all_weather);
  eg::WriteRegularLatLonGrib2(grid, start,
                              {{0, "10u", values(4.0), {}},
                               {0, "10v", values(-2.0), {}},
                               {360, "10u", values(6.0), {}},
                               {360, "10v", values(-4.0), {}}},
                              long_weather);
  eg::WriteGrib1Currents({Current(grid, start, 0.0),
                          Current(grid, start + std::chrono::hours(3), 1.0)},
                         current_matching);
  eg::WriteGrib1Currents({Current(grid, start, 0.0),
                          Current(grid, start + std::chrono::hours(3), 1.0),
                          Current(grid, start + std::chrono::hours(6), 2.0)},
                         current_differing);
  eg::WriteGrib1Currents({Current(grid, start, 0.0),
                          Current(grid, start + std::chrono::hours(255), 1.0),
                          Current(grid, start + std::chrono::hours(256), 2.0),
                          Current(grid, start + std::chrono::hours(360), 3.0)},
                         long_current);

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

  eg::EnvironmentalMergeRequest all_request;
  all_request.weather = all_weather;
  all_request.current = current_differing;
  all_request.output = combined_all;
  all_request.overwrite = true;
  const auto all_result = eg::MergeEnvironmentalGribs(all_request);
  Check(all_result.success && all_result.output_message_count == 23,
        "all displayable weather fields combine with current records");
  Check(all_result.output_inspection["grib2_parameter_counts"]["0:6:1"]
                .asUInt64() == 1 &&
            all_result.output_inspection["grib2_parameter_counts"]["0:1:8"]
                    .asUInt64() == 1 &&
            all_result.output_inspection["grib2_parameter_counts"]["0:7:6"]
                    .asUInt64() == 1 &&
            all_result.output_inspection["grib2_parameter_counts"]["0:16:196"]
                    .asUInt64() == 1,
        "combined all-data fixture retains xGRIB weather identities");
  Check(all_result.output_inspection["current_component_counts"]["u_49"]
                    .asUInt64() == 3 &&
            all_result.output_inspection["current_component_counts"]["v_50"]
                    .asUInt64() == 3,
        "combined all-data fixture retains both current components");

  eg::EnvironmentalMergeRequest long_request;
  long_request.weather = long_weather;
  long_request.current = long_current;
  long_request.output = combined_long;
  long_request.overwrite = true;
  const auto long_result = eg::MergeEnvironmentalGribs(long_request);
  Check(long_result.success && long_result.output_message_count == 12 &&
            long_result.output_inspection["valid_times"].size() == 4 &&
            long_result.output_inspection["first_valid_time"].asString() ==
                "20260712T0000" &&
            long_result.output_inspection["last_valid_time"].asString() ==
                "20260727T0000" &&
            long_result.output_inspection["current_component_counts"]["u_49"]
                    .asUInt64() == 4 &&
            long_result.output_inspection["current_component_counts"]["v_50"]
                    .asUInt64() == 4,
        "combined weather/current output preserves 255, 256 and 360-hour "
        "current leads");

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
  manifest["fixtures"]["all_weather"] = all_weather.filename().string();
  manifest["fixtures"]["current_matching"] =
      current_matching.filename().string();
  manifest["fixtures"]["current_differing"] =
      current_differing.filename().string();
  manifest["fixtures"]["corrupt"] = corrupt.filename().string();
  manifest["fixtures"]["combined"] = combined.filename().string();
  manifest["fixtures"]["combined_all"] = combined_all.filename().string();
  manifest["fixtures"]["long_weather"] = long_weather.filename().string();
  manifest["fixtures"]["long_current"] = long_current.filename().string();
  manifest["fixtures"]["combined_long"] = combined_long.filename().string();
  WriteJson(root / "fixture-manifest.json", manifest);
  WriteJson(root / "merge-result.json",
            eg::EnvironmentalMergeResultJson(result));

  std::cout << "deterministic environmental GRIB merge verified in " << root
            << '\n';
  return 0;
}
