#include "environmental_grib/preflight.h"

#include <algorithm>
#include <numeric>
#include <sstream>
#include "environmental_grib/grib.h"
#include "environmental_grib/metno.h"
#include "environmental_grib/rtofs.h"
#include "environmental_grib/ukv.h"

namespace environmental_grib {
namespace {
Json::Value Action(const std::string& label, const Json::Value& patch) {
  Json::Value action(Json::objectValue);
  action["label"] = label;
  action["request"] = patch;
  return action;
}
Json::Value Issue(const std::string& code, const std::string& message) {
  Json::Value issue(Json::objectValue);
  issue["code"] = code;
  issue["message"] = message;
  issue["severity"] = "decision_required";
  issue["requiresDecision"] = true;
  issue["actions"] = Json::Value(Json::arrayValue);
  return issue;
}
void WeatherHours(const EnvironmentRequest& r, int hours, int step) {
  const auto& provider = r.weather_provider;
  if (provider == "none" || provider == "existing-file") return;
  if (provider == "ukmo_ukv") {
    UkvForecastHours(hours, step);
    return;
  }
  if (provider == "metno_nordic") {
    MetNoForecastHours(hours, step);
    return;
  }
  if (provider == "dwd_icon_eu") {
    DwdIconEuForecastHourSequence(hours, step);
    return;
  }
  if (provider == "noaa_hrrr") {
    HrrrForecastHourSequence(hours, step);
    return;
  }
  if (provider == "ecmwf_aifs_open" && step != 6 && step != 12)
    throw ValidationError(
        "ECMWF AIFS requires 6-hour or 12-hour weather intervals.");
  if (provider == "ecmwf_ifs_open" && step != 3 && step != 6 && step != 12)
    throw ValidationError(
        "ECMWF IFS requires 3-hour, 6-hour or 12-hour weather intervals.");
  if (provider == "gfs" && step != 1 && step != 3 && step != 6 && step != 12)
    throw ValidationError(
        "GFS requires 1-hour, 3-hour, 6-hour or 12-hour weather intervals.");
  ForecastHourSequence(hours, step);
}
void ValidateTimes(const EnvironmentRequest& r) {
  // Extension orchestration selects each provider's native interval and
  // horizon.
  if (r.hours < 0 || r.hours > 8784 || r.step_hours <= 0 ||
      r.hours % r.step_hours != 0)
    throw ValidationError(
        "Duration must be between 0 and 8784 hours and divisible by the "
        "selected interval.");
  if (!r.extend_forecast) WeatherHours(r, r.hours, r.step_hours);
  if (r.include_waves) {
    if (r.wave_provider == "copernicus_global_waves") {
      if (r.wave_step_hours != 3)
        throw ValidationError(
            "Copernicus Global Waves requires a separate 3-hour wave "
            "interval.");
      if (!r.extend_forecast && r.hours > 240)
        throw ValidationError(
            "Copernicus Global Waves covers at most 240 forecast hours.");
    } else if (r.wave_provider == "gfs_wave" && r.wave_step_hours != 1 &&
               r.wave_step_hours != 3 && r.wave_step_hours != 6 &&
               r.wave_step_hours != 12) {
      throw ValidationError(
          "GFS waves requires 1-hour, 3-hour, 6-hour or 12-hour intervals.");
    }
    if (r.wave_step_hours <= 0 || r.hours % r.wave_step_hours != 0)
      throw ValidationError(
          "Duration must also be divisible by the separate wave interval.");
  }
  if (!r.extend_forecast && r.current_source == "noaa_rtofs_global") {
    if (RtofsForecastHours(r.hours, r.step_hours).empty())
      throw ValidationError(
          "RTOFS starts at forecast hour 6; select at least 6 hours.");
  }
}
struct Timeline {
  std::string role;
  std::string field;
  std::set<TimePoint> times;
};
std::string FieldLabel(const std::string& key) {
  static const std::map<std::string, std::string> names{
      {"10u", "10 m eastward wind"}, {"10v", "10 m northward wind"},
      {"2t", "2 m temperature"}, {"prmsl", "Mean sea-level pressure"},
      {"swh", "Significant wave height"}, {"htsgw", "Significant wave height"},
      {"dirpw", "Wave direction"}, {"perpw", "Wave period"},
      {"mwd", "Mean wave direction"}, {"mwp", "Mean wave period"},
      {"parameter-49", "Eastward current"}, {"parameter-50", "Northward current"}};
  const auto found = names.find(key.substr(0, key.find('|')));
  return found == names.end() ? key : found->second;
}
std::string CoverageSummary(const Json::Value& coverage) {
  // Group identical ranges, retaining the identity of every limiting field.
  std::map<std::string, std::string> groups;
  for (const auto& entry : coverage) {
    const auto range = entry["component"].asString() + ": " +
        entry["fromUtc"].asString() + " to " + entry["throughUtc"].asString();
    auto& labels = groups[range];
    if (!labels.empty()) labels += ", ";
    labels += entry["fieldLabel"].asString();
  }
  std::string summary;
  for (const auto& [range, labels] : groups) {
    if (!summary.empty()) summary += '\n';
    summary += range + " (" + labels + ")";
  }
  return summary;
}
struct Coverage {
  Json::Value issue{Json::objectValue};
  std::set<TimePoint> common;
  bool problem{false};
};
Coverage Analyze(const TimedGribInputs& inputs) {
  Coverage result;
  std::vector<Timeline> fields;
  Json::Value coverage(Json::arrayValue);
  std::map<std::pair<std::string, std::string>, std::set<TimePoint>> combined;
  for (const auto& [label, path] : inputs) {
    const auto role = label.substr(0, label.find('-'));
    for (const auto& [name, times] : ReadGribTimeInventory(path))
      combined[{role, name}].insert(times.begin(), times.end());
  }
  for (auto& [identity, times] : combined) {
    const auto& [role, name] = identity;
    if (times.empty()) continue;
    Json::Value entry(Json::objectValue);
    entry["component"] = role;
    entry["field"] = name;
    entry["fieldLabel"] = FieldLabel(name);
    entry["fromUtc"] = FormatUtcDateTime(*times.begin());
    entry["throughUtc"] = FormatUtcDateTime(*times.rbegin());
    entry["records"] = Json::UInt64(times.size());
    coverage.append(entry);
    fields.push_back({role, name, std::move(times)});
  }
  if (fields.empty()) return result;
  auto first = *fields.front().times.begin();
  auto last = *fields.front().times.rbegin();
  auto full_first = first, full_last = last;
  std::map<std::string, std::pair<TimePoint, TimePoint>> components;
  bool partial = false;
  for (const auto& field : fields) {
    partial |= *field.times.begin() != *fields.front().times.begin() ||
               *field.times.rbegin() != *fields.front().times.rbegin();
    first = std::max(first, *field.times.begin());
    last = std::min(last, *field.times.rbegin());
    full_first = std::min(full_first, *field.times.begin());
    full_last = std::max(full_last, *field.times.rbegin());
    auto [component, inserted] = components.emplace(
        field.role, std::make_pair(*field.times.begin(), *field.times.rbegin()));
    if (!inserted) {
      component->second.first = std::min(component->second.first, *field.times.begin());
      component->second.second = std::max(component->second.second, *field.times.rbegin());
    }
    if (&field == &fields.front())
      result.common = field.times;
    else {
      std::set<TimePoint> intersection;
      std::set_intersection(result.common.begin(), result.common.end(),
                            field.times.begin(), field.times.end(),
                            std::inserter(intersection, intersection.end()));
      result.common = std::move(intersection);
    }
  }
  result.issue["coverage"] = coverage;
  auto component_first = components.begin()->second.first;
  auto component_last = components.begin()->second.second;
  for (const auto& [role, bounds] : components) {
    component_first = std::max(component_first, bounds.first);
    component_last = std::min(component_last, bounds.second);
  }
  // A shorter optional field must not veto an otherwise usable combination.
  // Assess blocking overlap between complete preferred/fallback components;
  // continue to report each individual field's shorter coverage accurately.
  result.problem = components.size() > 1 && component_first >= component_last;
  if (!partial && !result.problem) return result;
  const bool disjoint = result.problem && component_first > component_last;
  const bool instant_only = result.problem && component_first == component_last;
  result.issue = Issue(
      disjoint        ? "no_time_overlap"
      : instant_only ? "instant_only_overlap"
                      : "partial_time_coverage",
      disjoint ? "The selected components have no common UTC period. Change the "
                 "period or sources, or explicitly omit a component."
      : instant_only ? "The selected components overlap at only one UTC instant, "
                        "not a usable forecast period."
                      : "The combined forecast is usable; all available records are kept. "
                        "Some individual fields cover shorter UTC periods and are "
                        "unavailable outside those periods.");
  result.issue["severity"] = result.problem ? "decision_required" : "advisory";
  result.issue["requiresDecision"] = result.problem;
  result.issue["coverage"] = coverage;
  result.issue["coverageSummary"] = CoverageSummary(coverage);
  result.issue["componentSharedFromUtc"] = FormatUtcDateTime(component_first);
  result.issue["componentSharedThroughUtc"] = FormatUtcDateTime(component_last);
  result.issue["sharedFromUtc"] = FormatUtcDateTime(first);
  result.issue["sharedThroughUtc"] = FormatUtcDateTime(last);
  if (result.common.size() >= 2) {
    const auto kept = std::chrono::duration_cast<std::chrono::hours>(
        *result.common.rbegin() - *result.common.begin()).count();
    const auto span = std::chrono::duration_cast<std::chrono::hours>(
        full_last - full_first).count();
    Json::Value patch(Json::objectValue);
    patch["timePolicy"] = "shared-period";
    result.issue["actions"].append(Action(
        "Shorten to shared period " + FormatUtcDateTime(*result.common.begin()) +
            " to " + FormatUtcDateTime(*result.common.rbegin()) +
            " (keep " + std::to_string(kept) + "h of " + std::to_string(span) +
            "h stored span; discard records outside this period; retain native intervals)",
        patch));
    patch["timePolicy"] = "common-times";
    result.issue["actions"].append(
        Action("Keep only common stored times (" +
                   std::to_string(result.common.size()) +
                   " timestamps; discards intermediate records)",
               patch));
  }
  if (!disjoint) {
    Json::Value patch(Json::objectValue);
    patch["timePolicy"] = "keep-all";
    result.issue["actions"].append(
        Action("Keep all records with partial field coverage", patch));
  }
  // Dropping a component is always explicit, never an automatic repair.
  std::set<std::string> roles;
  for (const auto& input : inputs)
    roles.insert(input.first.substr(0, input.first.find('-')));
  if (roles.size() > 1)
    for (const auto& role : roles) {
      Json::Value patch(Json::objectValue);
      if (role == "weather")
        patch["weatherProvider"] = "none";
      else if (role == "current")
        patch["currentSource"] = "none";
      else if (role == "waves")
        patch["includeWaves"] = false;
      else
        continue;
      result.issue["actions"].append(
          Action("Omit " + role + " and check again", patch));
    }
  return result;
}
}  // namespace

Json::Value PreflightEnvironment(const EnvironmentRequest& request) {
  Json::Value result(Json::objectValue);
  result["schemaVersion"] = 1;
  result["issues"] = Json::Value(Json::arrayValue);
  try {
    ValidateTimes(request);
  } catch (const ValidationError& error) {
    auto issue = Issue("unsupported_time_settings", error.what());
    // Offer only combinations accepted by exactly the same local validation.
    std::set<std::pair<int, int>> offered;
    for (int step : {request.step_hours, 1, 3, 6, 12}) {
      if (step <= 0 || step > 24) continue;
      auto candidate = request;
      candidate.step_hours = step;
      if (candidate.include_waves &&
          candidate.wave_provider == "copernicus_global_waves")
        candidate.wave_step_hours = 3;
      if (candidate.include_waves &&
          (candidate.wave_step_hours <= 0 || candidate.wave_step_hours > 24))
        candidate.wave_step_hours = 3;
      const int wave_step =
          candidate.include_waves ? candidate.wave_step_hours : 1;
      const int alignment = std::lcm(step, wave_step);
      int horizon = 384;
      if (!request.extend_forecast) {
        if (request.weather_provider == "noaa_hrrr") horizon = 48;
        if (request.weather_provider == "ukmo_ukv" ||
            request.weather_provider == "dwd_icon_eu")
          horizon = 120;
        if (request.weather_provider == "metno_nordic") horizon = 56;
        if (request.current_source == "noaa_rtofs_global")
          horizon = std::min(horizon, 192);
        if (request.include_waves &&
            request.wave_provider == "copernicus_global_waves")
          horizon = std::min(horizon, 240);
      }
      const int bounded_hours = std::clamp(request.hours, 0, horizon);
      for (int hours :
           {bounded_hours, bounded_hours - bounded_hours % alignment,
            bounded_hours +
                (alignment - bounded_hours % alignment) % alignment}) {
        if (hours <= 0 || hours > 384 || !offered.emplace(step, hours).second)
          continue;
        candidate.hours = hours;
        try {
          ValidateTimes(candidate);
        } catch (const ValidationError&) {
          continue;
        }
        Json::Value patch(Json::objectValue);
        patch["stepHours"] = step;
        patch["waveStepHours"] = candidate.wave_step_hours;
        patch["hours"] = hours;
        issue["actions"].append(Action(
            "Use " + std::to_string(hours) +
                " hours, weather/current request interval " +
                std::to_string(step) + "h" +
                (candidate.include_waves
                     ? ", waves " + std::to_string(candidate.wave_step_hours) +
                           "h"
                     : ""),
            patch));
        if (issue["actions"].size() >= 6) break;
      }
      if (issue["actions"].size() >= 6) break;
    }
    result["issues"].append(issue);
  }
  result["ready"] = result["issues"].empty();
  result["availabilityChecked"] = false;
  return result;
}

Json::Value PreflightGribTimes(const TimedGribInputs& inputs) {
  return Analyze(inputs).issue;
}

Json::Value ApplyGribTimePolicy(TimedGribInputs& inputs,
                                const std::string& policy,
                                const std::filesystem::path& workspace) {
  if (policy != "review" && policy != "keep-all" && policy != "shared-period" &&
      policy != "common-times")
    throw ValidationError("unsupported timePolicy: " + policy);
  const auto coverage = Analyze(inputs);
  if (coverage.problem && policy == "review")
    throw PreflightError(coverage.issue);
  Json::Value diagnostics = coverage.issue;
  diagnostics["policy"] = policy;
  if (policy == "shared-period" || policy == "common-times") {
    if (coverage.common.size() < 2)
      throw PreflightError(
          coverage.problem
              ? coverage.issue
              : Issue("no_common_stored_period",
                      "There are fewer than two common stored timestamps. "
                      "Change settings or retain native intervals."));
    const std::set<TimePoint> selected =
        policy == "common-times" ? coverage.common : std::set<TimePoint>{};
    TimedGribInputs filtered;
    for (const auto& [label, path] : inputs) {
      const auto target = workspace / (label + "-time-selected.grb");
      if (FilterGribTimes(path, target, *coverage.common.begin(),
                          *coverage.common.rbegin(), selected))
        filtered.emplace_back(label, target);
    }
    if (filtered.empty())
      throw ValidationError("time selection leaves no GRIB records");
    inputs = std::move(filtered);
    diagnostics["outputFromUtc"] = FormatUtcDateTime(*coverage.common.begin());
    diagnostics["outputThroughUtc"] =
        FormatUtcDateTime(*coverage.common.rbegin());
  }
  return diagnostics;
}

void ApplyPreflightAction(EnvironmentRequest& r, const Json::Value& action) {
  const auto& patch = action["request"];
  if (patch.isMember("hours")) r.hours = patch["hours"].asInt();
  if (patch.isMember("stepHours")) r.step_hours = patch["stepHours"].asInt();
  if (patch.isMember("waveStepHours"))
    r.wave_step_hours = patch["waveStepHours"].asInt();
  if (patch.isMember("timePolicy"))
    r.time_policy = patch["timePolicy"].asString();
  if (patch.isMember("weatherProvider"))
    r.weather_provider = patch["weatherProvider"].asString();
  if (patch.isMember("currentSource"))
    r.current_source = patch["currentSource"].asString();
  if (patch.isMember("includeWaves"))
    r.include_waves = patch["includeWaves"].asBool();
}
}  // namespace environmental_grib
