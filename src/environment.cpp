#include "environmental_grib/environment.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unistd.h>

#include "environmental_grib/error.h"
#include "environmental_grib/copernicus.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/netcdf.h"
#include "environmental_grib/providers.h"
#include "environmental_grib/remote_currents.h"
#include "environmental_grib/rtofs.h"
#include "environmental_grib/sources.h"
#include "environmental_grib/tpxo.h"
#include "environmental_grib/ukv.h"
#include "environmental_grib/waves.h"
#include "environmental_grib/xtd.h"
#include "environmental_grib/xtd_package.h"

namespace environmental_grib {
namespace {

class Workspace {
public:
  explicit Workspace(const std::filesystem::path& output, bool keep)
      : keep_(keep) {
    path_ = output.parent_path().empty() ? std::filesystem::current_path()
                                         : output.parent_path();
    path_ /= ".environmental-grib-" + std::to_string(::getpid());
    std::filesystem::create_directories(path_);
  }
  ~Workspace() {
    if (!keep_) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }
  std::filesystem::path File(const std::string& name) const {
    return path_ / name;
  }

private:
  std::filesystem::path path_;
  bool keep_{};
};

constexpr auto kResumeLifetime = std::chrono::hours(3);

std::string FingerprintHash(const std::string& value) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  std::ostringstream output;
  output << std::hex << std::setw(16) << std::setfill('0') << hash;
  return output.str();
}

Json::Value ReadJson(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw ValidationError("cannot open resume metadata: " + path.string());
  Json::CharReaderBuilder builder;
  Json::Value value;
  std::string errors;
  if (!Json::parseFromStream(builder, input, &value, &errors))
    throw ValidationError("invalid resume metadata: " + errors);
  return value;
}

void WriteJsonAtomic(const std::filesystem::path& path,
                     const Json::Value& value) {
  const auto temporary = path.string() + ".tmp-" + std::to_string(::getpid());
  try {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output)
        throw ValidationError("cannot create resume metadata: " + temporary);
      output << Json::writeString(builder, value) << '\n';
      if (!output)
        throw ValidationError("cannot write resume metadata: " + temporary);
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::rename(temporary, path);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

class ResumeCache {
public:
  ResumeCache(const std::filesystem::path& output, ProgressCallback progress)
      : progress_(std::move(progress)) {
    root_ = output.parent_path().empty() ? std::filesystem::current_path()
                                         : output.parent_path();
    root_ /= ".environmental-grib-resume";
  }

  struct Restored {
    std::filesystem::path path;
    std::optional<std::string> selected_cycle;
  };

  std::optional<Restored> Restore(const std::string& stage,
                                  const std::string& fingerprint,
                                  const std::filesystem::path& target) {
    const auto [data, metadata] = Paths(stage, fingerprint);
    const bool have_data = std::filesystem::is_regular_file(data);
    const bool have_metadata = std::filesystem::is_regular_file(metadata);
    if (have_data != have_metadata) {
      RemoveFiles(data, metadata);
      return std::nullopt;
    }
    if (!have_data) return std::nullopt;
    try {
      const auto value = ReadJson(metadata);
      const auto created = value["createdEpochSeconds"].asInt64();
      const auto age =
          std::chrono::system_clock::now() -
          std::chrono::system_clock::time_point{std::chrono::seconds(created)};
      if (value["schemaVersion"].asInt() != 1 ||
          value["stage"].asString() != stage ||
          value["fingerprint"].asString() != fingerprint ||
          age < std::chrono::seconds::zero() || age > kResumeLifetime) {
        RemoveFiles(data, metadata);
        return std::nullopt;
      }
      const auto scan = ScanGribMessages(data);
      if (scan.message_count == 0 ||
          scan.message_count != value["messageCount"].asUInt64() ||
          scan.byte_count != value["byteCount"].asUInt64()) {
        RemoveFiles(data, metadata);
        return std::nullopt;
      }
      std::filesystem::copy_file(
          data, target, std::filesystem::copy_options::overwrite_existing);
      Json::Value details(Json::objectValue);
      details["stage"] = stage;
      details["messageCount"] = Json::UInt64(scan.message_count);
      details["byteCount"] = Json::UInt64(scan.byte_count);
      details["detail"] =
          "validated completed " + stage + " from prior failed job";
      if (progress_) progress_("reusing completed " + stage, details);
      touched_.emplace_back(data, metadata);
      Restored result{target, std::nullopt};
      if (value.isMember("selectedCycle"))
        result.selected_cycle = value["selectedCycle"].asString();
      return result;
    } catch (const std::exception&) {
      RemoveFiles(data, metadata);
      return std::nullopt;
    }
  }

  void Save(const std::string& stage, const std::string& fingerprint,
            const std::filesystem::path& source,
            const std::optional<std::string>& selected_cycle = std::nullopt) {
    const auto [data, metadata] = Paths(stage, fingerprint);
    const auto temporary = data.string() + ".tmp-" + std::to_string(::getpid());
    try {
      const auto scan = ScanGribMessages(source);
      if (scan.message_count == 0) return;
      std::filesystem::create_directories(root_);
      std::filesystem::copy_file(
          source, temporary, std::filesystem::copy_options::overwrite_existing);
      std::error_code ignored;
      std::filesystem::remove(data, ignored);
      std::filesystem::rename(temporary, data);
      Json::Value value(Json::objectValue);
      value["schemaVersion"] = 1;
      value["stage"] = stage;
      value["fingerprint"] = fingerprint;
      value["createdEpochSeconds"] =
          Json::Int64(std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count());
      value["messageCount"] = Json::UInt64(scan.message_count);
      value["byteCount"] = Json::UInt64(scan.byte_count);
      if (selected_cycle) value["selectedCycle"] = *selected_cycle;
      WriteJsonAtomic(metadata, value);
      touched_.emplace_back(data, metadata);
      Json::Value details(Json::objectValue);
      details["stage"] = stage;
      details["messageCount"] = Json::UInt64(scan.message_count);
      details["byteCount"] = Json::UInt64(scan.byte_count);
      details["detail"] = "saved completed " + stage + " for failure recovery";
      if (progress_) progress_("checkpointing completed " + stage, details);
    } catch (const std::exception& error) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      Json::Value details(Json::objectValue);
      details["stage"] = stage;
      details["detail"] =
          "generation will continue, but this stage could not be checkpointed";
      details["error"] = error.what();
      if (progress_) progress_("checkpoint unavailable", details);
    }
  }

  void Complete() {
    for (const auto& [data, metadata] : touched_) RemoveFiles(data, metadata);
    std::error_code ignored;
    std::filesystem::remove(root_, ignored);
    touched_.clear();
  }

private:
  std::pair<std::filesystem::path, std::filesystem::path> Paths(
      const std::string& stage, const std::string& fingerprint) const {
    const auto base = stage + "-" + FingerprintHash(fingerprint);
    return {root_ / (base + ".grb"), root_ / (base + ".json")};
  }

  static void RemoveFiles(const std::filesystem::path& data,
                          const std::filesystem::path& metadata) {
    std::error_code ignored;
    std::filesystem::remove(data, ignored);
    std::filesystem::remove(metadata, ignored);
  }

  std::filesystem::path root_;
  ProgressCallback progress_;
  std::vector<std::pair<std::filesystem::path, std::filesystem::path>> touched_;
};

std::string CommonFingerprint(const EnvironmentRequest& request) {
  std::ostringstream value;
  value
      << std::setprecision(17) << request.bbox.west << ',' << request.bbox.south
      << ',' << request.bbox.east << ',' << request.bbox.north << '|'
      << FormatUtcDateTime(request.start) << '|' << request.hours << '|'
      << std::filesystem::absolute(request.output).lexically_normal().string();
  return value.str();
}

std::string WeatherFingerprint(const EnvironmentRequest& request) {
  std::ostringstream value;
  value << "weather|" << CommonFingerprint(request) << '|'
        << request.weather_provider << '|' << request.step_hours << '|'
        << request.cycle << '|' << request.date.value_or("") << '|'
        << request.weather_preset << '|' << std::setprecision(17)
        << request.weather_grid_spacing_deg;
  return value.str();
}

std::string WaveFingerprint(const EnvironmentRequest& request,
                            const std::optional<std::string>& coupled_cycle) {
  std::ostringstream value;
  value << "waves|" << CommonFingerprint(request) << '|'
        << request.wave_provider << '|' << request.wave_step_hours << '|'
        << coupled_cycle.value_or("independent-auto");
  return value.str();
}

std::string CurrentFingerprint(const EnvironmentRequest& request,
                               const std::string& current_source) {
  std::ostringstream value;
  value << "current|" << CommonFingerprint(request) << '|' << current_source
        << '|' << request.step_hours << '|' << std::setprecision(17)
        << request.current_grid_spacing_deg << '|' << request.cycle << '|'
        << request.date.value_or("");
  return value.str();
}

std::filesystem::path CopyValidated(const std::filesystem::path& source,
                                    const std::filesystem::path& target) {
  if (!std::filesystem::is_regular_file(source))
    throw ValidationError("input GRIB not found: " + source.string());
  ScanGribMessages(source);
  std::filesystem::copy_file(source, target,
                             std::filesystem::copy_options::overwrite_existing);
  return target;
}

void Report(const ProgressCallback& callback, const std::string& stage,
            const std::string& detail) {
  if (!callback) return;
  Json::Value value;
  value["detail"] = detail;
  callback(stage, value);
}

std::string ResolveCurrentSource(const EnvironmentRequest& request) {
  if (request.current_source != "auto") return request.current_source;
  ProviderRegistry registry;
  const auto* selected =
      SelectBestProviderForBbox(request.bbox, request.hours, registry);
  if (!selected)
    throw ValidationError(
        "no current provider supports the requested bbox/duration");
  return selected->id;
}

int AlignDown(int hours, int step) {
  return step > 0 ? hours - hours % step : hours;
}

int AlignUp(int hours, int step) {
  if (step <= 0 || hours % step == 0) return hours;
  return hours + step - hours % step;
}

int WeatherHorizon(const std::string& provider, int requested) {
  if (provider == "ukmo_ukv" || provider == "dwd_icon_eu")
    return std::min(requested, 120);
  if (provider == "noaa_hrrr") return std::min(requested, 48);
  return requested;
}

int WeatherStep(const std::string& provider, int requested, bool fallback) {
  if (provider == "noaa_hrrr") return 1;
  if (provider == "ukmo_ukv") return requested == 1 ? 1 : 3;
  if (provider == "dwd_icon_eu") return requested == 1 ? 1 : 3;
  if (provider == "ecmwf_aifs_open") return 6;
  if (provider == "ecmwf_ifs_open") return 3;
  if (provider == "gfs" && fallback) return 3;
  return requested;
}

int WaveHorizon(const std::string& provider, int requested) {
  if (provider == "copernicus_global_waves")
    return std::min(requested, 240);
  return requested;
}

int CurrentHorizon(const std::string& source, int requested) {
  if (source == "marine_ie_irish_sea") return std::min(requested, 72);
  if (source == "noaa_rtofs_global") return std::min(requested, 192);
  if (source == "copernicus_nws" || source == "copernicus_global" ||
      source == "copernicus_ibi" ||
      source == "copernicus_mediterranean")
    return std::min(requested, 240);
  return requested;
}

constexpr int kAutoCycleAllowanceHours = 24;

std::optional<TimePoint> InspectionTime(const Json::Value& inspection,
                                        const char* key) {
  if (!inspection.isMember(key) || !inspection[key].isString())
    return std::nullopt;
  const std::string value = inspection[key].asString();
  if (value.size() != 13 || value[8] != 'T') return std::nullopt;
  return ParseUtcDateTime(value.substr(0, 4) + "-" + value.substr(4, 2) +
                          "-" + value.substr(6, 2) + "T" +
                          value.substr(9, 2) + ":" + value.substr(11, 2) +
                          ":00Z");
}

struct ComponentRun {
  bool complete{false};
  std::optional<TimePoint> first_valid;
  std::optional<TimePoint> last_valid;
};

bool CoversRequestedWindow(const ComponentRun& run,
                           const EnvironmentRequest& request) {
  const auto requested_end = request.start + std::chrono::hours(request.hours);
  return run.complete && run.first_valid && run.last_valid &&
         *run.first_valid <= request.start && *run.last_valid >= requested_end;
}

int ThroughHour(const ComponentRun& run, const EnvironmentRequest& request,
                int fallback) {
  if (!run.last_valid) return fallback;
  return static_cast<int>(std::chrono::duration_cast<std::chrono::hours>(
                              *run.last_valid - request.start)
                              .count());
}

EnvironmentRequest SingleComponentRequest(const EnvironmentRequest& request,
                                          const std::filesystem::path& output) {
  EnvironmentRequest child = request;
  child.extend_forecast = false;
  child.fallback_weather_provider = "none";
  child.fallback_wave_provider = "none";
  child.fallback_current_source = "none";
  child.weather_provider = "none";
  child.include_waves = false;
  child.current_source = "none";
  child.output = output;
  child.overwrite = true;
  child.keep_intermediate = request.keep_intermediate;
  return child;
}

void AddCoverage(Json::Value& coverage, const std::string& component,
                 const std::string& role, const std::string& source,
                 int through_hour, const std::string& status,
                 const std::string& detail = {}) {
  Json::Value item(Json::objectValue);
  item["role"] = role;
  item["source"] = source;
  item["through_hour"] = through_hour;
  item["status"] = status;
  if (!detail.empty()) item["detail"] = detail;
  coverage[component].append(item);
}

EnvironmentResult GenerateExtendedEnvironment(
    const EnvironmentRequest& request, HttpGet http_get,
    std::optional<TimePoint> now, ProgressCallback progress) {
  Workspace workspace(request.output, request.keep_intermediate);
  std::vector<std::pair<std::string, std::filesystem::path>> streams;
  std::vector<std::filesystem::path> inputs;
  Json::Value diagnostics(Json::objectValue);
  Json::Value& coverage = diagnostics["forecast_extension"]["coverage"];
  diagnostics["forecast_extension"]["enabled"] = true;
  diagnostics["forecast_extension"]["requested_hours"] = request.hours;
  std::optional<std::string> selected_cycle;

  auto run = [&](const std::string& label, EnvironmentRequest child,
                 const std::string& component, const std::string& role,
                 const std::string& source, int through_hour,
                 bool may_fallback) -> ComponentRun {
    if (request.dry_run) {
      AddCoverage(coverage, component, role, source, through_hour, "planned");
      coverage[component][coverage[component].size() - 1]
              ["model_lead_hours_requested"] = child.hours;
      return {true, std::nullopt, std::nullopt};
    }
    try {
      Report(progress, "generating " + component + " " + role,
             source + " using model leads through hour " +
                 std::to_string(child.hours));
      const auto result = GenerateEnvironment(child, http_get, now, progress);
      streams.emplace_back(label, result.output);
      inputs.push_back(result.output);
      if (!selected_cycle && result.selected_cycle)
        selected_cycle = result.selected_cycle;
      ComponentRun completed{true,
                             InspectionTime(result.inspection,
                                            "first_valid_time"),
                             InspectionTime(result.inspection,
                                            "last_valid_time")};
      AddCoverage(coverage, component, role, source,
                  ThroughHour(completed, request, through_hour), "complete");
      Json::Value& item = coverage[component][coverage[component].size() - 1];
      item["model_lead_hours_requested"] = child.hours;
      if (completed.first_valid)
        item["first_valid_time"] = FormatUtcDateTime(*completed.first_valid);
      if (completed.last_valid)
        item["last_valid_time"] = FormatUtcDateTime(*completed.last_valid);
      return completed;
    } catch (const std::exception& error) {
      AddCoverage(coverage, component, role, source, through_hour, "failed",
                  error.what());
      if (!may_fallback) throw;
      Report(progress, component + " preferred source unavailable",
             source + ": " + error.what() + "; using selected fallback");
      return {};
    }
  };

  if (request.weather_provider != "none") {
    const int primary_step =
        WeatherStep(request.weather_provider, request.step_hours, false);
    const int weather_lead_target = AlignUp(
        request.hours + (request.weather_provider == "existing-file"
                             ? 0
                             : kAutoCycleAllowanceHours),
        primary_step);
    const int primary_hours = AlignDown(
        WeatherHorizon(request.weather_provider, weather_lead_target),
        primary_step);
    auto primary = SingleComponentRequest(
        request, workspace.File("extended-weather-preferred.grb"));
    primary.weather_provider = request.weather_provider;
    primary.hours = primary_hours;
    primary.step_hours = primary_step;
    const bool have_fallback =
        request.fallback_weather_provider != "none" &&
        request.fallback_weather_provider != request.weather_provider;
    const ComponentRun primary_result = run(
        "weather-preferred", primary, "weather", "preferred",
        request.weather_provider, primary_hours, have_fallback);
    const bool weather_needs_fallback =
        request.dry_run ? primary_hours < weather_lead_target
                        : !CoversRequestedWindow(primary_result, request);
    if (weather_needs_fallback && have_fallback) {
      const int fallback_step = WeatherStep(
          request.fallback_weather_provider, request.step_hours, true);
      const int fallback_hours = AlignUp(
          request.hours + kAutoCycleAllowanceHours, fallback_step);
      auto fallback = SingleComponentRequest(
          request, workspace.File("extended-weather-fallback.grb"));
      fallback.weather_provider = request.fallback_weather_provider;
      fallback.hours = fallback_hours;
      fallback.step_hours = fallback_step;
      const ComponentRun fallback_result =
          run("weather-fallback", fallback, "weather", "fallback",
              request.fallback_weather_provider, request.hours, false);
      if (!request.dry_run &&
          !CoversRequestedWindow(fallback_result, request))
        throw ValidationError(
            "selected weather fallback does not cover the requested UTC "
            "window");
    } else if (weather_needs_fallback) {
      throw ValidationError(
          "preferred weather source does not cover the requested UTC window "
          "and no distinct weather fallback is selected");
    }
  }

  if (request.include_waves) {
    const int primary_step = std::max(1, request.wave_step_hours);
    const int wave_lead_target = AlignUp(
        request.hours + (request.wave_provider == "gfs_wave"
                             ? kAutoCycleAllowanceHours
                             : 0),
        primary_step);
    const int primary_hours = AlignDown(
        WaveHorizon(request.wave_provider, wave_lead_target), primary_step);
    auto primary = SingleComponentRequest(
        request, workspace.File("extended-waves-preferred.grb"));
    primary.include_waves = true;
    primary.wave_provider = request.wave_provider;
    primary.hours = primary_hours;
    primary.step_hours = primary_step;
    primary.wave_step_hours = primary_step;
    const bool have_fallback =
        request.fallback_wave_provider != "none" &&
        request.fallback_wave_provider != request.wave_provider;
    const ComponentRun primary_result =
        run("waves-preferred", primary, "waves", "preferred",
            request.wave_provider, primary_hours, have_fallback);
    const bool waves_need_fallback =
        request.dry_run ? primary_hours < wave_lead_target
                        : !CoversRequestedWindow(primary_result, request);
    if (waves_need_fallback && have_fallback) {
      const int fallback_step = 3;
      const int fallback_hours = AlignUp(
          request.hours + kAutoCycleAllowanceHours, fallback_step);
      auto fallback = SingleComponentRequest(
          request, workspace.File("extended-waves-fallback.grb"));
      fallback.include_waves = true;
      fallback.wave_provider = request.fallback_wave_provider;
      fallback.hours = fallback_hours;
      fallback.step_hours = fallback_step;
      fallback.wave_step_hours = fallback_step;
      const ComponentRun fallback_result =
          run("waves-fallback", fallback, "waves", "fallback",
              request.fallback_wave_provider, request.hours, false);
      if (!request.dry_run &&
          !CoversRequestedWindow(fallback_result, request))
        throw ValidationError(
            "selected wave fallback does not cover the requested UTC window");
    } else if (waves_need_fallback) {
      throw ValidationError(
          "preferred wave source does not cover the requested UTC window and "
          "no distinct wave fallback is selected");
    }
  }

  if (request.current_source != "none") {
    const int primary_step = std::max(1, request.step_hours);
    const int primary_hours = AlignDown(
        CurrentHorizon(request.current_source, request.hours), primary_step);
    auto primary = SingleComponentRequest(
        request, workspace.File("extended-current-preferred.grb"));
    primary.current_source = request.current_source;
    primary.hours = primary_hours;
    primary.step_hours = primary_step;
    const bool have_fallback =
        request.fallback_current_source != "none" &&
        request.fallback_current_source != request.current_source;
    const ComponentRun primary_result =
        run("current-preferred", primary, "current", "preferred",
            request.current_source, primary_hours, have_fallback);
    const bool currents_need_fallback =
        request.dry_run ? primary_hours < request.hours
                        : !CoversRequestedWindow(primary_result, request);
    if (currents_need_fallback && have_fallback) {
      const int fallback_hours = AlignUp(request.hours, primary_step);
      auto fallback = SingleComponentRequest(
          request, workspace.File("extended-current-fallback.grb"));
      fallback.current_source = request.fallback_current_source;
      fallback.hours = fallback_hours;
      fallback.step_hours = primary_step;
      const ComponentRun fallback_result =
          run("current-fallback", fallback, "current", "fallback",
              request.fallback_current_source, request.hours, false);
      if (!request.dry_run &&
          !CoversRequestedWindow(fallback_result, request))
        throw ValidationError(
            "selected current fallback does not cover the requested UTC "
            "window");
    } else if (currents_need_fallback) {
      throw ValidationError(
          "preferred current source does not cover the requested UTC window "
          "and no distinct current fallback is selected");
    }
  }

  if (request.dry_run) {
    return {request.output,
            0,
            0,
            request.weather_provider + "->" +
                request.fallback_weather_provider,
            request.include_waves
                ? request.wave_provider + "->" +
                      request.fallback_wave_provider
                : "none",
            request.current_source + "->" + request.fallback_current_source,
            std::nullopt,
            {},
            Json::Value(Json::objectValue),
            diagnostics};
  }
  if (streams.empty())
    throw ValidationError("forecast extension produced no environmental data");

  Report(progress, "compositing extended environmental GRIB",
         "preferred model records take priority over fallback records");
  const auto merged = CompositeGribStreamsPreferFirst(
      streams, request.output, request.overwrite, std::nullopt,
      request.start + std::chrono::hours(request.hours));
  diagnostics["forecast_extension"]["output_message_count"] =
      Json::UInt64(merged.output_message_count);
  return {request.output,
          merged.output_message_count,
          merged.byte_count,
          request.weather_provider + "->" +
              request.fallback_weather_provider,
          request.include_waves
              ? request.wave_provider + "->" + request.fallback_wave_provider
              : "none",
          request.current_source + "->" + request.fallback_current_source,
          selected_cycle,
          inputs,
          merged.inspection,
          diagnostics};
}

}  // namespace

