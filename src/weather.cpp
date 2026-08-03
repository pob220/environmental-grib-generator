#include "environmental_grib/weather.h"

#include <curl/curl.h>
#include <bzlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <semaphore>
#include <sstream>
#include <set>
#include <thread>

#include "environmental_grib/error.h"
#include "environmental_grib/ukv.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/parallel.h"
#include "environmental_grib/platform.h"

namespace environmental_grib {
namespace {

constexpr const char* kGfsEndpoint =
    "https://nomads.ncep.noaa.gov/cgi-bin/filter_gfs_0p25.pl";
constexpr const char* kGfsWaveEndpoint =
    "https://nomads.ncep.noaa.gov/cgi-bin/filter_gfswave.pl";
constexpr const char* kGfsProductionBase =
    "https://nomads.ncep.noaa.gov/pub/data/nccf/com/gfs/prod";
constexpr const char* kDwdIconEuBase =
    "https://opendata.dwd.de/weather/nwp/icon-eu/grib";
constexpr const char* kHrrrBase =
    "https://nomads.ncep.noaa.gov/pub/data/nccf/com/hrrr/prod";

const std::map<std::string, std::string> kRoutingFields{
    {"var_UGRD", "on"},
    {"var_VGRD", "on"},
    {"var_PRES", "on"},
    {"var_TMP", "on"},
    {"lev_10_m_above_ground", "on"},
    {"lev_mean_sea_level", "on"},
    {"lev_2_m_above_ground", "on"}};
const std::map<std::string, std::string> kMinimalFields{
    {"var_UGRD", "on"}, {"var_VGRD", "on"}, {"lev_10_m_above_ground", "on"}};
const std::map<std::string, std::string> kMarineFields{
    {"var_GUST", "on"},
    {"var_TCDC", "on"},
    {"var_APCP", "on"},
    {"lev_surface", "on"},
    {"lev_entire_atmosphere", "on"}};
const std::map<std::string, std::string> kAllDisplayableFields{
    {"var_GUST", "on"},
    {"var_TCDC", "on"},
    {"var_APCP", "on"},
    {"var_CAPE", "on"},
    {"var_REFC", "on"},
    {"var_RH", "on"},
    {"var_HGT", "on"},
    {"lev_surface", "on"},
    {"lev_entire_atmosphere", "on"},
    {"lev_850_mb", "on"},
    {"lev_700_mb", "on"},
    {"lev_500_mb", "on"},
    {"lev_300_mb", "on"}};
const std::map<std::string, std::string> kWaveFields{{"var_HTSGW", "on"},
                                                     {"var_PERPW", "on"},
                                                     {"var_DIRPW", "on"},
                                                     {"lev_surface", "on"}};
const std::map<std::string, std::string> kDwdRoutingFields{
    {"10u", "u_10m"}, {"10v", "v_10m"}, {"prmsl", "pmsl"}, {"2t", "t_2m"}};
const std::map<std::string, std::string> kDwdMinimalFields{{"10u", "u_10m"},
                                                           {"10v", "v_10m"}};
const std::map<std::string, std::string> kDwdMarineFields{
    {"10u", "u_10m"},   {"10v", "v_10m"}, {"prmsl", "pmsl"},
    {"2t", "t_2m"},     {"10fg", "vmax_10m"},
    {"tp", "tot_prec"}, {"tcc", "clct"}};
const std::map<std::string, std::string> kDwdAllDisplayableFields{
    {"10u", "u_10m"},       {"10v", "v_10m"},   {"prmsl", "pmsl"},
    {"2t", "t_2m"},         {"10fg", "vmax_10m"},
    {"tp", "tot_prec"},     {"tcc", "clct"},     {"cape", "cape_ml"},
    {"2r", "relhum_2m"}};
const std::map<std::string, std::string> kHrrrRoutingFields{
    {"var_UGRD", "on"},
    {"var_VGRD", "on"},
    {"var_PRES", "on"},
    {"var_TMP", "on"},
    {"lev_10_m_above_ground", "on"},
    {"lev_surface", "on"},
    {"lev_2_m_above_ground", "on"}};
const std::map<std::string, std::string> kHrrrMinimalFields{
    {"var_UGRD", "on"}, {"var_VGRD", "on"}, {"lev_10_m_above_ground", "on"}};
const std::map<std::string, std::string> kHrrrMarineFields{
    {"var_GUST", "on"},
    {"var_APCP", "on"},
    {"var_TCDC", "on"},
    {"lev_surface", "on"},
    {"lev_entire_atmosphere", "on"}};
const std::map<std::string, std::string> kHrrrAllDisplayableFields{
    {"var_UGRD", "on"},
    {"var_VGRD", "on"},
    {"var_PRES", "on"},
    {"var_TMP", "on"},
    {"var_GUST", "on"},
    {"var_APCP", "on"},
    {"var_TCDC", "on"},
    {"var_CAPE", "on"},
    {"var_REFC", "on"},
    {"var_RH", "on"},
    {"var_HGT", "on"},
    {"lev_10_m_above_ground", "on"},
    {"lev_2_m_above_ground", "on"},
    {"lev_surface", "on"},
    {"lev_entire_atmosphere", "on"},
    {"lev_850_mb", "on"},
    {"lev_700_mb", "on"},
    {"lev_500_mb", "on"},
    {"lev_300_mb", "on"}};

std::string FormatNumber(double value) {
  std::ostringstream stream;
  stream << std::setprecision(15) << value;
  return stream.str();
}

std::string FormatHour(int hour, int width = 3) {
  std::ostringstream stream;
  stream << std::setw(width) << std::setfill('0') << hour;
  return stream.str();
}

std::string Encode(const std::string& value) {
  CURL* curl = curl_easy_init();
  if (!curl) throw Error("libcurl initialization failed");
  char* encoded =
      curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
  if (!encoded) {
    curl_easy_cleanup(curl);
    throw Error("URL encoding failed");
  }
  std::string result(encoded);
  curl_free(encoded);
  curl_easy_cleanup(curl);
  return result;
}

std::string Query(
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::ostringstream result;
  bool first = true;
  for (const auto& [key, value] : values) {
    result << (first ? '?' : '&') << Encode(key) << '=' << Encode(value);
    first = false;
  }
  return result.str();
}

void ValidateDate(const std::string& date) {
  if (date.size() != 8 || !std::all_of(date.begin(), date.end(), ::isdigit)) {
    throw ValidationError("date must use YYYYMMDD");
  }
  ParseUtcDateTime(date.substr(0, 4) + "-" + date.substr(4, 2) + "-" +
                   date.substr(6, 2) + "T00:00:00Z");
}

std::filesystem::path TemporarySibling(const std::filesystem::path& output) {
  auto path = output;
  path += "." + std::to_string(ProcessId()) + ".tmp";
  return path;
}

void ValidateDownloaded(const std::vector<unsigned char>& bytes,
                        const std::string& provider) {
  if (bytes.empty())
    throw ValidationError(provider + " download returned empty response");
  auto first =
      std::find_if_not(bytes.begin(), bytes.end(), [](unsigned char value) {
        return value == ' ' || value == '\r' || value == '\n' || value == '\t';
      });
  if (first != bytes.end() && *first == '<') {
    throw ValidationError(provider +
                          " download returned HTML/text instead of GRIB2");
  }
  const std::size_t prefix = std::min<std::size_t>(32, bytes.size());
  static constexpr std::array<unsigned char, 4> kMarker{'G', 'R', 'I', 'B'};
  if (std::search(bytes.begin(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(prefix),
                  kMarker.begin(), kMarker.end()) ==
      bytes.begin() + static_cast<std::ptrdiff_t>(prefix)) {
    throw ValidationError(provider +
                          " download did not start with a GRIB message");
  }
  if (ScanGribBytes(bytes).message_count == 0) {
    throw ValidationError(provider + " download contained no GRIB messages");
  }
}

std::size_t CurlWrite(void* data, std::size_t size, std::size_t count,
                      void* target) {
  const std::size_t bytes = size * count;
  auto* output = static_cast<std::vector<unsigned char>*>(target);
  const auto* first = static_cast<unsigned char*>(data);
  output->insert(output->end(), first, first + bytes);
  return bytes;
}

CURL* ThreadCurlHandle() {
  EnsureHttpInitialized();
  thread_local std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(
      curl_easy_init(), &curl_easy_cleanup);
  if (!handle) throw Error("libcurl initialization failed");
  curl_easy_reset(handle.get());
  return handle.get();
}

std::vector<std::string> Split(const std::string& value, char separator) {
  std::vector<std::string> result;
  std::istringstream stream(value);
  std::string part;
  while (std::getline(stream, part, separator)) result.push_back(part);
  return result;
}

std::vector<GFSCycle> SixHourlyCycles(const GFSRequest& request,
                                      std::optional<TimePoint> now) {
  if (request.cycle != "auto") {
    static const std::set<std::string> valid{"00", "06", "12", "18"};
    if (!valid.contains(request.cycle) || !request.date)
      throw ValidationError(
          "explicit cycle requires date and 00, 06, 12, or 18");
    ValidateDate(*request.date);
    return {{*request.date, request.cycle}};
  }
  TimePoint value = now.value_or(std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now()));
  std::time_t raw = static_cast<std::time_t>(value.time_since_epoch().count());
  std::tm tm = UtcTime(raw);
  tm.tm_hour = (tm.tm_hour / 6) * 6;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  TimePoint cursor{std::chrono::seconds{UtcTimeToEpoch(&tm)}};
  std::vector<GFSCycle> result;
  for (int i = 0; i < std::max(1, request.max_auto_cycles); ++i) {
    raw = static_cast<std::time_t>(cursor.time_since_epoch().count());
    tm = UtcTime(raw);
    std::ostringstream date, cycle;
    date << std::put_time(&tm, "%Y%m%d");
    cycle << std::put_time(&tm, "%H");
    result.push_back({date.str(), cycle.str()});
    cursor -= std::chrono::hours{6};
  }
  return result;
}

void Progress(const ProgressCallback& callback, const std::string& stage,
              const Json::Value& details) {
  if (callback) callback(stage, details);
}

class DownloadPermit {
public:
  DownloadPermit() { Slots().acquire(); }
  ~DownloadPermit() { Slots().release(); }

private:
  static std::counting_semaphore<64>& Slots() {
    static std::counting_semaphore<64> slots(8);
    return slots;
  }
};

bool TransientCurlError(CURLcode status) {
  switch (status) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_PARTIAL_FILE:
    case CURLE_HTTP2:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
      return true;
    default:
      return false;
  }
}

bool TransientHttpStatus(long status) {
  return status == 408 || status == 425 || status == 429 ||
         (status >= 500 && status <= 599);
}

std::vector<unsigned char> DecompressBzip2(
    const std::vector<unsigned char>& compressed) {
  if (compressed.empty())
    throw ValidationError("DWD ICON-EU download returned empty response");
  std::size_t capacity = std::max<std::size_t>(compressed.size() * 8, 1024);
  constexpr std::size_t kLimit = 1024ULL * 1024ULL * 1024ULL;
  while (capacity <= kLimit) {
    std::vector<unsigned char> output(capacity);
    unsigned int output_size = static_cast<unsigned int>(output.size());
    const int status = BZ2_bzBuffToBuffDecompress(
        reinterpret_cast<char*>(output.data()), &output_size,
        const_cast<char*>(reinterpret_cast<const char*>(compressed.data())),
        static_cast<unsigned int>(compressed.size()), 0, 0);
    if (status == BZ_OK) {
      output.resize(output_size);
      return output;
    }
    if (status != BZ_OUTBUFF_FULL) {
      throw ValidationError(
          "DWD ICON-EU compressed GRIB could not be decompressed");
    }
    capacity *= 2;
  }
  throw ValidationError(
      "DWD ICON-EU decompressed field exceeds 1 GiB safety limit");
}

Json::Value BboxJson(const BoundingBox& bbox) {
  Json::Value value(Json::objectValue);
  value["west"] = bbox.west;
  value["south"] = bbox.south;
  value["east"] = bbox.east;
  value["north"] = bbox.north;
  return value;
}

}  // namespace

