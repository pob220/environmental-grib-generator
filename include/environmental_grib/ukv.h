#pragma once

#include <filesystem>
#include <optional>

#include "environmental_grib/weather.h"

namespace environmental_grib {

inline constexpr BoundingBox kUkvDomain{-12.0, 48.0, 4.0, 62.0};

struct UkvRequest {
  BoundingBox bbox;
  std::filesystem::path output;
  int hours{};
  int step_hours{1};
  std::string cycle{"auto"};
  std::optional<std::string> date;
  bool overwrite{false};
  bool dry_run{false};
  std::string preset{"routing"};
  double grid_spacing_deg{0.025};
  double timeout_seconds{180.0};
  int max_auto_days{5};
};

std::vector<int> UkvForecastHours(int hours, int step_hours);
std::string UkvSourceKey(const std::string& cycle, int forecast_hour,
                         const std::string& field_token);
WeatherGenerateResult GenerateUkv(
    const UkvRequest& request, HttpGet download = {},
    std::optional<TimePoint> now = std::nullopt,
    ProgressCallback progress = {});
bool UkvProjectionAvailable();

}  // namespace environmental_grib
