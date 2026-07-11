#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <json/json.h>

#include "environmental_grib/geo.h"
#include "environmental_grib/remote_currents.h"

namespace environmental_grib {

inline constexpr int kRtofsMaxForecastHour = 192;
inline constexpr int kRtofsNativeStepHours = 6;

struct RtofsCycle {
  std::string date;
  std::string cycle{"00"};
  [[nodiscard]] TimePoint CycleTime() const;
  [[nodiscard]] std::string DirectoryName() const;
};

struct RtofsRequest {
  BoundingBox bbox;
  std::filesystem::path output;
  int hours{};
  int step_hours{6};
  std::string cycle{"auto"};
  std::optional<std::string> date;
  std::filesystem::path download_directory;
  double grid_spacing_deg{0.03};
  bool overwrite{false};
  bool dry_run{false};
  int max_auto_days{5};
  double timeout_seconds{120.0};
};

struct RtofsResult {
  std::filesystem::path output;
  std::size_t message_count{};
  std::size_t byte_count{};
  std::string selected_cycle;
  std::vector<int> forecast_hours;
  std::vector<std::filesystem::path> source_files;
  Json::Value summary;
};

using TextDownload = std::function<std::string(const std::string&, double)>;

std::vector<int> RtofsForecastHours(int requested_hours, int step_hours);
std::string RtofsRegionForBbox(const BoundingBox& bbox);
std::string RtofsDirectoryUrl(const RtofsCycle& cycle);
std::string RtofsFilename(int forecast_hour, const std::string& region);
std::string RtofsUrl(const RtofsCycle& cycle, int forecast_hour,
                     const std::string& region);
RtofsResult GenerateRtofs(const RtofsRequest& request,
                          TextDownload text_download = {},
                          BinaryDownload binary_download = {});
Json::Value RtofsResultJson(const RtofsResult& result);

}  // namespace environmental_grib
