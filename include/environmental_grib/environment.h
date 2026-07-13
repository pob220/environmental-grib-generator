#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <json/json.h>

#include "environmental_grib/geo.h"
#include "environmental_grib/weather.h"

namespace environmental_grib {

struct EnvironmentRequest {
  BoundingBox bbox;
  TimePoint start;
  int hours{};
  int step_hours{3};
  std::string cycle{"auto"};
  std::optional<std::string> date;
  std::string weather_provider{"gfs"};
  std::string weather_preset{"routing"};
  double weather_grid_spacing_deg{0.025};
  std::optional<std::filesystem::path> weather_file;
  bool include_waves{false};
  std::string wave_provider{"gfs_wave"};
  int wave_step_hours{3};
  std::string current_source{"none"};
  std::optional<std::filesystem::path> current_file;
  std::optional<std::filesystem::path> input_netcdf;
  std::optional<std::filesystem::path> input_cache;
  std::optional<std::filesystem::path> offline_tidal_file;
  std::optional<std::filesystem::path> tpxo_model_directory;
  bool auto_prepare_tpxo_cache{false};
  std::filesystem::path download_directory;
  std::string copernicus_username;
  std::string copernicus_password;
  double current_grid_spacing_deg{0.05};
  bool infer_minor_tides{true};
  std::filesystem::path output;
  bool overwrite{false};
  bool keep_intermediate{false};
  bool dry_run{false};
};

struct EnvironmentResult {
  std::filesystem::path output;
  std::size_t message_count{};
  std::size_t byte_count{};
  std::string weather_provider;
  std::string wave_provider;
  std::string current_source;
  std::optional<std::string> selected_cycle;
  std::vector<std::filesystem::path> inputs;
  Json::Value inspection;
  Json::Value diagnostics{Json::objectValue};
};

EnvironmentResult GenerateEnvironment(const EnvironmentRequest& request,
                                      HttpGet http_get = {},
                                      std::optional<TimePoint> now = std::nullopt,
                                      ProgressCallback progress = {});
Json::Value EnvironmentResultJson(const EnvironmentResult& result);

}  // namespace environmental_grib
