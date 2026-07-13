#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include <json/json.h>

#include "environmental_grib/error.h"
#include "environmental_grib/copernicus.h"
#include "environmental_grib/environment.h"
#include "environmental_grib/geo.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/job.h"
#include "environmental_grib/netcdf.h"
#include "environmental_grib/providers.h"
#include "environmental_grib/remote_currents.h"
#include "environmental_grib/rtofs.h"
#include "environmental_grib/sources.h"
#include "environmental_grib/tpxo.h"
#include "environmental_grib/xtd.h"
#include "environmental_grib/xtd_package.h"
#include "environmental_grib/weather.h"
#include "environmental_grib/ukv.h"
#include "environmental_grib/waves.h"

namespace eg = environmental_grib;

namespace {

std::string RequireValue(const std::vector<std::string>& args,
                         std::size_t& index, const std::string& option) {
  if (++index >= args.size())
    throw eg::ValidationError(option + " requires a value");
  return args[index];
}

double ParseDouble(const std::string& value, const std::string& option) {
  std::size_t consumed = 0;
  try {
    const double result = std::stod(value, &consumed);
    if (consumed != value.size()) throw std::invalid_argument("trailing");
    return result;
  } catch (...) {
    throw eg::ValidationError(option + " requires a number");
  }
}

int ParseInt(const std::string& value, const std::string& option) {
  std::size_t consumed = 0;
  try {
    const int result = std::stoi(value, &consumed);
    if (consumed != value.size()) throw std::invalid_argument("trailing");
    return result;
  } catch (...) {
    throw eg::ValidationError(option + " requires an integer");
  }
}

void PrintJson(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "  ";
  std::cout << Json::writeString(builder, value) << '\n';
}

Json::Value ReadJsonFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw eg::ValidationError("cannot open job file: " + path.string());
  Json::CharReaderBuilder builder;
  Json::Value value;
  std::string errors;
  if (!Json::parseFromStream(builder, input, &value, &errors)) {
    throw eg::ValidationError("invalid job JSON: " + errors);
  }
  return value;
}

void PrintEvent(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  std::cout << Json::writeString(builder, value) << '\n' << std::flush;
}

Json::Value ProviderJson(const eg::Provider& provider) {
  Json::Value value(Json::objectValue);
  value["id"] = provider.id;
  value["label"] = provider.label;
  value["implemented"] = provider.implemented;
  value["resolution"] = provider.resolution;
  value["description"] = provider.description;
  value["provider_type"] = provider.provider_type;
  value["default_step_hours"] = provider.default_step_hours;
  if (provider.coverage) {
    value["coverage"]["west"] = provider.coverage->west;
    value["coverage"]["south"] = provider.coverage->south;
    value["coverage"]["east"] = provider.coverage->east;
    value["coverage"]["north"] = provider.coverage->north;
  } else {
    value["coverage"] = Json::nullValue;
  }
  if (provider.dataset_id) value["dataset_id"] = *provider.dataset_id;
  if (provider.product_id) value["product_id"] = *provider.product_id;
  if (provider.source_url) value["source_url"] = *provider.source_url;
  for (const auto& variable : provider.variables)
    value["variables"].append(variable);
  return value;
}

int Providers() {
  eg::ProviderRegistry registry;
  Json::Value result(Json::arrayValue);
  for (const auto& provider : registry.List())
    result.append(ProviderJson(provider));
  PrintJson(result);
  return 0;
}

int WeatherProviders() {
  Json::Value result(Json::arrayValue);
  for (const auto& provider : eg::ListWeatherProviders()) {
    Json::Value value(Json::objectValue);
    value["id"] = provider.id;
    value["label"] = provider.label;
    value["source"] = provider.source;
    value["format"] = provider.format;
    value["account"] = provider.account;
    value["description"] = provider.description;
    value["implemented"] = provider.implemented;
    result.append(value);
  }
  PrintJson(result);
  return 0;
}