ProgressCallback SynchronizedProgressCallback(ProgressCallback progress) {
  if (!progress) return {};
  auto mutex = std::make_shared<std::mutex>();
  return [progress = std::move(progress), mutex = std::move(mutex)](
             const std::string& stage, const Json::Value& details) {
    std::lock_guard lock(*mutex);
    progress(stage, details);
  };
}

void EnsureHttpInitialized() {
  static const int initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (initialized != CURLE_OK)
    throw Error("libcurl global initialization failed");
}

std::string SanitizedHttpResource(const std::string& url) {
  const auto query = url.find('?');
  std::string result = url.substr(0, query);
  const auto scheme = result.find("://");
  if (scheme != std::string::npos) {
    const auto authority = scheme + 3;
    const auto path = result.find('/', authority);
    const auto userinfo = result.find('@', authority);
    if (userinfo != std::string::npos &&
        (path == std::string::npos || userinfo < path)) {
      result.erase(authority, userinfo - authority + 1);
    }
  }
  return result;
}

HttpGet MakeRetryingHttpGet(HttpGet download, const std::string& provider,
                            ProgressCallback progress, HttpRetryPolicy policy,
                            RetrySleeper sleeper) {
  progress = SynchronizedProgressCallback(std::move(progress));
  if (!download) download = CurlHttpGet;
  if (policy.max_attempts < 1 || policy.initial_delay_ms < 0 ||
      policy.maximum_delay_ms < policy.initial_delay_ms) {
    throw ValidationError("invalid HTTP retry policy");
  }
  if (!sleeper) {
    sleeper = [](int milliseconds) {
      std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    };
  }
  return [download = std::move(download), provider,
          progress = std::move(progress), policy, sleeper = std::move(sleeper)](
             const std::string& url, double timeout_seconds) {
    const std::string resource = SanitizedHttpResource(url);
    int delay_ms = policy.initial_delay_ms;
    for (int attempt = 1;; ++attempt) {
      Json::Value details(Json::objectValue);
      details["provider"] = provider;
      details["resource"] = resource;
      details["attempt"] = attempt;
      details["maxAttempts"] = policy.max_attempts;
      details["state"] = "requesting";
      Progress(progress, "downloading " + provider, details);
      try {
        DownloadPermit permit;
        return download(url, timeout_seconds);
      } catch (const HttpDownloadError& error) {
        if (!error.transient() || attempt >= policy.max_attempts) {
          throw HttpDownloadError(
              provider + " download failed after " + std::to_string(attempt) +
                  (attempt == 1 ? " attempt: " : " attempts: ") + error.what() +
                  " [" + resource + "]",
              error.transient());
        }
        details["state"] = "retrying";
        details["delayMs"] = delay_ms;
        details["error"] = error.what();
        Progress(progress, "retrying " + provider + " download", details);
        sleeper(delay_ms);
        delay_ms = std::min(delay_ms * 2, policy.maximum_delay_ms);
      }
    }
  };
}

