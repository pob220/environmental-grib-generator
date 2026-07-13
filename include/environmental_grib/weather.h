#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <json/json.h>

#include "environmental_grib/geo.h"

namespace environmental_grib {

struct WeatherProvider {
  std::string id;
  std::string label;
  std::string source;
  std::string format;
  std::string account;
  std::string description;
  bool implemented{true};
};

struct GFSCycle {
  std::string date;
  std::string cycle;
  [[nodiscard]] std::string Directory() const;
  [[nodiscard]] std::string CycleTime() const;
};

struct GFSRequest {
  BoundingBox bbox;
  std::filesystem::path output;
  int hours{};
  int step_hours{3};
  std::string cycle{"auto"};
  std::optional<std::string> date;
  bool overwrite{false};
  double timeout_seconds{60.0};
  int max_auto_cycles{8};
  bool dry_run{false};
  std::string preset{"routing"};
  bool waves{false};
};

struct WeatherGenerateResult {
  std::string provider;
  std::string source;
  std::string model;
  GFSCycle cycle;
  BoundingBox bbox;
  std::vector<int> forecast_hours;
  std::filesystem::path output;
  std::size_t byte_count{};
  std::size_t message_count{};
  Json::Value inspection;
  std::vector<std::string> urls;
  std::map<std::string, std::string> variables_levels;
};

using HttpGet = std::function<std::vector<unsigned char>(const std::string&, double)>;
using HttpGetRange = std::function<std::vector<unsigned char>(
    const std::string&, std::size_t, std::size_t, double)>;
using ProgressCallback = std::function<void(const std::string&, const Json::Value&)>;

struct HttpRetryPolicy {
  int max_attempts{3};
  int initial_delay_ms{1000};
  int maximum_delay_ms{4000};
};

using RetrySleeper = std::function<void(int)>;

std::string SanitizedHttpResource(const std::string& url);
HttpGet MakeRetryingHttpGet(
    HttpGet download, const std::string& provider,
    ProgressCallback progress = {}, HttpRetryPolicy policy = {},
    RetrySleeper sleeper = {});
HttpGetRange MakeRetryingHttpGetRange(
    HttpGetRange download, const std::string& provider,
    ProgressCallback progress = {}, HttpRetryPolicy policy = {},
    RetrySleeper sleeper = {});

std::vector<WeatherProvider> ListWeatherProviders();
std::vector<int> ForecastHourSequence(int hours, int step_hours);
std::map<std::string, std::string> GfsVariablesForPreset(const std::string& preset);
std::vector<GFSCycle> GfsCycleCandidates(const GFSRequest& request,
                                         std::optional<TimePoint> now = std::nullopt);
std::string BuildGfsFilterUrl(const GFSCycle& cycle, int forecast_hour,
                              const BoundingBox& bbox,
                              const std::map<std::string, std::string>& fields);
std::string BuildGfsWaveFilterUrl(const GFSCycle& cycle, int forecast_hour,
                                  const BoundingBox& bbox);
std::vector<unsigned char> CurlHttpGet(const std::string& url,
                                       double timeout_seconds);
WeatherGenerateResult GenerateGfs(const GFSRequest& request,
                                  HttpGet http_get = {},
                                  std::optional<TimePoint> now = std::nullopt,
                                  ProgressCallback progress = {});
std::vector<int> DwdIconEuForecastHourSequence(int hours, int step_hours);
std::map<std::string, std::string> DwdIconEuFieldsForPreset(
    const std::string& preset);
std::vector<GFSCycle> DwdIconEuCycleCandidates(
    const GFSRequest& request, std::optional<TimePoint> now = std::nullopt);
std::string BuildDwdIconEuUrl(const GFSCycle& cycle, int forecast_hour,
                              const std::string& field);
WeatherGenerateResult GenerateDwdIconEu(
    const GFSRequest& request, HttpGet http_get = {},
    std::optional<TimePoint> now = std::nullopt,
    ProgressCallback progress = {});
struct HrrrInventoryEntry {
  int message_number{};
  std::size_t offset{};
  std::string short_name;
  std::string level;
  std::string forecast;
};
std::vector<HrrrInventoryEntry> ParseHrrrInventory(const std::string& text);
std::vector<int> HrrrForecastHourSequence(int hours, int step_hours);
std::map<std::string, std::string> HrrrVariablesForPreset(
    const std::string& preset);
std::string BuildHrrrFileUrl(const GFSCycle& cycle, int forecast_hour);
std::string BuildHrrrIndexUrl(const GFSCycle& cycle, int forecast_hour);
std::vector<unsigned char> CurlHttpGetRange(const std::string& url,
                                            std::size_t start,
                                            std::size_t end,
                                            double timeout_seconds);
WeatherGenerateResult GenerateHrrr(
    const GFSRequest& request, HttpGet http_get = {},
    HttpGetRange http_get_range = {},
    std::optional<TimePoint> now = std::nullopt,
    ProgressCallback progress = {});
std::string BuildEcmwfDataUrl(const GFSCycle& cycle, int forecast_hour,
                              bool aifs);
std::string BuildEcmwfIndexUrl(const GFSCycle& cycle, int forecast_hour,
                               bool aifs);
WeatherGenerateResult GenerateEcmwfOpenData(
    const GFSRequest& request, bool aifs, HttpGet http_get = {},
    HttpGetRange http_get_range = {},
    std::optional<TimePoint> now = std::nullopt,
    ProgressCallback progress = {});
Json::Value WeatherResultJson(const WeatherGenerateResult& result);

}  // namespace environmental_grib