int GenerateWeather(const std::vector<std::string>& args) {
  eg::GFSRequest request;
  bool have_bbox = false;
  std::string provider = "gfs";
  std::optional<eg::TimePoint> start;
  std::string username,
      password_environment = "CURRENTGRIB_COPERNICUS_PASSWORD";
  std::optional<double> grid_spacing;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const auto& arg = args[i];
    if (arg == "--provider")
      provider = RequireValue(args, i, arg);
    else if (arg == "--bbox") {
      if (i + 4 >= args.size())
        throw eg::ValidationError("--bbox requires W S E N");
      request.bbox = {ParseDouble(args[++i], arg), ParseDouble(args[++i], arg),
                      ParseDouble(args[++i], arg), ParseDouble(args[++i], arg)};
      have_bbox = true;
    } else if (arg == "--hours")
      request.hours = ParseInt(RequireValue(args, i, arg), arg);
    else if (arg == "--start")
      start = eg::ParseUtcDateTime(RequireValue(args, i, arg));
    else if (arg == "--step-hours")
      request.step_hours = ParseInt(RequireValue(args, i, arg), arg);
    else if (arg == "--cycle")
      request.cycle = RequireValue(args, i, arg);
    else if (arg == "--date")
      request.date = RequireValue(args, i, arg);
    else if (arg == "--weather-preset")
      request.preset = RequireValue(args, i, arg);
    else if (arg == "--username")
      username = RequireValue(args, i, arg);
    else if (arg == "--password-env")
      password_environment = RequireValue(args, i, arg);
    else if (arg == "--weather-grid-spacing-deg" || arg == "--grid-spacing-deg")
      grid_spacing = ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--output")
      request.output = RequireValue(args, i, arg);
    else if (arg == "--overwrite")
      request.overwrite = true;
    else if (arg == "--dry-run")
      request.dry_run = true;
    else if (arg == "--timeout-seconds")
      request.timeout_seconds = ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--json" || arg == "--metadata-summary") {
    } else
      throw eg::ValidationError("unknown generate-weather option: " + arg);
  }
  if (!have_bbox || request.output.empty())
    throw eg::ValidationError("generate-weather requires --bbox and --output");
  if (provider == "gfs") {
    request.waves = false;
    PrintJson(eg::WeatherResultJson(eg::GenerateGfs(request)));
  } else if (provider == "gfs_wave") {
    request.waves = true;
    PrintJson(eg::WeatherResultJson(eg::GenerateGfs(request)));
  } else if (provider == "dwd_icon_eu") {
    PrintJson(eg::WeatherResultJson(eg::GenerateDwdIconEu(request)));
  } else if (provider == "noaa_hrrr") {
    PrintJson(eg::WeatherResultJson(eg::GenerateHrrr(request)));
  } else if (provider == "ecmwf_ifs_open" || provider == "ecmwf_aifs_open") {
    PrintJson(eg::WeatherResultJson(
        eg::GenerateEcmwfOpenData(request, provider == "ecmwf_aifs_open")));
  } else if (provider == "ukmo_ukv") {
    eg::UkvRequest ukv;
    ukv.bbox = request.bbox;
    ukv.output = request.output;
    ukv.hours = request.hours;
    ukv.step_hours = request.step_hours;
    ukv.cycle = request.cycle;
    ukv.date = request.date;
    ukv.overwrite = request.overwrite;
    ukv.dry_run = request.dry_run;
    ukv.preset = request.preset;
    ukv.timeout_seconds = request.timeout_seconds;
    PrintJson(eg::WeatherResultJson(eg::GenerateUkv(ukv)));
  } else if (provider == "copernicus_global_waves") {
    if (!start)
      throw eg::ValidationError("Copernicus Global Waves requires --start");
    std::string password;
    if (const char* value = std::getenv(password_environment.c_str()))
      password = value;
    const auto result = eg::GenerateCopernicusGlobalWaves(
        request.bbox, *start, request.hours, request.step_hours, username,
        password, request.output, grid_spacing, request.overwrite);
    Json::Value json(Json::objectValue);
    json["provider"] = provider;
    json["output"] = result.output.string();
    json["message_count"] = Json::UInt64(result.message_count);
    json["byte_count"] = Json::UInt64(result.byte_count);
    json["inspection"] = result.inspection;
    PrintJson(json);
  } else {
    throw eg::UnsupportedSourceError(
        "native weather provider is not yet available: " + provider);
  }
  return 0;
}

int GenerateProvider(const std::vector<std::string>& args) {
  std::string provider;
  std::filesystem::path output;
  bool overwrite = false, dry_run = false;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--provider")
      provider = RequireValue(args, i, args[i]);
    else if (args[i] == "--output")
      output = RequireValue(args, i, args[i]);
    else if (args[i] == "--overwrite")
      overwrite = true;
    else if (args[i] == "--dry-run")
      dry_run = true;
    else if (args[i] == "--json" || args[i] == "--metadata-summary" ||
             args[i] == "--verbose") {
    } else
      throw eg::ValidationError("unknown generate-provider option: " + args[i]);
  }
  if (provider.empty() || output.empty())
    throw eg::ValidationError(
        "generate-provider requires --provider and --output");
  if (provider != "marine_ie_irish_sea")
    throw eg::UnsupportedSourceError(
        "native current provider is not yet available: " + provider);
  if (dry_run) {
    Json::Value result;
    result["provider"] = provider;
    result["output"] = output.string();
    result["dry_run"] = true;
    PrintJson(result);
    return 0;
  }
  PrintJson(
      eg::DirectCurrentResultJson(eg::DownloadMarineIe(output, overwrite)));
  return 0;
}