HttpGetRange MakeRetryingHttpGetRange(HttpGetRange download,
                                      const std::string& provider,
                                      ProgressCallback progress,
                                      HttpRetryPolicy policy,
                                      RetrySleeper sleeper) {
  progress = SynchronizedProgressCallback(std::move(progress));
  if (!download) download = CurlHttpGetRange;
  if (policy.max_attempts < 1 || policy.initial_delay_ms < 0 ||
      policy.maximum_delay_ms < policy.initial_delay_ms) {
    throw ValidationError("invalid HTTP retry policy");
  }
  if (!sleeper) {
    sleeper = [](int milliseconds) {
      std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    };
  }
  return [download = std::move(download), provider,
          progress = std::move(progress), policy, sleeper = std::move(sleeper)](
             const std::string& url, std::size_t start, std::size_t end,
             double timeout_seconds) {
    const std::string resource = SanitizedHttpResource(url);
    int delay_ms = policy.initial_delay_ms;
    for (int attempt = 1;; ++attempt) {
      Json::Value details(Json::objectValue);
      details["provider"] = provider;
      details["resource"] = resource;
      details["rangeStart"] = Json::UInt64(start);
      details["rangeEnd"] = Json::UInt64(end);
      details["attempt"] = attempt;
      details["maxAttempts"] = policy.max_attempts;
      details["state"] = "requesting";
      Progress(progress, "downloading " + provider, details);
      try {
        DownloadPermit permit;
        return download(url, start, end, timeout_seconds);
      } catch (const HttpDownloadError& error) {
        if (!error.transient() || attempt >= policy.max_attempts) {
          throw HttpDownloadError(
              provider + " range download failed after " +
                  std::to_string(attempt) +
                  (attempt == 1 ? " attempt: " : " attempts: ") + error.what() +
                  " [" + resource + "]",
              error.transient());
        }
        details["state"] = "retrying";
        details["delayMs"] = delay_ms;
        details["error"] = error.what();
        Progress(progress, "retrying " + provider + " download", details);
        sleeper(delay_ms);
        delay_ms = std::min(delay_ms * 2, policy.maximum_delay_ms);
      }
    }
  };
}

std::string GFSCycle::Directory() const {
  return "/gfs." + date + "/" + cycle + "/atmos";
}

std::string GFSCycle::CycleTime() const { return date + "T" + cycle + "00Z"; }

std::vector<WeatherProvider> ListWeatherProviders() {
  return {
      {"gfs", "NOAA GFS 0.25 degree global forecast", "NOAA NOMADS", "GRIB2",
       "free/no account",
       "Global Forecast System subsets from the official NOMADS filter."},
      {"gfs_wave", "NOAA GFS Wave forecast", "NOAA NOMADS", "GRIB2",
       "free/no account", "GFS Wave global 0.25 degree subsets from NOMADS."},
      {"copernicus_global_waves", "Copernicus Marine Global Waves forecast",
       "Copernicus Marine", "NetCDF source, converted to OpenCPN GRIB2",
       "Copernicus Marine account required",
       "Global Ocean Waves Analysis and Forecast product via native ARCO chunk "
       "decoding.",
       true},
      {"noaa_hrrr", "NOAA HRRR 3 km forecast", "NOAA NOMADS", "GRIB2",
       "free/no account",
       "Indexed full-grid GRIB messages; bbox cropping is not yet available."},
      {"ukmo_ukv", "Met Office UKV 2 km forecast", "Met Office AWS/Open Data",
       "NetCDF source, converted to OpenCPN GRIB", "free/no account",
       UkvProjectionAvailable()
           ? "Native projection-aware NetCDF regridding."
           : "Install native PROJ to enable projection-aware regridding.",
       UkvProjectionAvailable()},
      {"metno_nordic", "MET Norway Nordic 1 km forecast", "MET Norway THREDDS",
       "NetCDF source, converted to OpenCPN GRIB", "free/no account",
       "MEPS-derived compact Nordic forecast, regridded from Lambert "
       "conformal conic NetCDF."},
      {"dwd_icon_eu", "DWD ICON-EU 7 km forecast", "DWD Open Data", "GRIB2",
       "free/no account",
       "Full-domain DWD regular-lat/lon field files; bbox cropping is not yet "
       "available."},
      {"ecmwf_ifs_open", "ECMWF IFS Open Data forecast", "ECMWF Open Data",
       "GRIB2", "free/no account",
       "Indexed global Open Data surface and pressure-level fields; bbox "
       "cropping is not yet available."},
      {"ecmwf_aifs_open", "ECMWF AIFS Open Data forecast (experimental)",
       "ECMWF Open Data", "GRIB2", "free/no account",
       "Experimental indexed AIFS Open Data surface fields."},
  };
}

std::vector<int> ForecastHourSequence(int hours, int step_hours) {
  if (hours < 0) throw ValidationError("hours must be zero or greater");
  if (step_hours <= 0)
    throw ValidationError("step-hours must be greater than zero");
  if (hours % step_hours != 0)
    throw ValidationError("hours must be evenly divisible by step-hours");
  std::vector<int> result;
  for (int hour = 0; hour <= hours; hour += step_hours) result.push_back(hour);
  return result;
}

std::map<std::string, std::string> GfsVariablesForPreset(
    const std::string& preset) {
  if (preset == "minimal") return kMinimalFields;
  if (preset == "routing") return kRoutingFields;
  if (preset == "marine") {
    auto result = kRoutingFields;
    result.insert(kMarineFields.begin(), kMarineFields.end());
    return result;
  }
  if (preset == "all") {
    auto result = kRoutingFields;
    result.insert(kAllDisplayableFields.begin(),
                  kAllDisplayableFields.end());
    return result;
  }
  throw ValidationError(
      "weather-preset must be minimal, routing, marine, or all");
}

std::vector<GFSCycle> GfsCycleCandidates(const GFSRequest& request,
                                         std::optional<TimePoint> now) {
  if (request.cycle != "auto") {
    static const std::set<std::string> valid{"00", "03", "06", "09",
                                             "12", "15", "18", "21"};
    if (!valid.contains(request.cycle))
      throw ValidationError("invalid explicit GFS cycle");
    if (!request.date)
      throw ValidationError("date YYYYMMDD is required when cycle is explicit");
    ValidateDate(*request.date);
    return {{*request.date, request.cycle}};
  }
  TimePoint value = now.value_or(std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now()));
  std::time_t raw = static_cast<std::time_t>(value.time_since_epoch().count());
  std::tm tm = UtcTime(raw);
  tm.tm_hour = (tm.tm_hour / 6) * 6;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  TimePoint cursor{std::chrono::seconds{UtcTimeToEpoch(&tm)}};
  std::vector<GFSCycle> result;
  for (int i = 0; i < std::max(1, request.max_auto_cycles); ++i) {
    raw = static_cast<std::time_t>(cursor.time_since_epoch().count());
    tm = UtcTime(raw);
    std::ostringstream date, cycle;
    date << std::put_time(&tm, "%Y%m%d");
    cycle << std::put_time(&tm, "%H");
    result.push_back({date.str(), cycle.str()});
    cursor -= std::chrono::hours{6};
  }
  return result;
}

std::string BuildGfsFilterUrl(
    const GFSCycle& cycle, int forecast_hour, const BoundingBox& bbox,
    const std::map<std::string, std::string>& fields) {
  std::vector<std::pair<std::string, std::string>> query{
      {"dir", cycle.Directory()},
      {"file",
       "gfs.t" + cycle.cycle + "z.pgrb2.0p25.f" + FormatHour(forecast_hour)},
      {"subregion", ""},
      {"leftlon", FormatNumber(bbox.west)},
      {"rightlon", FormatNumber(bbox.east)},
      {"toplat", FormatNumber(bbox.north)},
      {"bottomlat", FormatNumber(bbox.south)}};
  query.insert(query.end(), fields.begin(), fields.end());
  return std::string(kGfsEndpoint) + Query(query);
}

