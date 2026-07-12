#include "environmental_grib/environment.h"

#include <fstream>
#include <optional>
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

namespace environmental_grib {
namespace {

class Workspace {
 public:
  explicit Workspace(const std::filesystem::path& output, bool keep)
      : keep_(keep) {
    path_ = output.parent_path().empty() ? std::filesystem::current_path() : output.parent_path();
    path_ /= ".environmental-grib-" + std::to_string(::getpid());
    std::filesystem::create_directories(path_);
  }
  ~Workspace() {
    if (!keep_) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }
  std::filesystem::path File(const std::string& name) const { return path_ / name; }
 private:
  std::filesystem::path path_;
  bool keep_{};
};

std::filesystem::path CopyValidated(const std::filesystem::path& source,
                                    const std::filesystem::path& target) {
  if (!std::filesystem::is_regular_file(source)) throw ValidationError("input GRIB not found: " + source.string());
  ScanGribMessages(source);
  std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);
  return target;
}

void Report(const ProgressCallback& callback, const std::string& stage,
            const std::string& detail) {
  if (!callback) return;
  Json::Value value; value["detail"] = detail; callback(stage, value);
}

std::string ResolveCurrentSource(const EnvironmentRequest& request) {
  if (request.current_source != "auto") return request.current_source;
  ProviderRegistry registry;
  const auto* selected = SelectBestProviderForBbox(request.bbox, request.hours, registry);
  if (!selected) throw ValidationError("no current provider supports the requested bbox/duration");
  return selected->id;
}

}  // namespace