int GenerateCopernicus(const std::vector<std::string>& args) {
  eg::CopernicusRequest request;
  bool have_bbox = false, have_start = false;
  std::string password_environment = "CURRENTGRIB_COPERNICUS_PASSWORD";
  for (std::size_t i = 1; i < args.size(); ++i) {
    const auto& arg = args[i];
    if (arg == "--provider")
      request.provider = RequireValue(args, i, arg);
    else if (arg == "--bbox") {
      if (i + 4 >= args.size())
        throw eg::ValidationError("--bbox requires W S E N");
      request.bbox = {ParseDouble(args[++i], arg), ParseDouble(args[++i], arg),
                      ParseDouble(args[++i], arg), ParseDouble(args[++i], arg)};
      have_bbox = true;
    } else if (arg == "--start") {
      request.start = eg::ParseUtcDateTime(RequireValue(args, i, arg));
      have_start = true;
    } else if (arg == "--hours")
      request.hours = ParseInt(RequireValue(args, i, arg), arg);
    else if (arg == "--step-hours")
      request.step_hours = ParseInt(RequireValue(args, i, arg), arg);
    else if (arg == "--grid-spacing-deg")
      request.grid_spacing_deg = ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--username")
      request.username = RequireValue(args, i, arg);
    else if (arg == "--password-env")
      password_environment = RequireValue(args, i, arg);
    else if (arg == "--output")
      request.output = RequireValue(args, i, arg);
    else if (arg == "--overwrite")
      request.overwrite = true;
    else if (arg == "--dry-run")
      request.dry_run = true;
    else if (arg == "--timeout-seconds")
      request.timeout_seconds = ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--json" || arg == "--metadata-summary" ||
             arg == "--verbose") {
    } else
      throw eg::ValidationError("unknown generate-copernicus option: " + arg);
  }
  if (const char* password = std::getenv(password_environment.c_str()))
    request.password = password;
  if (!have_bbox || !have_start || request.output.empty())
    throw eg::ValidationError(
        "generate-copernicus requires --bbox, --start, and --output");
  PrintJson(eg::CopernicusResultJson(request.provider == "copernicus_global"
                                         ? eg::GenerateCopernicusGlobal(request)
                                         : eg::GenerateCopernicusNws(request)));
  return 0;
}

int GenerateRtofs(const std::vector<std::string>& args) {
  eg::RtofsRequest request;
  bool have_bbox = false;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const auto& arg = args[i];
    if (arg == "--bbox") {
      if (i + 4 >= args.size())
        throw eg::ValidationError("--bbox requires W S E N");
      request.bbox = {ParseDouble(args[++i], arg), ParseDouble(args[++i], arg),
                      ParseDouble(args[++i], arg), ParseDouble(args[++i], arg)};
      have_bbox = true;
    } else if (arg == "--hours")
      request.hours = ParseInt(RequireValue(args, i, arg), arg);
    else if (arg == "--step-hours")
      request.step_hours = ParseInt(RequireValue(args, i, arg), arg);
    else if (arg == "--cycle")
      request.cycle = RequireValue(args, i, arg);
    else if (arg == "--date")
      request.date = RequireValue(args, i, arg);
    else if (arg == "--download-directory")
      request.download_directory = RequireValue(args, i, arg);
    else if (arg == "--grid-spacing-deg")
      request.grid_spacing_deg = ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--output")
      request.output = RequireValue(args, i, arg);
    else if (arg == "--overwrite")
      request.overwrite = true;
    else if (arg == "--dry-run")
      request.dry_run = true;
    else if (arg == "--timeout-seconds")
      request.timeout_seconds = ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--json" || arg == "--metadata-summary" ||
             arg == "--verbose") {
    } else
      throw eg::ValidationError("unknown generate-rtofs option: " + arg);
  }
  if (!have_bbox || request.output.empty() ||
      request.download_directory.empty())
    throw eg::ValidationError(
        "generate-rtofs requires --bbox, --download-directory, and --output");
  PrintJson(eg::RtofsResultJson(eg::GenerateRtofs(request)));
  return 0;
}