std::string BuildGfsWaveFilterUrl(const GFSCycle& cycle, int forecast_hour,
                                  const BoundingBox& bbox) {
  std::ostringstream hour;
  hour << std::setw(3) << std::setfill('0') << forecast_hour;
  std::vector<std::pair<std::string, std::string>> query{
      {"dir", "/gfs." + cycle.date + "/" + cycle.cycle + "/wave/gridded"},
      {"file",
       "gfswave.t" + cycle.cycle + "z.global.0p25.f" + hour.str() + ".grib2"},
      {"subregion", ""},
      {"leftlon", FormatNumber(bbox.west)},
      {"rightlon", FormatNumber(bbox.east)},
      {"toplat", FormatNumber(bbox.north)},
      {"bottomlat", FormatNumber(bbox.south)}};
  query.insert(query.end(), kWaveFields.begin(), kWaveFields.end());
  return std::string(kGfsWaveEndpoint) + Query(query);
}

std::string BuildGfsWaveFileUrl(const GFSCycle& cycle, int forecast_hour) {
  return std::string(kGfsProductionBase) + "/gfs." + cycle.date + "/" +
         cycle.cycle + "/wave/gridded/gfswave.t" + cycle.cycle +
         "z.global.0p25.f" + FormatHour(forecast_hour) + ".grib2";
}

std::string BuildGfsWaveIndexUrl(const GFSCycle& cycle, int forecast_hour) {
  return BuildGfsWaveFileUrl(cycle, forecast_hour) + ".idx";
}

std::vector<unsigned char> CurlHttpGet(const std::string& url,
                                       double timeout_seconds) {
  CURL* curl = ThreadCurlHandle();
  std::vector<unsigned char> output;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "environmental-grib-generator/0.1");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                   static_cast<long>(timeout_seconds * 1000.0));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(std::min(timeout_seconds, 20.0) * 1000.0));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                   static_cast<curl_off_t>(512ULL * 1024ULL * 1024ULL));
  const CURLcode status = curl_easy_perform(curl);
  long response = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
  if (status != CURLE_OK)
    throw HttpDownloadError(
        "HTTP download failed: " + std::string(curl_easy_strerror(status)),
        TransientCurlError(status));
  if (response < 200 || response >= 300) {
    const std::string body(output.begin(), output.end());
    const bool file_missing =
        body.find("Data file is not present") != std::string::npos;
    const bool rate_limited = body.find("Over Rate Limit") != std::string::npos;
    std::string detail;
    if (file_missing)
      detail = ": requested forecast file is not yet available";
    else if (rate_limited)
      detail = ": NOAA NOMADS rate limit reached";
    throw HttpDownloadError(
        "HTTP download failed with status " + std::to_string(response) + detail,
        rate_limited ||
            (!file_missing && (TransientHttpStatus(response) ||
                               (response >= 300 && response < 400))));
  }
  return output;
}