EnvironmentResult GenerateEnvironment(const EnvironmentRequest& request,
                                      HttpGet http_get,
                                      std::optional<TimePoint> now,
                                      ProgressCallback progress) {
  request.bbox.Validate();
  BuildTimeSequence(request.start, request.hours, request.step_hours);
  if (request.output.empty()) throw ValidationError("environment output path is required");
  if (std::filesystem::exists(request.output) && !request.overwrite) throw ValidationError("output already exists; enable overwrite to replace it");
  if (request.weather_provider == "none" && request.current_source == "none") throw ValidationError("at least one weather or current source must be enabled");
  if (request.dry_run) {
    return {request.output, 0, 0, request.weather_provider,
            request.include_waves ? request.wave_provider : "none",
            request.current_source, std::nullopt, {}, Json::Value(Json::objectValue)};
  }
  Workspace workspace(request.output, request.keep_intermediate);
  std::vector<std::pair<std::string, std::filesystem::path>> streams;
  std::optional<std::string> selected_cycle;

  if (request.weather_provider == "existing-file") {
    if (!request.weather_file) throw ValidationError("existing weather provider requires weather-file");
    streams.emplace_back("weather", CopyValidated(*request.weather_file, workspace.File("weather.grb")));
  } else if (request.weather_provider == "gfs") {
    const auto weather_path = workspace.File("weather.grb");
    if (request.include_waves && request.wave_provider == "gfs_wave") {
      GFSRequest probe{request.bbox, weather_path, request.hours, request.step_hours,
                       request.cycle, request.date, true, 60.0, 8, false,
                       request.weather_preset, false};
      std::vector<std::string> errors;
      bool complete = false;
      for (const auto& candidate : GfsCycleCandidates(probe, now)) {
        try {
          GFSRequest atmosphere = probe;
          atmosphere.cycle = candidate.cycle; atmosphere.date = candidate.date;
          const auto weather = GenerateGfs(atmosphere, http_get, now, progress);
          GFSRequest waves{request.bbox, workspace.File("waves.grb"), request.hours,
                           request.wave_step_hours, candidate.cycle, candidate.date,
                           true, 60.0, 1, false, "routing", true};
          const auto wave = GenerateGfs(waves, http_get, now, progress);
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
      if (!complete) throw ValidationError("No common complete GFS atmosphere/wave cycle was available");
    } else {
      GFSRequest weather{request.bbox, weather_path, request.hours, request.step_hours,
                         request.cycle, request.date, true, 60.0, 8, false,
                         request.weather_preset, false};
      const auto result = GenerateGfs(weather, http_get, now, progress);
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
    const auto weather = GenerateUkv(ukv, http_get, now, progress);
    streams.emplace_back("weather", weather.output);
    selected_cycle = weather.cycle.CycleTime();
    if (request.include_waves) {
      if (request.wave_provider == "gfs_wave") {
        GFSRequest waves{request.bbox, workspace.File("waves.grb"), request.hours,
                         request.wave_step_hours, "auto", std::nullopt, true,
                         60.0, 8, false, "routing", true};
        streams.emplace_back("waves", GenerateGfs(waves, http_get, now, progress).output);
      }
    }
  } else if (request.weather_provider == "dwd_icon_eu" ||
             request.weather_provider == "noaa_hrrr") {
    GFSRequest icon{request.bbox, workspace.File("weather.grb"), request.hours,
                    request.step_hours, request.cycle, request.date, true, 90.0,
                    8, false, request.weather_preset, false};
    const auto weather = request.weather_provider == "dwd_icon_eu"
                             ? GenerateDwdIconEu(icon, http_get, now, progress)
                             : GenerateHrrr(icon, http_get, {}, now, progress);
    streams.emplace_back("weather", weather.output);
    selected_cycle = weather.cycle.CycleTime();
    if (request.include_waves && request.wave_provider == "gfs_wave") {
      GFSRequest waves{request.bbox, workspace.File("waves.grb"), request.hours,
                       request.wave_step_hours, "auto", std::nullopt, true,
                       60.0, 8, false, "routing", true};
      const auto wave = GenerateGfs(waves, http_get, now, progress);
      streams.emplace_back("waves", wave.output);
    }
  } else if (request.weather_provider == "ecmwf_ifs_open" ||
             request.weather_provider == "ecmwf_aifs_open") {
    GFSRequest ecmwf{request.bbox, workspace.File("weather.grb"), request.hours,
                     request.step_hours, request.cycle, request.date, true,
                     180.0, 8, false, request.weather_preset, false};
    const auto weather = GenerateEcmwfOpenData(
        ecmwf, request.weather_provider == "ecmwf_aifs_open", http_get, {}, now,
        progress);
    streams.emplace_back("weather", weather.output);
    selected_cycle = weather.cycle.CycleTime();
    if (request.include_waves && request.wave_provider == "gfs_wave") {
      GFSRequest waves{request.bbox, workspace.File("waves.grb"), request.hours,
                       request.wave_step_hours, "auto", std::nullopt, true,
                       60.0, 8, false, "routing", true};
      const auto wave = GenerateGfs(waves, http_get, now, progress);
      streams.emplace_back("waves", wave.output);
    }
  } else if (request.weather_provider != "none") {
    throw UnsupportedSourceError("native weather provider is not yet available: " + request.weather_provider);
  }

  if (request.include_waves &&
      request.wave_provider == "copernicus_global_waves") {
    streams.emplace_back(
        "waves",
        GenerateCopernicusGlobalWaves(
            request.bbox, request.start, request.hours, request.wave_step_hours,
            request.copernicus_username, request.copernicus_password,
            workspace.File("waves.grb"), request.weather_grid_spacing_deg, true,
            http_get ? BinaryDownload(http_get) : BinaryDownload{})
            .output);
  } else if (request.include_waves && request.wave_provider != "gfs_wave") {
    throw UnsupportedSourceError("native wave provider is not available: " +
                                 request.wave_provider);
  }

  const std::string current_source = ResolveCurrentSource(request);
  if (current_source == "existing-file") {
    if (!request.current_file) throw ValidationError("existing current source requires current-file");
    streams.emplace_back("current", CopyValidated(*request.current_file, workspace.File("current.grb")));
  } else if (current_source == "marine_ie_irish_sea") {
    Report(progress, "downloading current", current_source);
    streams.emplace_back("current", DownloadMarineIe(workspace.File("current.grb"), true,
                                                      http_get ? BinaryDownload(http_get) : BinaryDownload{}).output);
  } else if (current_source == "copernicus_nws" ||
             current_source == "copernicus_global") {
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
    const auto generated = current_source == "copernicus_global"
                               ? GenerateCopernicusGlobal(
                                     current, http_get ? BinaryDownload(http_get) : BinaryDownload{})
                               : GenerateCopernicusNws(
                                     current, http_get ? BinaryDownload(http_get) : BinaryDownload{});
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
    streams.emplace_back("current", GenerateRtofs(
        current, {},
        http_get ? BinaryDownload(http_get) : BinaryDownload{}).output);
  } else if (current_source == "netcdf") {
    if (!request.input_netcdf) throw ValidationError("netcdf current source requires input-netcdf");
    const auto grid = BuildRegularGrid(request.bbox, request.current_grid_spacing_deg);
    const auto times = BuildTimeSequence(request.start, request.hours, request.step_hours);
    NetCDFCurrentSource source(*request.input_netcdf);
    std::vector<CurrentGrid> fields;
    for (const auto time : times) fields.push_back(source.GetCurrentGrid(request.bbox, time, grid));
    const auto path = workspace.File("current.grb");
    WriteGrib1Currents(fields, path);
    streams.emplace_back("current", path);
  } else if (current_source == "tpxo-cache") {
    if (!request.input_cache)
      throw ValidationError("tpxo-cache current source requires input-cache");
    if (!std::filesystem::is_regular_file(*request.input_cache) &&
        request.auto_prepare_tpxo_cache) {
      if (!request.tpxo_model_directory) {
        throw ValidationError(
            "automatic TPXO cache preparation requires tpxoModelDirectory");
      }
      Report(progress, "preparing TPXO cache", request.input_cache->string());
      PrepareTpxo10Cache(*request.tpxo_model_directory, request.bbox,
                         request.current_grid_spacing_deg,
                         *request.input_cache, true);
    }
    streams.emplace_back(
        "current",
        GenerateFromTpxoCache(*request.input_cache, request.start,
                              request.hours, request.step_hours,
                              workspace.File("current.grb"),
                              request.infer_minor_tides, true)
            .output);
  } else if (current_source == "tpxo") {
    if (!request.tpxo_model_directory)
      throw ValidationError("tpxo current source requires model-dir");
    TpxoModelRequest current{request.bbox, request.start, request.hours,
                             request.step_hours,
                             request.current_grid_spacing_deg,
                             *request.tpxo_model_directory,
                             workspace.File("current.grb"),
                             request.infer_minor_tides, true};
    streams.emplace_back("current",GenerateFromTpxo10AtlasModel(current).output);
  } else if (current_source == "synthetic") {
    const auto grid = BuildRegularGrid(request.bbox, request.current_grid_spacing_deg);
    std::vector<CurrentGrid> fields;
    for (const auto time : BuildTimeSequence(request.start, request.hours, request.step_hours)) fields.push_back(MakeSyntheticRotaryCurrent(request.bbox, time, grid));
    const auto path = workspace.File("current.grb");
    WriteGrib1Currents(fields, path);
    streams.emplace_back("current", path);
  } else if (current_source != "none") {
    throw UnsupportedSourceError("native current provider is not yet available: " + current_source);
  }

  if (streams.empty()) throw ValidationError("generation produced no environmental streams");
  Report(progress, "merging environmental GRIB", std::to_string(streams.size()) + " streams");
  const auto merged = MergeGribStreams(streams, request.output, request.overwrite);
  std::vector<std::filesystem::path> input_paths;
  for (const auto& [label, path] : streams) { (void)label; input_paths.push_back(path); }
  return {request.output, merged.output_message_count, merged.byte_count,
          request.weather_provider, request.include_waves ? request.wave_provider : "none",
          current_source, selected_cycle, input_paths, merged.inspection};
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
  return value;
}

}  // namespace environmental_grib
