#include "environmental_grib/job.h"

#include <fstream>

#include "environmental_grib/error.h"

namespace environmental_grib {
namespace {

const Json::Value& Required(const Json::Value& object, const char* name) {
  if (!object.isObject() || !object.isMember(name)) {
    throw ValidationError(std::string("job is missing required field: ") + name);
  }
  return object[name];
}

std::string String(const Json::Value& object, const char* name,
                   const std::string& fallback = {}) {
  if (!object.isMember(name)) return fallback;
  if (!object[name].isString()) {
    throw ValidationError(std::string("job field must be a string: ") + name);
  }
  return object[name].asString();
}

int Integer(const Json::Value& object, const char* name, int fallback) {
  if (!object.isMember(name)) return fallback;
  if (!object[name].isInt()) {
    throw ValidationError(std::string("job field must be an integer: ") + name);
  }
  return object[name].asInt();
}

double Number(const Json::Value& object, const char* name, double fallback) {
  if (!object.isMember(name)) return fallback;
  if (!object[name].isNumeric()) {
    throw ValidationError(std::string("job field must be numeric: ") + name);
  }
  return object[name].asDouble();
}

bool Boolean(const Json::Value& object, const char* name, bool fallback) {
  if (!object.isMember(name)) return fallback;
  if (!object[name].isBool()) {
    throw ValidationError(std::string("job field must be boolean: ") + name);
  }
  return object[name].asBool();
}

void OptionalPath(const Json::Value& object, const char* name,
                  std::optional<std::filesystem::path>* target) {
  const auto value = String(object, name);
  if (!value.empty()) *target = value;
}

Json::Value StringArray(std::initializer_list<const char*> values) {
  Json::Value result(Json::arrayValue);
  for (const auto* value : values) result.append(value);
  return result;
}

}  // namespace

GeneratorJob ParseGeneratorJob(const Json::Value& value) {
  if (!value.isObject()) throw ValidationError("job root must be an object");
  GeneratorJob job;
  if (!value.isMember("schemaVersion") || !value["schemaVersion"].isInt()) {
    throw ValidationError("job schemaVersion must be an integer");
  }
  job.schema_version = value["schemaVersion"].asInt();
  if (job.schema_version != kJobSchemaVersion) {
    throw ValidationError("unsupported job schemaVersion: " +
                          std::to_string(job.schema_version));
  }
  job.operation = String(value, "operation");
  if (job.operation != "generateEnvironment") {
    throw ValidationError("unsupported job operation: " + job.operation);
  }

  const auto& request = Required(value, "request");
  const auto& bbox = Required(request, "bbox");
  job.request.bbox = {
      Number(bbox, "west", 999.0), Number(bbox, "south", 999.0),
      Number(bbox, "east", 999.0), Number(bbox, "north", 999.0)};
  job.request.start = ParseUtcDateTime(String(request, "start"));
  job.request.hours = Integer(request, "hours", 0);
  job.request.step_hours = Integer(request, "stepHours", 3);
  job.request.cycle = String(request, "cycle", "auto");
  const auto date = String(request, "date");
  if (!date.empty()) job.request.date = date;
  job.request.weather_provider =
      String(request, "weatherProvider", "gfs");
  job.request.extend_forecast =
      Boolean(request, "extendForecast", false);
  job.request.fallback_weather_provider =
      String(request, "fallbackWeatherProvider", "none");
  job.request.fallback_wave_provider =
      String(request, "fallbackWaveProvider", "none");
  job.request.fallback_current_source =
      String(request, "fallbackCurrentSource", "none");
  job.request.weather_preset =
      String(request, "weatherPreset", "routing");
  job.request.weather_grid_spacing_deg =
      Number(request, "weatherGridSpacingDeg", 0.025);
  OptionalPath(request, "weatherFile", &job.request.weather_file);
  job.request.include_waves = Boolean(request, "includeWaves", false);
  job.request.wave_provider = String(request, "waveProvider", "gfs_wave");
  job.request.wave_step_hours = Integer(request, "waveStepHours", 3);
  job.request.current_source = String(request, "currentSource", "none");
  OptionalPath(request, "currentFile", &job.request.current_file);
  OptionalPath(request, "inputNetcdf", &job.request.input_netcdf);
  OptionalPath(request, "inputCache", &job.request.input_cache);
  OptionalPath(request, "offlineTidalFile", &job.request.offline_tidal_file);
  job.request.offline_current_mode =
      String(request, "offlineCurrentMode", "tide-only");
  OptionalPath(request, "tpxoModelDirectory",
               &job.request.tpxo_model_directory);
  job.request.auto_prepare_tpxo_cache =
      Boolean(request, "autoPrepareTpxoCache", false);
  job.request.download_directory = String(request, "downloadDirectory");
  job.request.copernicus_username = String(request, "copernicusUsername");
  job.request.current_grid_spacing_deg =
      Number(request, "currentGridSpacingDeg", 0.05);
  job.request.infer_minor_tides =
      Boolean(request, "inferMinorTides", true);
  job.request.output = String(request, "output");
  job.request.overwrite = Boolean(request, "overwrite", false);
  job.request.keep_intermediate =
      Boolean(request, "keepIntermediate", false);
  job.request.dry_run = Boolean(request, "dryRun", false);

  if (value.isMember("credentials")) {
    const auto& credentials = value["credentials"];
    job.copernicus_password_environment = String(
        credentials, "copernicusPasswordEnvironment",
        job.copernicus_password_environment);
  }
  job.request.bbox.Validate();
  if (job.request.output.empty()) {
    throw ValidationError("job request output is required");
  }
  if (job.request.hours < 0 || job.request.step_hours <= 0) {
    throw ValidationError("job duration and step must be positive");
  }
  return job;
}

Json::Value GeneratorCapabilitiesJson() {
  Json::Value value(Json::objectValue);
  value["schemaVersion"] = kJobSchemaVersion;
  value["generatorVersion"] = kGeneratorVersion;
  value["operations"] = StringArray({"generateEnvironment"});
  value["weatherProviders"] = StringArray(
      {"none", "existing-file", "gfs", "noaa_hrrr", "ukmo_ukv",
       "dwd_icon_eu", "ecmwf_ifs_open", "ecmwf_aifs_open"});
  value["waveProviders"] =
      StringArray({"gfs_wave", "copernicus_global_waves"});
  value["forecastExtension"] = true;
  value["fallbackWeatherProviders"] = StringArray({"none", "gfs"});
  value["fallbackWaveProviders"] = StringArray({"none", "gfs_wave"});
  value["fallbackCurrentSources"] =
      StringArray({"none", "offline-tidal"});
  value["currentSources"] = StringArray(
      {"none", "auto", "existing-file", "offline-tidal", "tpxo",
       "tpxo-cache", "netcdf", "synthetic", "marine_ie_irish_sea",
       "noaa_rtofs_global", "copernicus_nws", "copernicus_global",
       "copernicus_ibi", "copernicus_mediterranean"});
  value["progressProtocol"] = "json-lines-v1";
  return value;
}

Json::Value JobStatusJson(const std::string& status) {
  Json::Value value(Json::objectValue);
  value["schemaVersion"] = kJobSchemaVersion;
  value["generatorVersion"] = kGeneratorVersion;
  value["operation"] = "generateEnvironment";
  value["status"] = status;
  return value;
}

void WriteJsonFileAtomic(const std::filesystem::path& path,
                         const Json::Value& value) {
  if (path.empty()) throw ValidationError("result file path is required");
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  const auto temporary = path.string() + ".tmp";
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "  ";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw ValidationError("cannot open result file: " + temporary);
    output << Json::writeString(builder, value) << '\n';
    if (!output) throw ValidationError("cannot write result file: " + temporary);
  }
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::rename(temporary, path);
}

}  // namespace environmental_grib