WeatherGenerateResult GenerateGfs(const GFSRequest& request, HttpGet http_get,
                                  std::optional<TimePoint> now,
                                  ProgressCallback progress,
                                  HttpGetRange http_get_range) {
  progress = SynchronizedProgressCallback(std::move(progress));
  request.bbox.Validate();
  if (!std::set<int>{1, 3, 6, 12}.contains(request.step_hours))
    throw ValidationError("step-hours must be one of 1, 3, 6, or 12 for GFS");
  const auto hours = ForecastHourSequence(request.hours, request.step_hours);
  const auto fields =
      request.waves ? kWaveFields : GfsVariablesForPreset(request.preset);
  const auto cycles = GfsCycleCandidates(request, now);
  auto url_for = [&](const GFSCycle& cycle, int hour) {
    return request.waves ? BuildGfsWaveFilterUrl(cycle, hour, request.bbox)
                         : BuildGfsFilterUrl(cycle, hour, request.bbox, fields);
  };
  if (request.dry_run) {
    WeatherGenerateResult result{
        request.waves ? "gfs_wave" : "gfs",
        request.waves ? "NOAA GFS Wave forecast via NOMADS"
                      : "NOAA GFS 0.25 degree forecast via NOMADS",
        request.waves ? "gfswave_global_0p25" : "gfs_0p25",
        cycles.front(),
        request.bbox,
        hours,
        request.output,
        0,
        0,
        Json::Value(Json::objectValue),
        {},
        fields};
    result.inspection["stream_valid"] = false;
    result.inspection["message_count"] = 0;
    result.inspection["dry_run"] = true;
    for (int hour : hours) result.urls.push_back(url_for(cycles.front(), hour));
    return result;
  }
  if (std::filesystem::exists(request.output) && !request.overwrite)
    throw ValidationError(
        "output already exists; enable overwrite to replace it");
  const bool using_builtin_http = !http_get;
  if (!http_get) http_get = CurlHttpGet;
  if (!http_get_range && using_builtin_http) http_get_range = CurlHttpGetRange;
  std::vector<std::vector<unsigned char>> segments;
  std::vector<std::string> urls, errors;
  std::optional<GFSCycle> selected;
  struct DownloadedHour {
    int hour{};
    std::string url;
    std::vector<unsigned char> bytes;
  };
  auto download_filtered = [&](const GFSCycle& cycle, int hour) {
    const auto url = url_for(cycle, hour);
    Json::Value details;
    details["cycle"] = cycle.CycleTime();
    details["hour"] = hour;
    Progress(progress, "downloading forecast hour", details);
    auto bytes = http_get(url, request.timeout_seconds);
    ValidateDownloaded(bytes, request.waves ? "GFS Wave" : "GFS");
    return DownloadedHour{hour, url, std::move(bytes)};
  };
  auto download_indexed_wave = [&](const GFSCycle& cycle, int hour) {
    const auto file_url = BuildGfsWaveFileUrl(cycle, hour);
    const auto index_url = BuildGfsWaveIndexUrl(cycle, hour);
    Json::Value details;
    details["cycle"] = cycle.CycleTime();
    details["hour"] = hour;
    Progress(progress, "checking NOAA GFS Wave inventory", details);
    const auto index_bytes = http_get(index_url, request.timeout_seconds);
    const auto inventory =
        ParseHrrrInventory(std::string(index_bytes.begin(), index_bytes.end()));
    if (inventory.empty())
      throw ValidationError("GFS Wave inventory was empty or unreadable");
    const std::set<std::string> wanted{"HTSGW", "PERPW", "DIRPW"};
    std::set<std::string> found;
    std::vector<std::size_t> selected_entries;
    std::vector<unsigned char> combined;
    for (std::size_t i = 0; i < inventory.size(); ++i) {
      if (!wanted.contains(inventory[i].short_name)) continue;
      if (i + 1 == inventory.size())
        throw ValidationError(
            "GFS Wave selected final inventory message without following "
            "offset");
      found.insert(inventory[i].short_name);
      selected_entries.push_back(i);
    }
    if (found != wanted)
      throw ValidationError(
          "GFS Wave inventory did not contain all required fields");
    const bool contiguous =
        selected_entries.back() - selected_entries.front() + 1 ==
        selected_entries.size();
    if (contiguous) {
      combined =
          http_get_range(file_url, inventory[selected_entries.front()].offset,
                         inventory[selected_entries.back() + 1].offset - 1,
                         request.timeout_seconds);
      ValidateDownloaded(combined, "GFS Wave indexed fields");
    } else {
      for (const std::size_t i : selected_entries) {
        auto bytes = http_get_range(file_url, inventory[i].offset,
                                    inventory[i + 1].offset - 1,
                                    request.timeout_seconds);
        ValidateDownloaded(bytes, "GFS Wave indexed field");
        combined.insert(combined.end(), bytes.begin(), bytes.end());
      }
    }
    details["fields"] = static_cast<int>(found.size());
    Progress(progress, "downloaded NOAA GFS Wave indexed fields", details);
    return DownloadedHour{hour, file_url, std::move(combined)};
  };
  auto download_cycle = [&](const GFSCycle& cycle, const auto& download) {
    // Probe the furthest required forecast hour before starting concurrent
    // transfers.  A publishing cycle which cannot cover the requested window
    // is rejected with one request instead of a burst for every early hour.
    std::vector<DownloadedHour> downloaded;
    downloaded.push_back(download(cycle, hours.back()));
    if (hours.size() > 1) {
      std::vector<int> remaining(hours.begin(), hours.end() - 1);
      auto rest = ParallelMapOrdered(
          remaining, kDefaultDownloadConcurrency,
          [&](const int& hour) { return download(cycle, hour); });
      downloaded.insert(downloaded.end(), std::make_move_iterator(rest.begin()),
                        std::make_move_iterator(rest.end()));
    }
    std::sort(downloaded.begin(), downloaded.end(),
              [](const auto& left, const auto& right) {
                return left.hour < right.hour;
              });
    return downloaded;
  };
  for (const auto& cycle : cycles) {
    try {
      segments.clear();
      urls.clear();
      std::vector<DownloadedHour> downloaded;
      try {
        downloaded = download_cycle(cycle, download_filtered);
      } catch (const ValidationError& filter_error) {
        if (!request.waves || !http_get_range) throw;
        Json::Value details;
        details["cycle"] = cycle.CycleTime();
        details["reason"] = filter_error.what();
        details["scope"] = "global selected fields";
        Progress(progress, "NOAA wave subset unavailable; using indexed data",
                 details);
        try {
          downloaded = download_cycle(cycle, download_indexed_wave);
        } catch (const ValidationError& indexed_error) {
          throw ValidationError(
              std::string("NOAA wave subset failed: ") + filter_error.what() +
              "; indexed fallback failed: " + indexed_error.what());
        }
      }
      for (auto& item : downloaded) {
        urls.push_back(std::move(item.url));
        segments.push_back(std::move(item.bytes));
      }
      selected = cycle;
      break;
    } catch (const ValidationError& error) {
      errors.push_back(cycle.CycleTime() + ": " + error.what());
      if (request.cycle != "auto") throw;
    }
  }
  if (!selected) {
    std::ostringstream message;
    message << "No complete GFS cycle was available. Tried: ";
    for (const auto& error : errors) message << error << "; ";
    throw ValidationError(message.str());
  }
  std::filesystem::create_directories(request.output.parent_path().empty()
                                          ? "."
                                          : request.output.parent_path());
  const auto temporary = TemporarySibling(request.output);
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    for (const auto& segment : segments)
      output.write(reinterpret_cast<const char*>(segment.data()),
                   static_cast<std::streamsize>(segment.size()));
    if (!output) throw ValidationError("writing downloaded GFS output failed");
    output.close();
    const auto scan = ScanGribMessages(temporary);
    const auto inspection = InspectGrib(temporary);
    std::filesystem::rename(temporary, request.output);
    return {request.waves ? "gfs_wave" : "gfs",
            request.waves ? "NOAA GFS Wave forecast via NOMADS"
                          : "NOAA GFS 0.25 degree forecast via NOMADS",
            request.waves ? "gfswave_global_0p25" : "gfs_0p25",
            *selected,
            request.bbox,
            hours,
            request.output,
            scan.byte_count,
            scan.message_count,
            inspection,
            urls,
            fields};
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

std::vector<int> DwdIconEuForecastHourSequence(int hours, int step_hours) {
  if (hours > 120)
    throw ValidationError(
        "DWD ICON-EU forecasts are supported only to 120 hours");
  if (step_hours != 1 && step_hours != 3)
    throw ValidationError("step-hours must be 1 or 3 for DWD ICON-EU");
  return ForecastHourSequence(hours, step_hours);
}

std::map<std::string, std::string> DwdIconEuFieldsForPreset(
    const std::string& preset) {
  if (preset == "minimal") return kDwdMinimalFields;
  if (preset == "routing") return kDwdRoutingFields;
  if (preset == "marine") return kDwdMarineFields;
  if (preset == "all") return kDwdAllDisplayableFields;
  throw ValidationError(
      "weather-preset must be minimal, routing, marine, or all");
}

std::vector<GFSCycle> DwdIconEuCycleCandidates(const GFSRequest& request,
                                               std::optional<TimePoint> now) {
  if (request.cycle != "auto") {
    static const std::set<std::string> valid{"00", "03", "06", "09",
                                             "12", "15", "18", "21"};
    if (!valid.contains(request.cycle) || !request.date)
      throw ValidationError(
          "explicit ICON-EU cycle requires valid cycle and date");
    ValidateDate(*request.date);
    return {{*request.date, request.cycle}};
  }
  TimePoint value = now.value_or(std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now()));
  std::time_t raw = static_cast<std::time_t>(value.time_since_epoch().count());
  std::tm tm = UtcTime(raw);
  tm.tm_hour = (tm.tm_hour / 3) * 3;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  TimePoint cursor{std::chrono::seconds{UtcTimeToEpoch(&tm)}};
  std::vector<GFSCycle> result;
  for (int i = 0; i < std::max(1, request.max_auto_cycles); ++i) {
    raw = static_cast<std::time_t>(cursor.time_since_epoch().count());
    tm = UtcTime(raw);
    std::ostringstream date, cycle;
    date << std::put_time(&tm, "%Y%m%d");
    cycle << std::put_time(&tm, "%H");
    result.push_back({date.str(), cycle.str()});
    cursor -= std::chrono::hours{3};
  }
  return result;
}

