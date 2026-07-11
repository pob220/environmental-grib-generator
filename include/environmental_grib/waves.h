#pragma once

#include <filesystem>
#include <optional>

#include <json/json.h>

#include "environmental_grib/geo.h"
#include "environmental_grib/copernicus.h"

namespace environmental_grib {

struct WaveConvertResult {
  std::filesystem::path input;
  std::filesystem::path output;
  std::size_t message_count{};
  std::size_t byte_count{};
  Json::Value inspection;
};

WaveConvertResult ConvertCopernicusWaveNetCDF(
    const std::filesystem::path& input, const BoundingBox& bbox,
    TimePoint start, int hours, int step_hours,
    const std::filesystem::path& output,
    std::optional<double> grid_spacing_deg = std::nullopt,
    bool overwrite = false);

WaveConvertResult GenerateCopernicusGlobalWaves(
    const BoundingBox& bbox, TimePoint start, int hours, int step_hours,
    const std::string& username, const std::string& password,
    const std::filesystem::path& output,
    std::optional<double> grid_spacing_deg = std::nullopt,
    bool overwrite = false, BinaryDownload download = {},
    CredentialValidator validate_credentials = {},
    double timeout_seconds = 180.0);

}  // namespace environmental_grib