EnvironmentResult GenerateEnvironment(const EnvironmentRequest& request,
                                      HttpGet http_get,
                                      std::optional<TimePoint> now,
                                      ProgressCallback progress) {
  request.bbox.Validate();
  BuildTimeSequence(request.start, request.hours, request.step_hours);
  if (request.output.empty())
    throw ValidationError("environment output path is required");
  if (std::filesystem::exists(request.output) && !request.overwrite)
    throw ValidationError(
        "output already exists; enable overwrite to replace it");
  if (request.weather_provider == "none" && request.current_source == "none" &&
      !request.include_waves)
    throw ValidationError(
        "at least one weather, wave, or current source must be enabled");
  if (request.extend_forecast)
    return GenerateExtendedEnvironment(request, std::move(http_get), now,
                                       std::move(progress));
  const std::string current_source = ResolveCurrentSource(request);
  if (current_source == "tpxo-cache") {
    if (!request.input_cache)
      throw ValidationError("tpxo-cache current source requires input-cache");
    bool cache_needs_preparation =
        !std::filesystem::is_regular_file(*request.input_cache);
    if (!cache_needs_preparation) {
      try {
        LoadTpxoCache(*request.input_cache);
      } catch (const ValidationError&) {
        cache_needs_preparation = true;
      }
    }
    if (cache_needs_preparation) {
      if (!request.auto_prepare_tpxo_cache)
        throw ValidationError("TPXO cache is missing or incompatible: " +
                              request.input_cache->string());
      if (!request.tpxo_model_directory)
        throw ValidationError(
            "automatic TPXO cache preparation requires tpxoModelDirectory");
      ResolveTpxo10AtlasDirectory(*request.tpxo_model_directory);
    }
  } else if (current_source == "tpxo") {
    if (!request.tpxo_model_directory)
      throw ValidationError("tpxo current source requires model-dir");
    ResolveTpxo10AtlasDirectory(*request.tpxo_model_directory);
  } else if (current_source == "offline-tidal") {
    if (!request.offline_tidal_file) {
      throw ValidationError(
          "offline-tidal current source requires offlineTidalFile");
    }
    XtdPackageReader package(*request.offline_tidal_file);
    const auto mode = ParseOfflineCurrentMode(request.offline_current_mode);
    if (mode == OfflineCurrentMode::kTideAndExpectedSeasonalCirculation &&
        !package.status().climatology_available) {
      throw ValidationError(
          "selected XTD package does not contain expected seasonal "
          "circulation");
    }
  }
  if (request.dry_run) {
    return {request.output,
            0,
            0,
            request.weather_provider,
            request.include_waves ? request.wave_provider : "none",
            current_source,
            std::nullopt,
            {},
            Json::Value(Json::objectValue),
            Json::Value(Json::objectValue)};
  }
  Workspace workspace(request.output, request.keep_intermediate);
  ResumeCache resume(request.output, progress);
  std::vector<std::pair<std::string, std::filesystem::path>> streams;
  std::optional<std::string> selected_cycle;
  Json::Value diagnostics(Json::objectValue);

  const auto has_stream = [&](const std::string& label) {
    return std::any_of(streams.begin(), streams.end(), [&](const auto& stream) {
      return stream.first == label;
    });
  };
  const bool coupled_gfs = request.weather_provider == "gfs" &&
                           request.include_waves &&
                           request.wave_provider == "gfs_wave";
  const bool cacheable_weather = request.weather_provider != "none" &&
                                 request.weather_provider != "existing-file" &&
                                 !coupled_gfs;
  const std::string weather_fingerprint = WeatherFingerprint(request);
  bool restored_weather = false;
  if (coupled_gfs) {
    if (const auto cached_weather = resume.Restore(
            "weather", weather_fingerprint, workspace.File("weather.grb"));
        cached_weather && cached_weather->selected_cycle) {
      const auto coupled_wave_fingerprint =
          WaveFingerprint(request, cached_weather->selected_cycle);
      if (const auto cached_waves = resume.Restore(
              "waves", coupled_wave_fingerprint, workspace.File("waves.grb"))) {
        streams.emplace_back("weather", cached_weather->path);
        streams.emplace_back("waves", cached_waves->path);
        selected_cycle = cached_weather->selected_cycle;
        restored_weather = true;
      }
    }
  } else if (cacheable_weather) {
    if (const auto cached = resume.Restore("weather", weather_fingerprint,
                                           workspace.File("weather.grb"))) {
      streams.emplace_back("weather", cached->path);
      selected_cycle = cached->selected_cycle;
      restored_weather = true;
    }
  }

  if (restored_weather) {
    // The validated checkpoint is already in the workspace and stream list.
  } else if (request.weather_provider == "existing-file") {
    if (!request.weather_file)
      throw ValidationError("existing weather provider requires weather-file");
    streams.emplace_back(
        "weather",
        CopyValidated(*request.weather_file, workspace.File("weather.grb")));
  } else if (request.weather_provider == "gfs") {
    const auto weather_path = workspace.File("weather.grb");
    if (request.include_waves && request.wave_provider == "gfs_wave") {
      GFSRequest probe{request.bbox,
                       weather_path,
                       request.hours,
                       request.step_hours,
                       request.cycle,
                       request.date,
                       true,
                       60.0,
                       8,
                       false,
                       request.weather_preset,
                       false};
      std::vector<std::string> errors;
      bool complete = false;
      for (const auto& candidate : GfsCycleCandidates(probe, now)) {
        try {
          GFSRequest atmosphere = probe;
          atmosphere.cycle = candidate.cycle;
          atmosphere.date = candidate.date;
          const auto weather = GenerateGfs(
              atmosphere,
              MakeRetryingHttpGet(http_get, "NOAA GFS weather", progress), now,
              progress);
          GFSRequest waves{request.bbox,
                           workspace.File("waves.grb"),
                           request.hours,
                           request.wave_step_hours,
                           candidate.cycle,
                           candidate.date,
                           true,
                           60.0,
                           1,
                           false,
                           "routing",
                           true};
          const auto wave = GenerateGfs(
              waves, MakeRetryingHttpGet(http_get, "NOAA GFS Wave", progress),
              now, progress);
          streams.emplace_back("weather", weather.output);
          streams.emplace_back("waves", wave.output);
          selected_cycle = candidate.CycleTime();
          complete = true;
          break;
        } catch (const ValidationError& error) {
          errors.push_back(candidate.CycleTime() + ": " + error.what());
          std::error_code ignored;
          std::filesystem::remove(weather_path, ignored);
          std::filesystem::remove(workspace.File("waves.grb"), ignored);
        }
      }
      if (!complete) {
        std::ostringstream message;
        message << "No common complete GFS atmosphere/wave cycle was "
                   "available. Tried: ";
        for (const auto& error : errors) message << error << "; ";
        throw ValidationError(message.str());
      }
    } else {
      GFSRequest weather{request.bbox,
                         weather_path,
                         request.hours,
                         request.step_hours,
                         request.cycle,
                         request.date,
                         true,
                         60.0,
                         8,
                         false,
                         request.weather_preset,
                         false};
      const auto result = GenerateGfs(
          weather, MakeRetryingHttpGet(http_get, "NOAA GFS weather", progress),
          now, progress);
      streams.emplace_back("weather", result.output);
      selected_cycle = result.cycle.CycleTime();
    }
  } else if (request.weather_provider == "ukmo_ukv") {
    UkvRequest ukv;
    ukv.bbox = request.bbox;
    ukv.output = workspace.File("weather.grb");
    ukv.hours = request.hours;
    ukv.step_hours = request.step_hours;
    ukv.cycle = request.cycle;
    ukv.date = request.date;
    ukv.overwrite = true;
    ukv.preset = request.weather_preset;
    ukv.grid_spacing_deg = request.weather_grid_spacing_deg;
    const auto weather = GenerateUkv(
        ukv, MakeRetryingHttpGet(http_get, "Met Office UKV weather", progress),
        now, progress);
    streams.emplace_back("weather", weather.output);
    selected_cycle = weather.cycle.CycleTime();
  } else if (request.weather_provider == "dwd_icon_eu" ||
             request.weather_provider == "noaa_hrrr") {
    GFSRequest icon{request.bbox,
                    workspace.File("weather.grb"),
                    request.hours,
                    request.step_hours,
                    request.cycle,
                    request.date,
                    true,
                    90.0,
                    8,
                    false,
                    request.weather_preset,
                    false};
    const std::string provider = request.weather_provider == "dwd_icon_eu"
                                     ? "DWD ICON-EU weather"
                                     : "NOAA HRRR weather";
    const auto weather =
        request.weather_provider == "dwd_icon_eu"
            ? GenerateDwdIconEu(
                  icon, MakeRetryingHttpGet(http_get, provider, progress), now,
                  progress)
            : GenerateHrrr(icon,
                           MakeRetryingHttpGet(http_get, provider, progress),
                           MakeRetryingHttpGetRange({}, provider, progress),
                           now, progress);
    streams.emplace_back("weather", weather.output);
    selected_cycle = weather.cycle.CycleTime();
  } else if (request.weather_provider == "ecmwf_ifs_open" ||
             request.weather_provider == "ecmwf_aifs_open") {
    GFSRequest ecmwf{request.bbox,
                     workspace.File("weather.grb"),
                     request.hours,
                     request.step_hours,
                     request.cycle,
                     request.date,
                     true,
                     180.0,
                     8,
                     false,
                     request.weather_preset,
                     false};
    const std::string provider = request.weather_provider == "ecmwf_aifs_open"
                                     ? "ECMWF AIFS weather"
                                     : "ECMWF IFS weather";
    const auto weather = GenerateEcmwfOpenData(
        ecmwf, request.weather_provider == "ecmwf_aifs_open",
        MakeRetryingHttpGet(http_get, provider, progress),
        MakeRetryingHttpGetRange({}, provider, progress), now, progress);
    streams.emplace_back("weather", weather.output);
    selected_cycle = weather.cycle.CycleTime();
  } else if (request.weather_provider != "none") {
    throw UnsupportedSourceError(
        "native weather provider is not yet available: " +
        request.weather_provider);
  }

  if (cacheable_weather && !restored_weather && has_stream("weather")) {
    resume.Save("weather", weather_fingerprint, workspace.File("weather.grb"),
                selected_cycle);
  }
  if (coupled_gfs && !restored_weather && has_stream("weather") &&
      has_stream("waves") && selected_cycle) {
    resume.Save("weather", weather_fingerprint, workspace.File("weather.grb"),
                selected_cycle);
    resume.Save("waves", WaveFingerprint(request, selected_cycle),
                workspace.File("waves.grb"), selected_cycle);
  }
  const bool cacheable_waves = request.include_waves && !coupled_gfs;
  const std::string wave_fingerprint = WaveFingerprint(request, std::nullopt);
  bool restored_waves = false;
  if (cacheable_waves && !has_stream("waves")) {
    if (const auto cached = resume.Restore("waves", wave_fingerprint,
                                           workspace.File("waves.grb"))) {
      streams.emplace_back("waves", cached->path);
      if (!selected_cycle) selected_cycle = cached->selected_cycle;
      restored_waves = true;
    }
  }

  std::optional<std::string> wave_cycle;
  if (request.include_waves && request.wave_provider == "gfs_wave" &&
      !has_stream("waves")) {
    GFSRequest waves{request.bbox,
                     workspace.File("waves.grb"),
                     request.hours,
                     request.wave_step_hours,
                     request.cycle,
                     request.date,
                     true,
                     60.0,
                     8,
                     false,
                     "routing",
                     true};
    if (request.weather_provider != "gfs") {
      waves.cycle = "auto";
      waves.date = std::nullopt;
    }
    const auto wave = GenerateGfs(
        waves, MakeRetryingHttpGet(http_get, "NOAA GFS Wave", progress), now,
        progress);
    streams.emplace_back("waves", wave.output);
    wave_cycle = wave.cycle.CycleTime();
    if (!selected_cycle) selected_cycle = wave.cycle.CycleTime();
  }

  if (request.include_waves &&
      request.wave_provider == "copernicus_global_waves" &&
      !has_stream("waves")) {
    Report(progress, "authenticating Copernicus Global Waves",
           "validating Copernicus Marine credentials");
    streams.emplace_back(
        "waves",
        GenerateCopernicusGlobalWaves(
            request.bbox, request.start, request.hours, request.wave_step_hours,
            request.copernicus_username, request.copernicus_password,
            workspace.File("waves.grb"), request.weather_grid_spacing_deg, true,
            BinaryDownload(MakeRetryingHttpGet(
                http_get, "Copernicus Global Waves", progress,
                {5, 1000, 8000})))
            .output);
  } else if (request.include_waves && request.wave_provider != "gfs_wave" &&
             request.wave_provider != "copernicus_global_waves") {
    throw UnsupportedSourceError("native wave provider is not available: " +
                                 request.wave_provider);
  }

  if (cacheable_waves && !restored_waves && has_stream("waves")) {
    resume.Save("waves", wave_fingerprint, workspace.File("waves.grb"),
                wave_cycle);
  }

  const bool cacheable_current = current_source == "marine_ie_irish_sea" ||
                                 current_source == "copernicus_nws" ||
                                 current_source == "copernicus_global" ||
                                 current_source == "copernicus_ibi" ||
                                 current_source ==
                                     "copernicus_mediterranean" ||
                                 current_source == "noaa_rtofs_global";
  const std::string current_fingerprint =
      CurrentFingerprint(request, current_source);
  bool restored_current = false;
  if (cacheable_current) {
    if (const auto cached = resume.Restore("current", current_fingerprint,
                                           workspace.File("current.grb"))) {
      streams.emplace_back("current", cached->path);
      restored_current = true;
    }
  }

  if (restored_current) {
    // The validated checkpoint is already in the workspace and stream list.
  } else if (current_source == "existing-file") {
    if (!request.current_file)
      throw ValidationError("existing current source requires current-file");
    streams.emplace_back(
        "current",
        CopyValidated(*request.current_file, workspace.File("current.grb")));
  } else if (current_source == "marine_ie_irish_sea") {
    Report(progress, "downloading current", current_source);
    streams.emplace_back(
        "current", DownloadMarineIe(
                       workspace.File("current.grb"), true,
                       BinaryDownload(MakeRetryingHttpGet(
                           http_get, "Marine.ie Irish Sea current", progress)))
                       .output);
  } else if (current_source == "copernicus_nws" ||
             current_source == "copernicus_global" ||
             current_source == "copernicus_ibi" ||
             current_source == "copernicus_mediterranean") {
    CopernicusRequest current;
    current.bbox = request.bbox;
    current.start = request.start;
    current.hours = request.hours;
    current.step_hours = request.step_hours;
    current.grid_spacing_deg = request.current_grid_spacing_deg;
    current.username = request.copernicus_username;
    current.password = request.copernicus_password;
    current.output = workspace.File("current.grb");
    current.overwrite = true;
    current.provider = current_source;
    const std::string provider =
        current_source == "copernicus_global"
            ? "Copernicus Global current"
            : current_source == "copernicus_ibi"
                  ? "Copernicus IBI current"
                  : current_source == "copernicus_mediterranean"
                        ? "Copernicus Mediterranean current"
                        : "Copernicus NWS current";
    Report(progress, "authenticating " + provider,
           "validating Copernicus Marine credentials");
    const auto download = BinaryDownload(MakeRetryingHttpGet(
        http_get, provider, progress, {5, 1000, 8000}));
    CopernicusResult generated;
    if (current_source == "copernicus_global")
      generated = GenerateCopernicusGlobal(current, download);
    else if (current_source == "copernicus_ibi")
      generated = GenerateCopernicusIbi(current, download);
    else if (current_source == "copernicus_mediterranean")
      generated = GenerateCopernicusMediterranean(current, download);
    else
      generated = GenerateCopernicusNws(current, download);
    streams.emplace_back("current", generated.output);
  } else if (current_source == "noaa_rtofs_global") {
    RtofsRequest current;
    current.bbox = request.bbox;
    current.output = workspace.File("current.grb");
    current.hours = request.hours;
    current.step_hours = request.step_hours;
    current.cycle = request.cycle;
    current.date = request.date;
    current.download_directory = request.download_directory.empty()
                                     ? workspace.File("rtofs")
                                     : request.download_directory;
    current.grid_spacing_deg = request.current_grid_spacing_deg;
    current.overwrite = true;
    streams.emplace_back(
        "current", GenerateRtofs(current, {},
                                 BinaryDownload(MakeRetryingHttpGet(
                                     http_get, "NOAA RTOFS current", progress)))
                       .output);
  } else if (current_source == "netcdf") {
    if (!request.input_netcdf)
      throw ValidationError("netcdf current source requires input-netcdf");
    const auto grid =
        BuildRegularGrid(request.bbox, request.current_grid_spacing_deg);
    const auto times =
        BuildTimeSequence(request.start, request.hours, request.step_hours);
    NetCDFCurrentSource source(*request.input_netcdf);
    std::vector<CurrentGrid> fields;
    for (const auto time : times)
      fields.push_back(source.GetCurrentGrid(request.bbox, time, grid));
    const auto path = workspace.File("current.grb");
    WriteGrib1Currents(fields, path);
    streams.emplace_back("current", path);
  } else if (current_source == "tpxo-cache") {
    if (!request.input_cache)
      throw ValidationError("tpxo-cache current source requires input-cache");
    bool cache_needs_preparation =
        !std::filesystem::is_regular_file(*request.input_cache);
    if (!cache_needs_preparation) {
      try {
        LoadTpxoCache(*request.input_cache);
      } catch (const ValidationError&) {
        cache_needs_preparation = true;
      }
    }
    if (cache_needs_preparation && request.auto_prepare_tpxo_cache) {
      if (!request.tpxo_model_directory) {
        throw ValidationError(
            "automatic TPXO cache preparation requires tpxoModelDirectory");
      }
      Report(progress, "preparing TPXO cache", request.input_cache->string());
      PrepareTpxo10Cache(*request.tpxo_model_directory, request.bbox,
                         request.current_grid_spacing_deg, *request.input_cache,
                         true);
    }
    streams.emplace_back(
        "current", GenerateFromTpxoCache(*request.input_cache, request.start,
                                         request.hours, request.step_hours,
                                         workspace.File("current.grb"),
                                         request.infer_minor_tides, true)
                       .output);
  } else if (current_source == "tpxo") {
    if (!request.tpxo_model_directory)
      throw ValidationError("tpxo current source requires model-dir");
    TpxoModelRequest current{request.bbox,
                             request.start,
                             request.hours,
                             request.step_hours,
                             request.current_grid_spacing_deg,
                             *request.tpxo_model_directory,
                             workspace.File("current.grb"),
                             request.infer_minor_tides,
                             true};
    streams.emplace_back("current",
                         GenerateFromTpxo10AtlasModel(current).output);
  } else if (current_source == "offline-tidal") {
    if (!request.offline_tidal_file) {
      throw ValidationError(
          "offline-tidal current source requires offlineTidalFile");
    }
    Report(progress, "loading Offline current package",
           request.offline_tidal_file->string());
    const auto grid =
        BuildRegularGrid(request.bbox, request.current_grid_spacing_deg);
    const auto times =
        BuildTimeSequence(request.start, request.hours, request.step_hours);
    XtdPackageReader reader(*request.offline_tidal_file);
    const auto offline_mode =
        ParseOfflineCurrentMode(request.offline_current_mode);
    Report(progress, "calculating offline currents",
           std::to_string(times.size()) + " timestamps");
    const auto prediction_started = std::chrono::steady_clock::now();
    auto fields =
        reader.Predict(grid, times, offline_mode, request.infer_minor_tides);
    const double prediction_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - prediction_started)
            .count();
    const auto path = workspace.File("current.grb");
    WriteGrib1Currents(fields, path);
    streams.emplace_back("current", path);

    const auto stats = reader.statistics();
    diagnostics["offline_tidal"]["package"] =
        request.offline_tidal_file->string();
    diagnostics["offline_tidal"]["tiles_loaded"] =
        Json::UInt64(stats.tide.tiles_loaded + stats.residual.tiles_loaded);
    diagnostics["offline_tidal"]["tide_tiles_loaded"] =
        Json::UInt64(stats.tide.tiles_loaded);
    diagnostics["offline_tidal"]["residual_tiles_loaded"] =
        Json::UInt64(stats.residual.tiles_loaded);
    diagnostics["offline_tidal"]["uncertainty_tiles_loaded"] =
        Json::UInt64(stats.uncertainty.tiles_loaded);
    diagnostics["offline_tidal"]["cache_hits"] =
        Json::UInt64(stats.tide.cache_hits + stats.residual.cache_hits);
    diagnostics["offline_tidal"]["encrypted_bytes"] = Json::UInt64(
        stats.tide.encrypted_bytes + stats.residual.encrypted_bytes);
    diagnostics["offline_tidal"]["compressed_bytes"] = Json::UInt64(
        stats.tide.compressed_bytes + stats.residual.compressed_bytes);
    diagnostics["offline_tidal"]["decompressed_bytes"] = Json::UInt64(
        stats.tide.decompressed_bytes + stats.residual.decompressed_bytes);
    diagnostics["offline_tidal"]["peak_cache_bytes"] = Json::UInt64(
        stats.tide.peak_cache_bytes + stats.residual.peak_cache_bytes);
    diagnostics["offline_tidal"]["tile_load_ms"] =
        stats.tide.load_ms + stats.residual.load_ms;
    diagnostics["offline_tidal"]["interpolation_ms"] =
        stats.tide.interpolation_ms + stats.residual.interpolation_ms;
    diagnostics["offline_tidal"]["package_validation_ms"] =
        reader.status().validation_ms;
    diagnostics["offline_tidal"]["package_bytes_read"] =
        Json::UInt64(stats.outer_bytes_read);
    diagnostics["offline_tidal"]["harmonic_evaluation_ms"] = prediction_ms;
    diagnostics["offline_tidal"]["format_version"] =
        reader.status().format_version;
    diagnostics["offline_tidal"]["mode"] = OfflineCurrentModeId(offline_mode);
    diagnostics["offline_tidal"]["source_description"] =
        offline_mode == OfflineCurrentMode::kAstronomicalTideOnly
            ? "deterministic astronomical tide"
            : "deterministic tide plus expected seasonal circulation; not a "
              "forecast";
    diagnostics["offline_tidal"]["parent_package_hash"] =
        reader.status().parent_package_hash;
    diagnostics["offline_tidal"]["package_id"] = reader.status().package_id;
    diagnostics["offline_tidal"]["package_hash"] = reader.status().package_hash;
    diagnostics["offline_tidal"]["residual_representation"] =
        reader.status().residual_representation;
    if (reader.status().metadata.isMember("uncertainty_summary")) {
      diagnostics["offline_tidal"]["uncertainty_summary"] =
          reader.status().metadata["uncertainty_summary"];
    }
  } else if (current_source == "synthetic") {
    const auto grid =
        BuildRegularGrid(request.bbox, request.current_grid_spacing_deg);
    std::vector<CurrentGrid> fields;
    for (const auto time :
         BuildTimeSequence(request.start, request.hours, request.step_hours))
      fields.push_back(MakeSyntheticRotaryCurrent(request.bbox, time, grid));
    const auto path = workspace.File("current.grb");
    WriteGrib1Currents(fields, path);
    streams.emplace_back("current", path);
  } else if (current_source != "none") {
    throw UnsupportedSourceError(
        "native current provider is not yet available: " + current_source);
  }

  if (cacheable_current && !restored_current && has_stream("current")) {
    resume.Save("current", current_fingerprint, workspace.File("current.grb"));
  }

  if (streams.empty())
    throw ValidationError("generation produced no environmental streams");
  Report(progress, "merging environmental GRIB",
         std::to_string(streams.size()) + " streams");
  const auto merged =
      MergeGribStreams(streams, request.output, request.overwrite);
  std::vector<std::filesystem::path> input_paths;
  for (const auto& [label, path] : streams) {
    (void)label;
    input_paths.push_back(path);
  }
  resume.Complete();
  return {request.output,
          merged.output_message_count,
          merged.byte_count,
          request.weather_provider,
          request.include_waves ? request.wave_provider : "none",
          current_source,
          selected_cycle,
          input_paths,
          merged.inspection,
          diagnostics};
}

Json::Value EnvironmentResultJson(const EnvironmentResult& result) {
  Json::Value value(Json::objectValue);
  value["output"] = result.output.string();
  value["message_count"] = Json::UInt64(result.message_count);
  value["byte_count"] = Json::UInt64(result.byte_count);
  value["weather_provider"] = result.weather_provider;
  value["wave_provider"] = result.wave_provider;
  value["current_source"] = result.current_source;
  if (result.selected_cycle) value["selected_cycle"] = *result.selected_cycle;
  for (const auto& path : result.inputs) value["inputs"].append(path.string());
  value["inspection"] = result.inspection;
  value["diagnostics"] = result.diagnostics;
  return value;
}

}  // namespace environmental_grib