int GenerateEnvironment(const std::vector<std::string>& args) {
  eg::EnvironmentRequest request;
  bool have_bbox = false, have_start = false;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const auto& arg = args[i];
    if (arg == "--bbox") {
      if (i + 4 >= args.size())
        throw eg::ValidationError("--bbox requires W S E N");
      request.bbox = {ParseDouble(args[++i], arg), ParseDouble(args[++i], arg),
                      ParseDouble(args[++i], arg), ParseDouble(args[++i], arg)};
      have_bbox = true;
    } else if (arg == "--start") {
      request.start = eg::ParseUtcDateTime(RequireValue(args, i, arg));
      have_start = true;
    } else if (arg == "--hours")
      request.hours = ParseInt(RequireValue(args, i, arg), arg);
    else if (arg == "--step-hours")
      request.step_hours = ParseInt(RequireValue(args, i, arg), arg);
    else if (arg == "--cycle")
      request.cycle = RequireValue(args, i, arg);
    else if (arg == "--date")
      request.date = RequireValue(args, i, arg);
    else if (arg == "--weather-provider")
      request.weather_provider = RequireValue(args, i, arg);
    else if (arg == "--weather-preset")
      request.weather_preset = RequireValue(args, i, arg);
    else if (arg == "--weather-file")
      request.weather_file = RequireValue(args, i, arg);
    else if (arg == "--include-waves")
      request.include_waves = true;
    else if (arg == "--wave-provider")
      request.wave_provider = RequireValue(args, i, arg);
    else if (arg == "--wave-step-hours")
      request.wave_step_hours = ParseInt(RequireValue(args, i, arg), arg);
    else if (arg == "--current-source")
      request.current_source = RequireValue(args, i, arg);
    else if (arg == "--current-file")
      request.current_file = RequireValue(args, i, arg);
    else if (arg == "--input-netcdf")
      request.input_netcdf = RequireValue(args, i, arg);
    else if (arg == "--input-cache")
      request.input_cache = RequireValue(args, i, arg);
    else if (arg == "--offline-tidal-file")
      request.offline_tidal_file = RequireValue(args, i, arg);
    else if (arg == "--offline-current-mode")
      request.offline_current_mode = RequireValue(args, i, arg);
    else if (arg == "--model-dir" || arg == "--model-directory")
      request.tpxo_model_directory = RequireValue(args, i, arg);
    else if (arg == "--no-infer-minor")
      request.infer_minor_tides = false;
    else if (arg == "--username")
      request.copernicus_username = RequireValue(args, i, arg);
    else if (arg == "--password-env") {
      const auto name = RequireValue(args, i, arg);
      if (const char* password = std::getenv(name.c_str()))
        request.copernicus_password = password;
    } else if (arg == "--download-directory")
      request.download_directory = RequireValue(args, i, arg);
    else if (arg == "--grid-spacing-deg")
      request.current_grid_spacing_deg =
          ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--output")
      request.output = RequireValue(args, i, arg);
    else if (arg == "--overwrite")
      request.overwrite = true;
    else if (arg == "--keep-intermediate")
      request.keep_intermediate = true;
    else if (arg == "--dry-run")
      request.dry_run = true;
    else if (arg == "--weather-grid-spacing-deg")
      request.weather_grid_spacing_deg =
          ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--metadata-summary" || arg == "--verbose" ||
             arg == "--json") {
    } else
      throw eg::ValidationError("unknown generate-environment-grib option: " +
                                arg);
  }
  if (!have_bbox || !have_start || request.output.empty())
    throw eg::ValidationError(
        "generate-environment-grib requires --bbox, --start, and --output");
  auto progress = [](const std::string& stage, const Json::Value& details) {
    std::cerr << stage;
    if (details.isMember("detail"))
      std::cerr << ": " << details["detail"].asString();
    std::cerr << '\n';
  };
  PrintJson(eg::EnvironmentResultJson(
      eg::GenerateEnvironment(request, {}, std::nullopt, progress)));
  return 0;
}