std::string BuildDwdIconEuUrl(const GFSCycle& cycle, int forecast_hour,
                              const std::string& field) {
  std::string upper = field;
  std::transform(
      upper.begin(), upper.end(), upper.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return std::string(kDwdIconEuBase) + "/" + cycle.cycle + "/" + field +
         "/icon-eu_europe_regular-lat-lon_single-level_" + cycle.date +
         cycle.cycle + "_" + FormatHour(forecast_hour) + "_" + upper +
         ".grib2.bz2";
}

WeatherGenerateResult GenerateDwdIconEu(const GFSRequest& request,
                                        HttpGet http_get,
                                        std::optional<TimePoint> now,
                                        ProgressCallback progress) {
  progress = SynchronizedProgressCallback(std::move(progress));
  request.bbox.Validate();
  const BoundingBox domain{-23.5, 29.5, 62.5, 70.5};
  if (!domain.Contains(request.bbox))
    throw ValidationError(
        "DWD ICON-EU bbox is outside the supported Europe regional domain");
  const auto hours =
      DwdIconEuForecastHourSequence(request.hours, request.step_hours);
  const auto fields = DwdIconEuFieldsForPreset(request.preset);
  const auto cycles = DwdIconEuCycleCandidates(request, now);
  if (request.dry_run) {
    WeatherGenerateResult result{"dwd_icon_eu",
                                 "DWD ICON-EU 7 km forecast via Open Data",
                                 "icon_eu_regular_lat_lon_7km",
                                 cycles.front(),
                                 request.bbox,
                                 hours,
                                 request.output,
                                 0,
                                 0,
                                 Json::Value(Json::objectValue),
                                 {},
                                 fields};
    result.inspection["dry_run"] = true;
    result.inspection["stream_valid"] = false;
    for (int hour : hours)
      for (const auto& [short_name, field] : fields) {
        (void)short_name;
        result.urls.push_back(BuildDwdIconEuUrl(cycles.front(), hour, field));
      }
    return result;
  }
  if (!http_get) http_get = CurlHttpGet;
  if (std::filesystem::exists(request.output) && !request.overwrite)
    throw ValidationError("output already exists; enable overwrite");
  std::vector<std::vector<unsigned char>> segments;
  std::vector<std::string> urls;
  std::optional<GFSCycle> selected;
  for (const auto& cycle : cycles) {
    try {
      segments.clear();
      urls.clear();
      struct FieldRequest {
        int hour{};
        std::string field;
      };
      struct DownloadedField {
        std::string url;
        std::vector<unsigned char> data;
      };
      std::vector<FieldRequest> requests;
      for (int hour : hours)
        for (const auto& [short_name, field] : fields) {
          (void)short_name;
          requests.push_back({hour, field});
        }
      auto downloaded = ParallelMapOrdered(
          requests, kDefaultDownloadConcurrency,
          [&](const FieldRequest& request_field) {
            const int hour = request_field.hour;
            const auto& field = request_field.field;
            const auto url = BuildDwdIconEuUrl(cycle, hour, field);
            Json::Value detail;
            detail["cycle"] = cycle.CycleTime();
            detail["hour"] = hour;
            detail["field"] = field;
            Progress(progress, "downloading DWD ICON-EU field", detail);
            auto compressed = http_get(url, request.timeout_seconds);
            auto data = DecompressBzip2(compressed);
            ValidateDownloaded(data, "DWD ICON-EU");
            return DownloadedField{url, std::move(data)};
          });
      for (auto& item : downloaded) {
        urls.push_back(std::move(item.url));
        segments.push_back(std::move(item.data));
      }
      selected = cycle;
      break;
    } catch (const ValidationError&) {
      if (request.cycle != "auto") throw;
    }
  }
  if (!selected)
    throw ValidationError("No complete DWD ICON-EU cycle was available");
  std::filesystem::create_directories(request.output.parent_path().empty()
                                          ? "."
                                          : request.output.parent_path());
  const auto temporary = TemporarySibling(request.output);
  auto simple = temporary;
  simple += ".simple.grib2";
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    for (const auto& segment : segments)
      output.write(reinterpret_cast<const char*>(segment.data()),
                   static_cast<std::streamsize>(segment.size()));
    output.close();
    RepackGrib2ToSimplePacking(temporary, simple);
    const auto scan = ScanGribMessages(simple);
    const auto inspection = InspectGrib(simple);
    std::filesystem::remove(temporary);
    std::filesystem::rename(simple, request.output);
    return {"dwd_icon_eu",
            "DWD ICON-EU 7 km forecast via Open Data",
            "icon_eu_regular_lat_lon_7km",
            *selected,
            request.bbox,
            hours,
            request.output,
            scan.byte_count,
            scan.message_count,
            inspection,
            urls,
            fields};
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    std::filesystem::remove(simple, ignored);
    throw;
  }
}

std::vector<HrrrInventoryEntry> ParseHrrrInventory(const std::string& text) {
  std::vector<HrrrInventoryEntry> result;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const auto parts = Split(line, ':');
    if (parts.size() < 6) continue;
    try {
      std::size_t used_number = 0, used_offset = 0;
      const int number = std::stoi(parts[0], &used_number);
      const auto offset = std::stoull(parts[1], &used_offset);
      if (used_number != parts[0].size() || used_offset != parts[1].size())
        continue;
      result.push_back({number, static_cast<std::size_t>(offset), parts[3],
                        parts[4], parts[5]});
    } catch (...) {
    }
  }
  return result;
}

std::vector<int> HrrrForecastHourSequence(int hours, int step_hours) {
  if (hours > 48)
    throw ValidationError("HRRR forecasts are supported only to 48 hours");
  if (step_hours != 1) throw ValidationError("step-hours must be 1 for HRRR");
  return ForecastHourSequence(hours, step_hours);
}

std::map<std::string, std::string> HrrrVariablesForPreset(
    const std::string& preset) {
  if (preset == "minimal") return kHrrrMinimalFields;
  if (preset == "routing") return kHrrrRoutingFields;
  if (preset == "marine") {
    auto result = kHrrrRoutingFields;
    result.insert(kHrrrMarineFields.begin(), kHrrrMarineFields.end());
    return result;
  }
  if (preset == "all") return kHrrrAllDisplayableFields;
  throw ValidationError(
      "weather-preset must be minimal, routing, marine, or all");
}

std::set<std::pair<std::string, std::string>> HrrrFieldPairsForPreset(
    const std::string& preset) {
  std::set<std::pair<std::string, std::string>> result{
      {"UGRD", "10 m above ground"}, {"VGRD", "10 m above ground"}};
  if (preset == "minimal") return result;
  result.insert({"TMP", "2 m above ground"});
  result.insert({"PRES", "surface"});
  if (preset == "routing") return result;
  result.insert({"GUST", "surface"});
  if (preset == "marine") {
    result.insert({"APCP", "surface"});
    result.insert({"TCDC", "entire atmosphere"});
    return result;
  }
  if (preset != "all")
    throw ValidationError(
        "weather-preset must be minimal, routing, marine, or all");
  result.insert({"APCP", "surface"});
  result.insert({"TCDC", "entire atmosphere"});
  result.insert({"CAPE", "surface"});
  result.insert({"REFC", "entire atmosphere"});
  result.insert({"RH", "2 m above ground"});
  for (const char* level : {"850 mb", "700 mb", "500 mb", "300 mb"}) {
    result.insert({"UGRD", level});
    result.insert({"VGRD", level});
  }
  // HRRR's public wrfsfc file carries temperature and geopotential height at
  // 850/700/500 hPa, but not at 300 hPa, and carries dew point rather than
  // pressure-level relative humidity. Request only directly displayable
  // fields actually present in this product.
  for (const char* level : {"850 mb", "700 mb", "500 mb"}) {
    result.insert({"TMP", level});
    result.insert({"HGT", level});
  }
  return result;
}

std::string BuildHrrrFileUrl(const GFSCycle& cycle, int forecast_hour) {
  return std::string(kHrrrBase) + "/hrrr." + cycle.date + "/conus/hrrr.t" +
         cycle.cycle + "z.wrfsfcf" + FormatHour(forecast_hour, 2) + ".grib2";
}

std::string BuildHrrrIndexUrl(const GFSCycle& cycle, int forecast_hour) {
  return BuildHrrrFileUrl(cycle, forecast_hour) + ".idx";
}

std::vector<unsigned char> CurlHttpGetRange(const std::string& url,
                                            std::size_t start, std::size_t end,
                                            double timeout_seconds) {
  if (end < start) throw ValidationError("invalid HTTP byte range");
  CURL* curl = ThreadCurlHandle();
  std::vector<unsigned char> output;
  const std::string range = std::to_string(start) + "-" + std::to_string(end);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "environmental-grib-generator/0.1");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                   static_cast<long>(timeout_seconds * 1000.0));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
  const CURLcode status = curl_easy_perform(curl);
  long response = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
  if (status != CURLE_OK)
    throw HttpDownloadError("HTTP range download failed: " +
                                std::string(curl_easy_strerror(status)),
                            TransientCurlError(status));
  if (response < 200 || response >= 300)
    throw HttpDownloadError(
        "HTTP range download failed with status " + std::to_string(response),
        TransientHttpStatus(response));
  const std::size_t expected = end - start + 1;
  if (output.size() != expected ||
      (response != 206 && output.size() != expected))
    throw HttpDownloadError("short or ignored HTTP byte range response", true);
  return output;
}

