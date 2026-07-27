#pragma once

#include <filesystem>
#include <string>

#include "environmental_grib/weather.h"

namespace environmental_grib {

inline constexpr BoundingBox kMetNoNordicDomain{-20.0, 51.0, 80.0, 88.0};
inline constexpr const char* kMetNoNordicLatestDataset =
    "https://thredds.met.no/thredds/dodsC/metpplatest/"
    "met_forecast_1_0km_nordic_latest.nc";

struct MetNoRequest {
  BoundingBox bbox;
  std::filesystem::path output;
  int hours{};
  int step_hours{1};
  bool overwrite{false};
  bool dry_run{false};
  std::string preset{"routing"};
  double grid_spacing_deg{0.025};
  std::string dataset_url{kMetNoNordicLatestDataset};
};

std::vector<int> MetNoForecastHours(int hours, int step_hours);
WeatherGenerateResult GenerateMetNoNordic(const MetNoRequest& request,
                                          ProgressCallback progress = {});
bool MetNoProjectionAvailable();

}  // namespace environmental_grib