int RunJob(const std::vector<std::string>& args) {
  std::filesystem::path job_path;
  std::filesystem::path result_path;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--job")
      job_path = RequireValue(args, i, args[i]);
    else if (args[i] == "--result")
      result_path = RequireValue(args, i, args[i]);
    else
      throw eg::ValidationError("unknown run-job option: " + args[i]);
  }
  if (job_path.empty() || result_path.empty()) {
    throw eg::ValidationError("run-job requires --job and --result");
  }

  auto running = eg::JobStatusJson("running");
  eg::WriteJsonFileAtomic(result_path, running);
  Json::Value started = running;
  started["event"] = "started";
  PrintEvent(started);

  try {
    auto job = eg::ParseGeneratorJob(ReadJsonFile(job_path));
    if (const char* password =
            std::getenv(job.copernicus_password_environment.c_str())) {
      job.request.copernicus_password = password;
    }
    auto progress = [](const std::string& stage, const Json::Value& details) {
      Json::Value event(Json::objectValue);
      event["schemaVersion"] = eg::kJobSchemaVersion;
      event["event"] = "progress";
      event["stage"] = stage;
      event["details"] = details;
      PrintEvent(event);
    };
    const auto generated =
        eg::GenerateEnvironment(job.request, {}, std::nullopt, progress);
    auto result = eg::JobStatusJson("complete");
    result["result"] = eg::EnvironmentResultJson(generated);
    eg::WriteJsonFileAtomic(result_path, result);
    Json::Value complete = result;
    complete["event"] = "complete";
    PrintEvent(complete);
    return 0;
  } catch (const std::exception& error) {
    auto result = eg::JobStatusJson("failed");
    result["error"]["code"] = "generation_failed";
    result["error"]["message"] = error.what();
    try {
      eg::WriteJsonFileAtomic(result_path, result);
    } catch (const std::exception& write_error) {
      std::cerr << "cannot write failed job result: " << write_error.what()
                << '\n';
    }
    result["event"] = "failed";
    PrintEvent(result);
    return 1;
  }
}

int Capabilities() {
  PrintJson(eg::GeneratorCapabilitiesJson());
  return 0;
}

int Inspect(const std::vector<std::string>& args) {
  if (args.size() != 2)
    throw eg::ValidationError("usage: environmental-grib inspect-grib FILE");
  PrintJson(eg::InspectGrib(args[1]));
  return 0;
}

int Normalize(const std::vector<std::string>& args) {
  if (args.size() != 3)
    throw eg::ValidationError(
        "usage: environmental-grib normalize-grib INPUT OUTPUT");
  const auto result = eg::NormalizeGribStream(args[1], args[2]);
  Json::Value json(Json::objectValue);
  json["message_count"] = Json::UInt64(result.message_count);
  json["raw_byte_count"] = Json::UInt64(result.raw_byte_count);
  json["clean_byte_count"] = Json::UInt64(result.clean_byte_count);
  json["skipped_byte_count"] = Json::UInt64(result.skipped_byte_count);
  PrintJson(json);
  return 0;
}