WeatherGenerateResult GenerateHrrr(const GFSRequest& request, HttpGet http_get,
                                   HttpGetRange http_get_range,
                                   std::optional<TimePoint> now,
                                   ProgressCallback progress) {
  progress = SynchronizedProgressCallback(std::move(progress));
  request.bbox.Validate();
  if (!BoundingBox{-130.0, 20.0, -60.0, 55.0}.Contains(request.bbox))
    throw ValidationError(
        "HRRR bbox is outside the supported contiguous United States domain");
  const auto hours =
      HrrrForecastHourSequence(request.hours, request.step_hours);
  const auto fields = HrrrVariablesForPreset(request.preset);
  std::vector<GFSCycle> cycles;
  if (request.cycle != "auto") {
    if (!request.date || request.cycle.size() != 2 ||
        std::stoi(request.cycle) > 23)
      throw ValidationError("explicit HRRR cycle requires date and hour 00-23");
    ValidateDate(*request.date);
    cycles.push_back({*request.date, request.cycle});
  } else {
    TimePoint value = now.value_or(std::chrono::floor<std::chrono::seconds>(
                          std::chrono::system_clock::now())) -
                      std::chrono::hours{1};
    for (int i = 0; i < std::max(1, request.max_auto_cycles); ++i) {
      const std::time_t raw =
          static_cast<std::time_t>(value.time_since_epoch().count());
      const std::tm tm = UtcTime(raw);
      std::ostringstream date, cycle;
      date << std::put_time(&tm, "%Y%m%d");
      cycle << std::put_time(&tm, "%H");
      cycles.push_back({date.str(), cycle.str()});
      value -= std::chrono::hours{1};
    }
  }
  if (request.dry_run) {
    WeatherGenerateResult result{"noaa_hrrr",
                                 "NOAA HRRR 3 km forecast via NOMADS",
                                 "hrrr_conus_3km",
                                 cycles.front(),
                                 request.bbox,
                                 hours,
                                 request.output,
                                 0,
                                 0,
                                 Json::Value(Json::objectValue),
                                 {},
                                 fields};
    result.inspection["dry_run"] = true;
    for (int hour : hours)
      result.urls.push_back(BuildHrrrIndexUrl(cycles.front(), hour));
    return result;
  }
  if (!http_get) http_get = CurlHttpGet;
  if (!http_get_range) http_get_range = CurlHttpGetRange;
  std::optional<GFSCycle> selected;
  std::vector<std::vector<unsigned char>> hour_segments;
  std::vector<std::string> urls;
  const auto requested = HrrrFieldPairsForPreset(request.preset);
  for (const auto& cycle : cycles) {
    try {
      hour_segments.clear();
      urls.clear();
      struct DownloadedHour {
        std::string url;
        std::vector<unsigned char> bytes;
      };
      auto downloaded = ParallelMapOrdered(
          hours, kDefaultDownloadConcurrency, [&](const int& hour) {
            const auto index_url = BuildHrrrIndexUrl(cycle, hour);
            const auto file_url = BuildHrrrFileUrl(cycle, hour);
            Json::Value detail;
            detail["cycle"] = cycle.CycleTime();
            detail["hour"] = hour;
            Progress(progress, "checking HRRR inventory", detail);
            const auto index_bytes =
                http_get(index_url, request.timeout_seconds);
            const std::string text(index_bytes.begin(), index_bytes.end());
            const auto inventory = ParseHrrrInventory(text);
            if (inventory.empty())
              throw ValidationError("HRRR inventory was empty or unreadable");
            std::vector<unsigned char> combined;
            std::set<std::pair<std::string, std::string>> found;
            for (std::size_t i = 0; i < inventory.size(); ++i) {
              const auto key =
                  std::make_pair(inventory[i].short_name, inventory[i].level);
              if (!requested.contains(key)) continue;
              if (i + 1 == inventory.size())
                throw ValidationError(
                    "HRRR selected final inventory message without following "
                    "offset");
              auto bytes = http_get_range(file_url, inventory[i].offset,
                                          inventory[i + 1].offset - 1,
                                          request.timeout_seconds);
              ValidateDownloaded(bytes, "HRRR");
              detail["field"] = inventory[i].short_name;
              detail["level"] = inventory[i].level;
              Progress(progress, "downloaded HRRR indexed field", detail);
              combined.insert(combined.end(), bytes.begin(), bytes.end());
              found.insert(key);
            }
            if (found != requested)
              throw ValidationError(
                  "HRRR inventory did not contain all required fields");
            return DownloadedHour{file_url, std::move(combined)};
          });
      for (auto& item : downloaded) {
        urls.push_back(std::move(item.url));
        hour_segments.push_back(std::move(item.bytes));
      }
      selected = cycle;
      break;
    } catch (const ValidationError&) {
      if (request.cycle != "auto") throw;
    }
  }
  if (!selected) throw ValidationError("No complete HRRR cycle was available");
  std::filesystem::create_directories(request.output.parent_path().empty()
                                          ? "."
                                          : request.output.parent_path());
  const auto temporary = TemporarySibling(request.output);
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    for (const auto& segment : hour_segments)
      output.write(reinterpret_cast<const char*>(segment.data()),
                   static_cast<std::streamsize>(segment.size()));
    output.close();
    const auto scan = ScanGribMessages(temporary);
    const auto inspection = InspectGrib(temporary);
    std::filesystem::rename(temporary, request.output);
    return {"noaa_hrrr",
            "NOAA HRRR 3 km forecast via NOMADS",
            "hrrr_conus_3km",
            *selected,
            request.bbox,
            hours,
            request.output,
            scan.byte_count,
            scan.message_count,
            inspection,
            urls,
            fields};
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

std::string BuildEcmwfDataUrl(const GFSCycle& cycle, int forecast_hour,
                              bool aifs) {
  // AIFS v2 moved from the legacy "aifs" path to "aifs-single" in 2026.
  const std::string model = aifs ? "aifs-single" : "ifs";
  return "https://data.ecmwf.int/forecasts/" + cycle.date + "/" + cycle.cycle +
         "z/" + model + "/0p25/oper/" + cycle.date + cycle.cycle + "0000-" +
         std::to_string(forecast_hour) + "h-oper-fc.grib2";
}

std::string BuildEcmwfIndexUrl(const GFSCycle& cycle, int forecast_hour,
                               bool aifs) {
  auto url = BuildEcmwfDataUrl(cycle, forecast_hour, aifs);
  url.resize(url.size() - std::string("grib2").size());
  return url + "index";
}

struct EcmwfFieldSelection {
  std::string key;
  std::string parameter;
  std::string level_type;
  std::optional<std::string> level;
};

std::vector<EcmwfFieldSelection> EcmwfFieldsForPreset(
    const std::string& preset, bool aifs) {
  std::vector<EcmwfFieldSelection> fields{
      {"10u", "10u", "sfc", std::nullopt},
      {"10v", "10v", "sfc", std::nullopt}};
  if (preset == "minimal") return fields;
  fields.push_back({"msl", "msl", "sfc", std::nullopt});
  fields.push_back({"2t", "2t", "sfc", std::nullopt});
  if (preset == "routing") return fields;
  fields.push_back({"tp", "tp", "sfc", std::nullopt});
  fields.push_back({"tcc", "tcc", "sfc", std::nullopt});
  if (!aifs) fields.push_back({"10fg", "10fg", "sfc", std::nullopt});
  if (preset == "marine") return fields;
  if (preset != "all")
    throw ValidationError(
        "weather-preset must be minimal, routing, marine, or all");
  if (!aifs)
    fields.push_back({"mucape", "mucape", "sfc", std::nullopt});
  for (const char* level : {"850", "700", "500", "300"}) {
    fields.push_back(
        {std::string("u:") + level, "u", "pl", std::string(level)});
    fields.push_back(
        {std::string("v:") + level, "v", "pl", std::string(level)});
    fields.push_back(
        {std::string("t:") + level, "t", "pl", std::string(level)});
    fields.push_back({std::string(aifs ? "z:" : "gh:") + level,
                      aifs ? "z" : "gh", "pl", std::string(level)});
    if (!aifs)
      fields.push_back(
          {std::string("r:") + level, "r", "pl", std::string(level)});
  }
  return fields;
}

WeatherGenerateResult GenerateEcmwfOpenData(const GFSRequest& request,
                                            bool aifs, HttpGet http_get,
                                            HttpGetRange http_get_range,
                                            std::optional<TimePoint> now,
                                            ProgressCallback progress) {
  progress = SynchronizedProgressCallback(std::move(progress));
  request.bbox.Validate();
  if (aifs && request.step_hours != 6 && request.step_hours != 12)
    throw ValidationError(
        "step-hours must be 6 or 12 for ECMWF AIFS Open Data");
  const auto hours = ForecastHourSequence(request.hours, request.step_hours);
  const auto cycles = SixHourlyCycles(request, now);
  const auto selections = EcmwfFieldsForPreset(request.preset, aifs);
  std::map<std::string, std::string> fields;
  std::set<std::string> wanted;
  for (const auto& selection : selections) {
    fields[selection.key] = "on";
    wanted.insert(selection.key);
  }
  if (request.dry_run) {
    WeatherGenerateResult result{
        aifs ? "ecmwf_aifs_open" : "ecmwf_ifs_open",
        aifs ? "ECMWF AIFS Open Data forecast" : "ECMWF IFS Open Data forecast",
        aifs ? "ecmwf_aifs_open" : "ecmwf_ifs_open_0p25",
        cycles.front(),
        request.bbox,
        hours,
        request.output,
        0,
        0,
        Json::Value(Json::objectValue),
        {},
        fields};
    result.inspection["dry_run"] = true;
    for (int hour : hours)
      result.urls.push_back(BuildEcmwfDataUrl(cycles.front(), hour, aifs));
    return result;
  }
  if (!http_get) http_get = CurlHttpGet;
  if (!http_get_range) http_get_range = CurlHttpGetRange;
  if (std::filesystem::exists(request.output) && !request.overwrite)
    throw ValidationError("output already exists; enable overwrite");
  std::optional<GFSCycle> selected;
  std::vector<std::vector<unsigned char>> segments;
  std::vector<std::string> urls;
  for (const auto& cycle : cycles) {
    try {
      segments.clear();
      urls.clear();
      struct DownloadedHour {
        std::string url;
        std::vector<unsigned char> bytes;
      };
      auto downloaded = ParallelMapOrdered(
          hours, kDefaultDownloadConcurrency, [&](const int& hour) {
            const auto data_url = BuildEcmwfDataUrl(cycle, hour, aifs);
            const auto index_bytes = http_get(
                BuildEcmwfIndexUrl(cycle, hour, aifs), request.timeout_seconds);
            std::istringstream lines(
                std::string(index_bytes.begin(), index_bytes.end()));
            std::string line;
            std::set<std::string> found;
            std::vector<unsigned char> combined;
            while (std::getline(lines, line)) {
              Json::CharReaderBuilder builder;
              Json::Value entry;
              std::string errors;
              std::istringstream json(line);
              if (!Json::parseFromStream(builder, json, &entry, &errors))
                continue;
              const std::string parameter = entry.get("param", "").asString();
              const std::string type = entry.get("type", "").asString();
              const std::string level_type =
                  entry.get("levtype", "").asString();
              const auto& level_value = entry["levelist"];
              const std::string level =
                  level_value.isString()
                      ? level_value.asString()
                      : (level_value.isNumeric()
                             ? std::to_string(level_value.asInt())
                             : std::string{});
              const auto& step_value = entry["step"];
              const std::string step = step_value.isString()
                                           ? step_value.asString()
                                           : std::to_string(step_value.asInt());
              const auto selected = std::find_if(
                  selections.begin(), selections.end(),
                  [&](const EcmwfFieldSelection& candidate) {
                    const bool parameter_matches =
                        parameter == candidate.parameter ||
                        (candidate.parameter == "10fg" &&
                         parameter.starts_with("10fg"));
                    return parameter_matches && type == "fc" &&
                           level_type == candidate.level_type &&
                           (!candidate.level || *candidate.level == level) &&
                           step == std::to_string(hour);
                  });
              if (selected == selections.end()) continue;
              const auto offset = entry["_offset"].asUInt64();
              const auto length = entry["_length"].asUInt64();
              if (length == 0)
                throw ValidationError(
                    "ECMWF index contains zero-length selected field");
              auto bytes = http_get_range(data_url, offset, offset + length - 1,
                                          request.timeout_seconds);
              ValidateDownloaded(bytes, aifs ? "ECMWF AIFS" : "ECMWF IFS");
              combined.insert(combined.end(), bytes.begin(), bytes.end());
              found.insert(selected->key);
              Json::Value detail;
              detail["cycle"] = cycle.CycleTime();
              detail["hour"] = hour;
              detail["field"] = selected->key;
              Progress(progress, "downloaded ECMWF indexed field", detail);
            }
            if (found != wanted)
              throw ValidationError(
                  "ECMWF index did not contain all requested fields");
            return DownloadedHour{data_url, std::move(combined)};
          });
      for (auto& item : downloaded) {
        urls.push_back(std::move(item.url));
        segments.push_back(std::move(item.bytes));
      }
      selected = cycle;
      break;
    } catch (const ValidationError&) {
      if (request.cycle != "auto") throw;
    }
  }
  if (!selected)
    throw ValidationError("No complete ECMWF Open Data cycle was available");
  std::filesystem::create_directories(request.output.parent_path().empty()
                                          ? "."
                                          : request.output.parent_path());
  const auto temporary = TemporarySibling(request.output);
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    for (const auto& segment : segments)
      output.write(reinterpret_cast<const char*>(segment.data()),
                   static_cast<std::streamsize>(segment.size()));
    output.close();
    auto repacked = temporary;
    repacked += ".simple.grib2";
    RepackGrib2ToSimplePacking(temporary, repacked);
    const auto scan = ScanGribMessages(repacked);
    const auto inspection = InspectGrib(repacked);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    std::filesystem::rename(repacked, request.output);
    return {
        aifs ? "ecmwf_aifs_open" : "ecmwf_ifs_open",
        aifs ? "ECMWF AIFS Open Data forecast" : "ECMWF IFS Open Data forecast",
        aifs ? "ecmwf_aifs_open" : "ecmwf_ifs_open_0p25",
        *selected,
        request.bbox,
        hours,
        request.output,
        scan.byte_count,
        scan.message_count,
        inspection,
        urls,
        fields};
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    auto repacked = temporary;
    repacked += ".simple.grib2";
    std::filesystem::remove(repacked, ignored);
    throw;
  }
}

Json::Value WeatherResultJson(const WeatherGenerateResult& result) {
  Json::Value value(Json::objectValue);
  value["provider"] = result.provider;
  value["source"] = result.source;
  value["model"] = result.model;
  value["cycle"] = result.cycle.CycleTime();
  value["bbox"] = BboxJson(result.bbox);
  value["output"] = PathToUtf8(result.output);
  value["byte_count"] = Json::UInt64(result.byte_count);
  value["message_count"] = Json::UInt64(result.message_count);
  for (int hour : result.forecast_hours) value["forecast_hours"].append(hour);
  for (const auto& url : result.urls) value["urls"].append(url);
  for (const auto& [key, entry] : result.variables_levels)
    value["variables_levels"][key] = entry;
  value["inspection"] = result.inspection;
  return value;
}

}  // namespace environmental_grib
