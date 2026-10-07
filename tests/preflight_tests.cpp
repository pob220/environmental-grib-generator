#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include "environmental_grib/preflight.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/model.h"
#include "environmental_grib/sources.h"

namespace eg = environmental_grib;
namespace {
int checks = 0;
void Check(bool value, const char* description) {
  ++checks;
  if (!value) throw std::runtime_error(description);
}
eg::EnvironmentRequest Request() {
  eg::EnvironmentRequest r;
  r.bbox = {179, -1, -179, 1};
  r.start = eg::ParseUtcDateTime("2026-10-07T21:00:00Z");
  r.hours = 12;
  r.step_hours = 6;
  r.include_waves = true;
  r.wave_provider = "copernicus_global_waves";
  r.wave_step_hours = 3;
  r.current_source = "synthetic";
  r.time_policy = "review";
  return r;
}
void Wind(const std::filesystem::path& path, const std::vector<int>& hours) {
  const auto r = Request();
  const auto grid = eg::BuildRegularGrid(r.bbox, 1.0);
  std::vector<eg::Grib2Field> fields;
  for (int hour : hours)
    for (const char* name : {"10u", "10v"})
      fields.push_back(
          {hour, name, std::vector<double>(grid.size(), 4.25), {}});
  eg::WriteRegularLatLonGrib2(grid, r.start, fields, path);
}
void Waves(const std::filesystem::path& path) {
  const auto r = Request();
  const auto grid = eg::BuildRegularGrid(r.bbox, 1.0);
  eg::WriteRegularLatLonGrib2(
      grid, r.start,
      {{9, "swh", std::vector<double>(grid.size(), 1.5), {}},
       {12, "swh", std::vector<double>(grid.size(), 1.5), {}}},
      path);
}
void Currents(const std::filesystem::path& path,
              const std::vector<int>& hours) {
  const auto r = Request();
  const auto grid = eg::BuildRegularGrid(r.bbox, 1.0);
  std::vector<eg::CurrentGrid> fields;
  for (int hour : hours)
    fields.push_back(eg::MakeSyntheticRotaryCurrent(
        r.bbox, r.start + std::chrono::hours(hour), grid));
  eg::WriteGrib1Currents(fields, path);
}
bool HasPolicy(const Json::Value& issue, const char* policy) {
  for (const auto& action : issue["actions"])
    if (action["request"]["timePolicy"] == policy) return true;
  return false;
}
}  // namespace
int main() {
  const auto root =
      std::filesystem::temp_directory_path() /
      ("xgrib-time-preflight-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    std::filesystem::create_directories(root);
    auto r = Request();
    Check(eg::PreflightEnvironment(r)["ready"].asBool(),
          "independent valid weather and wave intervals pass");
    for (const auto* provider :
         {"gfs", "noaa_hrrr", "dwd_icon_eu", "ukmo_ukv", "metno_nordic",
          "ecmwf_aifs_open", "ecmwf_ifs_open"}) {
      auto invalid = r;
      invalid.weather_provider = provider;
      invalid.step_hours = 2;
      const auto plan = eg::PreflightEnvironment(invalid);
      Check(!plan["ready"].asBool(), "unsupported cadence is detected locally");
      Check(!plan["issues"][0]["actions"].empty(), "valid remedies offered");
      for (const auto& action : plan["issues"][0]["actions"]) {
        auto fixed = invalid;
        eg::ApplyPreflightAction(fixed, action);
        Check(eg::PreflightEnvironment(fixed)["ready"].asBool(),
              "every offered settings remedy passes");
      }
    }
    r.wave_step_hours = 6;
    Check(!eg::PreflightEnvironment(r)["ready"].asBool(),
          "Copernicus 6h waves rejected upfront");
    for (bool dry : {false, true}) {
      r.output = root / "invalid.grb";
      r.dry_run = dry;
      int http_calls = 0;
      try {
        eg::GenerateEnvironment(r, [&](const std::string&, double) {
          ++http_calls;
          return std::vector<unsigned char>{};
        });
        Check(false, "invalid request must fail");
      } catch (const eg::PreflightError& error) {
        Check(error.issue()["code"] == "unsupported_time_settings",
              "structured early failure");
      }
      Check(http_calls == 0 && !std::filesystem::exists(r.output),
            "invalid requests perform no HTTP or output publication");
    }
    r = Request();
    r.hours = 13;
    Check(!eg::PreflightEnvironment(r)["ready"].asBool(),
          "duration alignment detected");
    r = Request();
    r.weather_provider = "noaa_hrrr";
    r.step_hours = 1;
    r.hours = 72;
    Check(!eg::PreflightEnvironment(r)["issues"][0]["actions"].empty(),
          "horizon reduction offered");
    r = Request();
    r.include_waves = false;
    r.weather_provider = "existing-file";
    r.weather_file = root / "does-not-exist";
    Check(eg::PreflightEnvironment(r)["ready"].asBool(),
          "instant check does not touch files");
    const auto before = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) (void)eg::PreflightEnvironment(Request());
    const double micros = std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - before)
                              .count() /
                          10000;
    std::cout << "Settings preflight mean: " << micros
              << " us (10000 iterations)\n";

    const auto weather = root / "weather.grb2", current = root / "current.grb",
               waves = root / "waves.grb2";
    Wind(weather, {0, 2, 4, 6});
    Currents(current, {0, 3, 6});
    Check(eg::ReadGribTimeInventory(current).count("parameter-49|surface|0") == 1 &&
              eg::ReadGribTimeInventory(current).count("parameter-50|surface|0") == 1,
          "standard current identities do not depend on ecCodes short-name aliases");
    eg::TimedGribInputs inputs{{"weather", weather}, {"current", current}};
    Check(!eg::PreflightGribTimes(inputs).isMember("code"),
          "2h weather / 3h currents with equal coverage need no prompt");
    r = Request();
    r.include_waves = false;
    r.weather_provider = "existing-file";
    r.current_source = "existing-file";
    r.weather_file = weather;
    r.current_file = current;
    r.step_hours = 2;
    r.hours = 6;
    r.output = root / "mixed.grb";
    r.overwrite = true;
    const auto mixed = eg::GenerateEnvironment(r);
    Check(mixed.message_count == 14,
          "valid mixed cadence retains all messages");
    Check(!mixed.diagnostics["warnings"].empty(),
          "merge warnings reach environment result");
    Check(mixed.inspection["first_valid_time"] == "20261007T2100" &&
              mixed.inspection["last_valid_time"] == "20261008T0300",
          "dateline and UTC midnight keep correct dates");

    Wind(weather, {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24});
    Currents(current, {6, 9, 12, 15, 18});
    const auto issue = eg::PreflightGribTimes(inputs);
    Check(issue["code"] == "partial_time_coverage",
          "partial coverage detected");
    Check(HasPolicy(issue, "shared-period") &&
              HasPolicy(issue, "common-times") && HasPolicy(issue, "keep-all"),
          "all viable temporal choices offered");
    const auto advisory = eg::ApplyGribTimePolicy(inputs, "review", root);
    Check(advisory["severity"] == "advisory" &&
              !advisory["requiresDecision"].asBool(),
          "usable partial coverage proceeds without a decision");
    Check(inputs[0].second == weather && inputs[1].second == current,
          "review keeps every original record for partial coverage");
    Check(advisory["coverageSummary"].asString().find("Eastward current") != std::string::npos,
          "advisory identifies individual limiting fields");
    auto selected = inputs;
    const auto common_period =
        eg::ApplyGribTimePolicy(selected, "shared-period", root);
    const auto wind_times = eg::ReadGribTimeInventory(selected[0].second);
    Check(wind_times.begin()->second.size() == 7,
          "shared period retains 2-hour wind intervals");
    Check(
        eg::ReadGribTimeInventory(selected[1].second).begin()->second.size() ==
            5,
        "shared period retains 3-hour currents");
    Check(!eg::PreflightGribTimes(selected).isMember("code"),
          "shared period resolves coverage problem");
    Check(
        eg::InspectGrib(selected[0].second)["messages"][0]["values"]["minimum"]
                .asDouble() ==
            eg::InspectGrib(weather)["messages"][0]["values"]["minimum"]
                .asDouble(),
        "filter preserves encoded field values");
    selected = inputs;
    eg::ApplyGribTimePolicy(selected, "common-times", root);
    Check(
        eg::ReadGribTimeInventory(selected[0].second).begin()->second.size() ==
            3,
        "common stored timestamps discard intermediate frames explicitly");
    Check(
        eg::ReadGribTimeInventory(selected[1].second).begin()->second.size() ==
            3,
        "common stored timestamps select same current frames");

    r.time_policy = "shared-period";
    r.hours = 24;
    r.output = root / "shared-period.grb";
    const auto shared_export = eg::GenerateEnvironment(r);
    Check(shared_export.message_count == 24,
          "shared-period choice generates an actual merged GRIB");
    Check(shared_export.inspection["first_valid_time"] == "20261008T0300" &&
              shared_export.inspection["last_valid_time"] == "20261008T1500",
          "shared-period output uses correct UTC dates");
    r.time_policy = "common-times";
    r.output = root / "common-times.grb";
    Check(eg::GenerateEnvironment(r).message_count == 12,
          "common-times choice generates an actual merged GRIB");
    r.time_policy = "keep-all";
    r.output = root / "keep-all.grb";
    const auto full_export = eg::GenerateEnvironment(r);
    Check(full_export.message_count == 36 &&
              !full_export.diagnostics["warnings"].empty(),
          "explicit partial coverage retains all records and reports it");
    r.time_policy = "review";
    r.output = root / "review-partial.grb";
    const auto reviewed_export = eg::GenerateEnvironment(r);
    Check(reviewed_export.message_count == 36 &&
              reviewed_export.diagnostics["time_coverage"]["severity"] == "advisory" &&
              !reviewed_export.diagnostics["warnings"].empty(),
          "GUI review policy generates partial coverage without trimming or blocking");

    Wind(weather, {0, 2, 4, 6});
    Currents(current, {1, 4, 7});
    const auto offset = eg::PreflightGribTimes(inputs);
    Check(!HasPolicy(offset, "shared-period") &&
              !HasPolicy(offset, "common-times"),
          "phase offsets do not invent a shared stored period from LCM");
    Check(!eg::ApplyGribTimePolicy(inputs, "review", root)["requiresDecision"].asBool(),
          "interpolatable offset timelines do not require identical stored times");
    Currents(current, {6, 9});
    Check(eg::PreflightGribTimes(inputs)["code"] == "instant_only_overlap",
          "endpoint-only overlap distinguished");
    Currents(current, {9, 12});
    Check(eg::PreflightGribTimes(inputs)["code"] == "no_time_overlap",
          "disjoint times detected");
    r.include_waves = true;
    r.wave_provider = "copernicus_global_waves";
    r.output = root / "disjoint.grb";
    int http_calls = 0;
    try {
      eg::GenerateEnvironment(r, [&](const std::string&, double) {
        ++http_calls;
        return std::vector<unsigned char>{};
      });
      Check(false, "existing inputs must be checked first");
    } catch (const eg::PreflightError&) {
      Check(http_calls == 0,
            "existing-input overlap checked before wave downloads");
    }
    const auto preferred = root / "preferred.grb2",
               fallback = root / "fallback.grb2";
    Wind(preferred, {0, 2, 4, 6});
    Wind(fallback, {6, 8, 10, 12});
    Currents(current, {0, 3, 6, 9, 12});
    eg::TimedGribInputs extended{{"weather-preferred", preferred},
                                 {"weather-fallback", fallback},
                                 {"current", current}};
    Check(!eg::PreflightGribTimes(extended).isMember("code"),
          "forecast extension components are reviewed as complete field "
          "timelines");
    // Reproduce the real UKV/GFS extension: MSL pressure has surface/0 in
    // converted UKV and meanSea/0 in GFS. They must form one coverage field.
    const auto grid = eg::BuildRegularGrid(Request().bbox, 1.0);
    auto pressure = [&](const std::filesystem::path& path,
                        const std::vector<int>& hours, const char* level_type,
                        const char* name = "prmsl") {
      std::vector<eg::Grib2Field> values;
      for (int hour : hours)
        values.push_back({hour, name, std::vector<double>(grid.size(), 101325.0),
                          {}, std::string(level_type), 0.0});
      eg::WriteRegularLatLonGrib2(grid, Request().start, values, path);
    };
    const auto short_pressure = root / "ukv-pressure.grb2",
               long_pressure = root / "gfs-pressure.grb2";
    pressure(short_pressure, {0, 3, 6}, "surface");
    pressure(long_pressure, {6, 9, 12}, "meanSea");
    auto pressure_extension = extended;
    pressure_extension.emplace_back("weather-preferred-pressure", short_pressure);
    pressure_extension.emplace_back("weather-fallback-pressure", long_pressure);
    Check(!eg::PreflightGribTimes(pressure_extension).isMember("code"),
          "equivalent UKV/GFS pressure encodings combine across fallback transition");
    Check(eg::ReadGribTimeInventory(short_pressure).begin()->first ==
              eg::ReadGribTimeInventory(long_pressure).begin()->first,
          "mean sea-level pressure level aliases share coverage identity");
    pressure(long_pressure, {6, 9, 12}, "surface", "sp");
    Check(eg::ReadGribTimeInventory(short_pressure).begin()->first !=
              eg::ReadGribTimeInventory(long_pressure).begin()->first,
          "surface pressure is not aliased to mean sea-level pressure");
    pressure(short_pressure, {20, 23}, "surface");
    const auto optional = eg::ApplyGribTimePolicy(pressure_extension, "review", root);
    Check(optional["severity"] == "advisory" &&
              optional["coverageSummary"].asString().find("Mean sea-level pressure") != std::string::npos,
          "disjoint optional fields do not veto usable weather/current coverage");
    Currents(current, {8, 10, 12});
    eg::ApplyGribTimePolicy(extended, "shared-period", root);
    Check(extended.size() == 2 &&
              !eg::PreflightGribTimes(extended).isMember("code"),
          "shared-period export omits an empty preferred segment without "
          "losing its component");
    Currents(current, {0, 3, 6});
    Waves(waves);
    inputs.emplace_back("waves", waves);
    Check(eg::PreflightGribTimes(inputs)["code"] == "no_time_overlap",
          "wave coverage participates in shared validation");
    Check(eg::PreflightGribTimes(inputs)["requiresDecision"].asBool(),
          "disjoint components still require an explicit decision");

    Wind(preferred, {0, 6, 120});
    Wind(fallback, {120, 240, 360});
    pressure(short_pressure, {0, 120}, "surface");
    pressure(long_pressure, {120, 360}, "meanSea");
    const eg::TimedGribInputs long_weather{{"weather-preferred", preferred},
        {"weather-fallback", fallback}, {"weather-preferred-pressure", short_pressure},
        {"weather-fallback-pressure", long_pressure}};
    const auto long_weather_path = root / "extended-360-weather.grb";
    eg::CompositeGribStreamsPreferFirst(long_weather, long_weather_path, true);
    Currents(current, {0, 120, 240, 360});
    r = Request();
    r.weather_provider = "existing-file";
    r.weather_file = long_weather_path;
    r.current_source = "existing-file";
    r.current_file = current;
    r.include_waves = false;
    r.hours = 360;
    r.step_hours = 3;
    r.output = root / "extended-360-reviewed.grb";
    const auto long_export = eg::GenerateEnvironment(r);
    Check(long_export.message_count == 22 &&
              !long_export.diagnostics["time_coverage"].isMember("code"),
          "360-hour UKV/GFS pressure fallback exports every message without prompting");
    Check(long_export.inspection["last_valid_time"] == "20261022T2100",
          "coverage review preserves all fifteen days rather than clipping to UKV horizon");
    std::filesystem::remove_all(root);
    std::cout << checks << " temporal checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << "; fixtures: " << root << '\n';
    return 1;
  }
}