int Generate(const std::vector<std::string>& args) {
  eg::BoundingBox bbox;
  bool have_bbox = false;
  std::string start;
  int hours = -1, step_hours = 1;
  double spacing = 0.05, u = 0.0, v = 0.0;
  std::string source = "synthetic", units = "mps";
  std::filesystem::path input_netcdf;
  std::filesystem::path input_cache;
  std::filesystem::path model_directory;
  bool infer_minor = true, overwrite = false;
  eg::NetCDFOptions netcdf_options;
  std::filesystem::path output;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const auto& arg = args[i];
    if (arg == "--bbox") {
      if (i + 4 >= args.size())
        throw eg::ValidationError("--bbox requires W S E N");
      bbox = {
          ParseDouble(args[++i], "--bbox"), ParseDouble(args[++i], "--bbox"),
          ParseDouble(args[++i], "--bbox"), ParseDouble(args[++i], "--bbox")};
      have_bbox = true;
    } else if (arg == "--start")
      start = RequireValue(args, i, arg);
    else if (arg == "--hours")
      hours = ParseInt(RequireValue(args, i, arg), arg);
    else if (arg == "--step-hours")
      step_hours = ParseInt(RequireValue(args, i, arg), arg);
    else if (arg == "--grid-spacing-deg")
      spacing = ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--source")
      source = RequireValue(args, i, arg);
    else if (arg == "--input-netcdf")
      input_netcdf = RequireValue(args, i, arg);
    else if (arg == "--input-cache")
      input_cache = RequireValue(args, i, arg);
    else if (arg == "--model-dir" || arg == "--model-directory")
      model_directory = RequireValue(args, i, arg);
    else if (arg == "--no-infer-minor")
      infer_minor = false;
    else if (arg == "--overwrite")
      overwrite = true;
    else if (arg == "--u-variable")
      netcdf_options.u_variable = RequireValue(args, i, arg);
    else if (arg == "--v-variable")
      netcdf_options.v_variable = RequireValue(args, i, arg);
    else if (arg == "--lat-variable")
      netcdf_options.lat_variable = RequireValue(args, i, arg);
    else if (arg == "--lon-variable")
      netcdf_options.lon_variable = RequireValue(args, i, arg);
    else if (arg == "--time-variable")
      netcdf_options.time_variable = RequireValue(args, i, arg);
    else if (arg == "--depth-index")
      netcdf_options.depth_index = ParseInt(RequireValue(args, i, arg), arg);
    else if (arg == "--depth-value")
      netcdf_options.depth_value = ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--assume-units")
      netcdf_options.assume_units = RequireValue(args, i, arg);
    else if (arg == "--nearest-time")
      netcdf_options.nearest_time = true;
    else if (arg == "--use-source-grid")
      netcdf_options.use_source_grid = true;
    else if (arg == "--coverage-tolerance-deg")
      netcdf_options.coverage_tolerance_deg =
          ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--source-grid-regularity-tolerance")
      netcdf_options.source_grid_regularity_tolerance =
          ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--u")
      u = ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--v")
      v = ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--units")
      units = RequireValue(args, i, arg);
    else if (arg == "--output")
      output = RequireValue(args, i, arg);
    else if (arg == "--json" || arg == "--metadata-summary") {
    } else
      throw eg::ValidationError("unknown generate option: " + arg);
  }
  if (source == "tpxo-cache") {
    if (start.empty() || hours < 0 || output.empty() || input_cache.empty())
      throw eg::ValidationError(
          "tpxo-cache generation requires --input-cache, --start, --hours, and "
          "--output");
    const auto generated = eg::GenerateFromTpxoCache(
        input_cache, eg::ParseUtcDateTime(start), hours, step_hours, output,
        infer_minor, overwrite);
    Json::Value result(Json::objectValue);
    result["source"] = source;
    result["output"] = generated.output.string();
    result["message_count"] = Json::UInt64(generated.message_count);
    result["byte_count"] = Json::UInt64(generated.byte_count);
    result["inspection"] = generated.inspection;
    PrintJson(result);
    return 0;
  }
  if (source == "tpxo") {
    if (!have_bbox || start.empty() || hours < 0 || output.empty() ||
        model_directory.empty())
      throw eg::ValidationError(
          "tpxo generation requires --model-dir, --bbox, --start, --hours, and "
          "--output");
    eg::TpxoModelRequest request{bbox,     eg::ParseUtcDateTime(start),
                                 hours,    step_hours,
                                 spacing,  model_directory,
                                 output,   infer_minor,
                                 overwrite};
    const auto generated = eg::GenerateFromTpxo10AtlasModel(request);
    Json::Value result(Json::objectValue);
    result["source"] = source;
    result["output"] = generated.output.string();
    result["message_count"] = Json::UInt64(generated.message_count);
    result["byte_count"] = Json::UInt64(generated.byte_count);
    result["inspection"] = generated.inspection;
    PrintJson(result);
    return 0;
  }
  if (!have_bbox || start.empty() || hours < 0 || output.empty()) {
    throw eg::ValidationError(
        "generate requires --bbox, --start, --hours, and --output");
  }
  bbox.Validate();
  std::optional<eg::NetCDFCurrentSource> netcdf;
  if (source == "netcdf") {
    if (input_netcdf.empty())
      throw eg::ValidationError("netcdf source requires --input-netcdf");
    netcdf.emplace(input_netcdf, netcdf_options);
  }
  const auto grid = netcdf && netcdf_options.use_source_grid
                        ? netcdf->BuildSourceGrid(bbox)
                        : eg::BuildRegularGrid(bbox, spacing);
  const auto times =
      eg::BuildTimeSequence(eg::ParseUtcDateTime(start), hours, step_hours);
  std::vector<eg::CurrentGrid> fields;
  fields.reserve(times.size());
  for (const auto time : times) {
    if (source == "synthetic")
      fields.push_back(eg::MakeSyntheticRotaryCurrent(bbox, time, grid));
    else if (source == "constant")
      fields.push_back(eg::MakeConstantCurrent(bbox, time, grid, u, v, units));
    else if (source == "netcdf")
      fields.push_back(netcdf->GetCurrentGrid(bbox, time, grid));
    else
      throw eg::UnsupportedSourceError(
          "native provider is not yet available: " + source);
  }
  const auto written = eg::WriteGrib1Currents(fields, output);
  const auto inspected = eg::InspectGrib(output);
  Json::Value result(Json::objectValue);
  result["source"] = source;
  result["output"] = output.string();
  result["message_count"] = Json::UInt64(written.message_count);
  result["grid_size"]["nx"] = Json::UInt64(grid.nx());
  result["grid_size"]["ny"] = Json::UInt64(grid.ny());
  result["grid_size"]["points"] = Json::UInt64(grid.size());
  result["time_range"]["start"] = eg::FormatUtcDateTime(times.front());
  result["time_range"]["end"] = eg::FormatUtcDateTime(times.back());
  result["time_range"]["step_count"] = Json::UInt64(times.size());
  result["inspection"] = inspected;
  PrintJson(result);
  return 0;
}

int InspectTpxo(const std::vector<std::string>& args) {
  if (args.size() != 2)
    throw eg::ValidationError(
        "usage: environmental-grib inspect-tpxo-cache FILE");
  PrintJson(eg::InspectTpxoCache(args[1]));
  return 0;
}

int InspectXtdPackage(const std::vector<std::string>& args) {
  if (args.size() != 2) {
    throw eg::ValidationError("usage: environmental-grib inspect-xtd FILE");
  }
  PrintJson(eg::InspectXtdPackage(args[1]));
  return 0;
}

int VerifyXtdPackage(const std::vector<std::string>& args) {
  if (args.size() != 2) {
    throw eg::ValidationError("usage: environmental-grib verify-xtd FILE");
  }
  PrintJson(eg::VerifyXtdPackage(args[1]));
  return 0;
}

int SampleXtdPackage(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    throw eg::ValidationError(
        "usage: environmental-grib sample-xtd FILE --latitude LAT "
        "--longitude LON --time UTC [--mode MODE]");
  }
  const std::filesystem::path path = args[1];
  std::optional<double> latitude;
  std::optional<double> longitude;
  std::optional<eg::TimePoint> time;
  std::string mode = "tide-only";
  for (std::size_t i = 2; i < args.size(); ++i) {
    const auto& arg = args[i];
    if (arg == "--latitude")
      latitude = ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--longitude")
      longitude = ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--time")
      time = eg::ParseUtcDateTime(RequireValue(args, i, arg));
    else if (arg == "--mode")
      mode = RequireValue(args, i, arg);
    else
      throw eg::ValidationError("unknown sample-xtd option: " + arg);
  }
  if (!latitude || !longitude || !time) {
    throw eg::ValidationError(
        "sample-xtd requires --latitude, --longitude, and --time");
  }
  if (*latitude < -90.0 || *latitude > 90.0) {
    throw eg::ValidationError("sample-xtd latitude is outside [-90, 90]");
  }
  eg::RegularGrid grid;
  grid.longitudes = {*longitude};
  grid.latitudes = {*latitude};
  eg::XtdPackageReader reader(path);
  const auto selected_mode = eg::ParseOfflineCurrentMode(mode);
  const auto fields = reader.Predict(grid, {*time}, selected_mode);
  const auto& field = fields.front();
  const bool valid = (!field.has_mask() || field.mask.front()) &&
                     std::isfinite(field.u_mps.front()) &&
                     std::isfinite(field.v_mps.front());
  Json::Value result(Json::objectValue);
  result["package"] = path.string();
  result["package_id"] = reader.status().package_id;
  result["format_version"] = reader.status().format_version;
  result["mode"] = eg::OfflineCurrentModeId(selected_mode);
  result["latitude"] = *latitude;
  result["longitude"] = *longitude;
  result["time"] = eg::FormatUtcDateTime(*time);
  result["valid"] = valid;
  if (valid) {
    const double u = field.u_mps.front();
    const double v = field.v_mps.front();
    const double speed = std::hypot(u, v);
    result["u_mps"] = u;
    result["v_mps"] = v;
    result["speed_mps"] = speed;
    result["speed_knots"] = speed * 1.9438444924406;
    if (speed >= 1e-4) {
      double direction = std::atan2(u, v) * 180.0 / std::numbers::pi;
      if (direction < 0) direction += 360.0;
      result["direction_to_degrees"] = direction;
    } else {
      result["direction_to_degrees"] = Json::nullValue;
    }
  }
  const auto stats = reader.statistics();
  result["diagnostics"]["tide_tiles_loaded"] =
      Json::UInt64(stats.tide.tiles_loaded);
  result["diagnostics"]["residual_tiles_loaded"] =
      Json::UInt64(stats.residual.tiles_loaded);
  result["diagnostics"]["uncertainty_tiles_loaded"] =
      Json::UInt64(stats.uncertainty.tiles_loaded);
  result["diagnostics"]["package_bytes_read"] =
      Json::UInt64(stats.outer_bytes_read);
  PrintJson(result);
  return 0;
}

int PrepareTpxo(const std::vector<std::string>& args) {
  eg::BoundingBox bbox;
  bool have_bbox = false, overwrite = false;
  double spacing = 0.05;
  std::filesystem::path model_directory, output;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const auto& arg = args[i];
    if (arg == "--bbox") {
      if (i + 4 >= args.size())
        throw eg::ValidationError("--bbox requires W S E N");
      bbox = {ParseDouble(args[++i], arg), ParseDouble(args[++i], arg),
              ParseDouble(args[++i], arg), ParseDouble(args[++i], arg)};
      have_bbox = true;
    } else if (arg == "--grid-spacing-deg")
      spacing = ParseDouble(RequireValue(args, i, arg), arg);
    else if (arg == "--model-dir" || arg == "--model-directory")
      model_directory = RequireValue(args, i, arg);
    else if (arg == "--output")
      output = RequireValue(args, i, arg);
    else if (arg == "--overwrite")
      overwrite = true;
    else if (arg == "--json" || arg == "--verbose") {
    } else
      throw eg::ValidationError("unknown prepare-tpxo-cache option: " + arg);
  }
  if (!have_bbox || model_directory.empty() || output.empty())
    throw eg::ValidationError(
        "prepare-tpxo-cache requires --bbox, --model-dir, and --output");
  PrintJson(eg::PrepareTpxo10Cache(model_directory, bbox, spacing, output,
                                   overwrite));
  return 0;
}

int Merge(const std::vector<std::string>& args) {
  std::vector<std::pair<std::string, std::filesystem::path>> inputs;
  std::filesystem::path output;
  bool overwrite = false;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--input") {
      const auto value = RequireValue(args, i, args[i]);
      const auto equals = value.find('=');
      if (equals == std::string::npos || equals == 0 ||
          equals + 1 == value.size())
        throw eg::ValidationError("--input must be LABEL=PATH");
      inputs.emplace_back(value.substr(0, equals), value.substr(equals + 1));
    } else if (args[i] == "--output")
      output = RequireValue(args, i, args[i]);
    else if (args[i] == "--overwrite")
      overwrite = true;
    else
      throw eg::ValidationError("unknown merge option: " + args[i]);
  }
  if (output.empty())
    throw eg::ValidationError("merge-gribs requires --output");
  const auto merged = eg::MergeGribStreams(inputs, output, overwrite);
  Json::Value result(Json::objectValue);
  result["output"] = output.string();
  result["output_message_count"] = Json::UInt64(merged.output_message_count);
  result["byte_count"] = Json::UInt64(merged.byte_count);
  for (const auto& [label, count] : merged.input_message_counts)
    result["input_message_counts"][label] = Json::UInt64(count);
  result["inspection"] = merged.inspection;
  PrintJson(result);
  return 0;
}

void Usage() {
  std::cerr
      << "Environmental GRIB Generator C++\n"
      << "Commands:\n"
      << "  providers\n  weather-providers\n  generate [options]\n"
      << "  generate-weather [options]\n  inspect-grib FILE\n"
      << "  generate-provider [options]\n"
      << "  generate-copernicus [options]\n"
      << "  generate-rtofs [options]\n"
      << "  generate-environment-grib [options]\n"
      << "  run-job --job FILE --result FILE\n"
      << "  capabilities\n"
      << "  inspect-tpxo-cache FILE\n"
      << "  inspect-xtd FILE\n"
      << "  verify-xtd FILE\n"
      << "  sample-xtd FILE --latitude LAT --longitude LON --time UTC "
         "[--mode MODE]\n"
      << "  prepare-tpxo-cache [options]\n"
      << "  normalize-grib INPUT OUTPUT\n"
      << "  merge-gribs --input LABEL=FILE... --output FILE [--overwrite]\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      Usage();
      return 2;
    }
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    if (args[0] == "providers") return Providers();
    if (args[0] == "weather-providers") return WeatherProviders();
    if (args[0] == "generate") return Generate(args);
    if (args[0] == "generate-weather") return GenerateWeather(args);
    if (args[0] == "generate-provider") return GenerateProvider(args);
    if (args[0] == "generate-copernicus") return GenerateCopernicus(args);
    if (args[0] == "generate-rtofs") return GenerateRtofs(args);
    if (args[0] == "generate-environment-grib")
      return GenerateEnvironment(args);
    if (args[0] == "run-job") return RunJob(args);
    if (args[0] == "capabilities") return Capabilities();
    if (args[0] == "inspect-tpxo-cache") return InspectTpxo(args);
    if (args[0] == "inspect-xtd") return InspectXtdPackage(args);
    if (args[0] == "verify-xtd") return VerifyXtdPackage(args);
    if (args[0] == "sample-xtd") return SampleXtdPackage(args);
    if (args[0] == "prepare-tpxo-cache") return PrepareTpxo(args);
    if (args[0] == "inspect-grib") return Inspect(args);
    if (args[0] == "normalize-grib") return Normalize(args);
    if (args[0] == "merge-gribs") return Merge(args);
    Usage();
    throw eg::ValidationError("unknown command: " + args[0]);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
