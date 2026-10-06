#include "environmental_grib/gfs_fallback.h"
#include "environmental_grib/cancellation.h"
#include "environmental_grib/grib.h"

#include <curl/curl.h>
#include <eccodes.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace environmental_grib {
namespace {
constexpr std::size_t kMaximumResult = 64ULL * 1024 * 1024;

std::set<std::string> Fields(const GFSRequest& request) {
  if (request.waves) return {"swh", "perpw", "dirpw"};
  if (request.preset == "minimal") return {"10u", "10v"};
  if (request.preset == "routing") return {"10u", "10v", "prmsl", "2t"};
  throw HostedGfsServiceError(
      "The hosted GFS cache supports minimal/routing weather "
      "and waves; the selected weather preset requires "
      "additional fields. No fields were removed.");
}

Json::Value ParseJson(const std::vector<unsigned char>& body) {
  Json::CharReaderBuilder builder;
  builder["rejectDupKeys"] = true;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  std::string errors;
  const std::string text(body.begin(), body.end());
  if (body.size() > 1024 * 1024 ||
      !reader->parse(text.data(), text.data() + text.size(), &value, &errors) ||
      !value.isObject())
    throw HostedGfsServiceError("Hosted GFS service returned invalid JSON");
  return value;
}

std::string Hex(const unsigned char* data, std::size_t size) {
  std::string value(size * 2 + 1, '\0');
  sodium_bin2hex(value.data(), value.size(), data, size);
  value.resize(size * 2);
  return value;
}

HostedHttpReply CurlRequest(const std::string& url, const std::string& method,
                            const std::string& body, const std::string& client,
                            double timeout) {
  EnsureHttpInitialized();
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                           &curl_easy_cleanup);
  if (!curl) throw Error("Hosted GFS HTTP initialization failed");
  HostedHttpReply reply;
  curl_slist* list = nullptr;
  list = curl_slist_append(list, "Content-Type: application/json");
  list = curl_slist_append(list, ("X-xGRIB-Client-ID: " + client).c_str());
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
      list, &curl_slist_free_all);
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_USERAGENT,
                   "environmental-grib-generator/0.3.1");
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
  // All returned paths are validated below. Do not follow redirects to another
  // origin, forward client identifiers, or relax certificate validation.
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS,
                   static_cast<long>(timeout * 1000));
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(std::min(timeout, 20.0) * 1000));
  curl_easy_setopt(curl.get(), CURLOPT_MAXFILESIZE_LARGE,
                   static_cast<curl_off_t>(kMaximumResult));
  if (method == "POST") {
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body.size()));
  }
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &reply.body);
  curl_easy_setopt(
      curl.get(), CURLOPT_WRITEFUNCTION,
      +[](char* data, std::size_t size, std::size_t count,
          void* target) -> std::size_t {
        auto& bytes = *static_cast<std::vector<unsigned char>*>(target);
        if (size && count > kMaximumResult / size) return 0;
        const auto length = size * count;
        if (length > kMaximumResult - bytes.size()) return 0;
        bytes.insert(bytes.end(), data, data + length);
        return length;
      });
  curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &reply.retry_after_seconds);
  curl_easy_setopt(
      curl.get(), CURLOPT_HEADERFUNCTION,
      +[](char* data, std::size_t size, std::size_t count,
          void* target) -> std::size_t {
        std::string header(data, size * count);
        std::string lower = header;
        std::transform(
            lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.starts_with("retry-after:")) {
          const auto value = header.substr(12);
          try {
            const auto seconds = std::stoll(value);
            *static_cast<int*>(target) =
                static_cast<int>(std::clamp(seconds, 0LL, 86400LL));
          } catch (...) {
            const auto date = curl_getdate(value.c_str(), nullptr);
            if (date >= 0)
              *static_cast<int*>(target) = static_cast<int>(std::clamp<double>(
                  std::difftime(date, std::time(nullptr)), 0, 86400));
          }
        }
        return size * count;
      });
  ConfigureJobCurl(curl.get());
  const auto status = curl_easy_perform(curl.get());
  CheckCancellation();
  if (status != CURLE_OK)
    throw HostedGfsServiceError("Hosted GFS connection failed: " +
                                std::string(curl_easy_strerror(status)));
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &reply.status);
  return reply;
}

long Long(codes_handle* handle, const char* key) {
  long value = 0;
  if (codes_get_long(handle, key, &value))
    throw HostedGfsServiceError(
        "Hosted GFS result has invalid forecast metadata");
  return value;
}
std::string String(codes_handle* handle, const char* key) {
  std::array<char, 128> value{};
  auto size = value.size();
  if (codes_get_string(handle, key, value.data(), &size))
    throw HostedGfsServiceError("Hosted GFS result has invalid field metadata");
  return value.data();
}
double Number(codes_handle* handle, const char* key) {
  double value = 0;
  if (codes_get_double(handle, key, &value) || !std::isfinite(value))
    throw HostedGfsServiceError("Hosted GFS result has invalid grid metadata");
  return value;
}
}  // namespace

bool IsGfsFailoverError(const HttpDownloadError& error) {
  if (error.forecast_missing()) return false;
  const auto status = error.status();
  return error.transient() || status < 0 || status == 403 || status == 408 ||
         status == 425 || status == 429 || (status >= 500 && status <= 599) ||
         (status >= 300 && status < 400);
}

HostedGfsDownload BuiltinHostedGfsDownload() {
  return [](const GFSRequest& request, const GFSCycle& cycle,
            const std::vector<int>& hours, ProgressCallback progress) {
    return DownloadHostedGfs(request, cycle, hours, std::move(progress));
  };
}

std::map<int, std::vector<unsigned char>> ValidateHostedGfsBytes(
    const std::vector<unsigned char>& bytes, const GFSRequest& request,
    const GFSCycle& cycle, const std::vector<int>& hours) {
  if (bytes.empty() || bytes.size() > kMaximumResult)
    throw HostedGfsServiceError("Hosted GFS result has invalid size");
  ScanGribBytes(bytes);
  const auto fields = Fields(request);
  const double west = std::ceil(request.bbox.west * 4 - 1e-8) / 4;
  const double east = request.bbox.UnwrappedEast();
  const long columns = std::min(
      1440L,
      static_cast<long>((request.waves ? std::ceil(east * 4 - 1e-8)
                                       : std::floor(east * 4 + 1e-8) + 1) -
                        std::ceil(request.bbox.west * 4 - 1e-8)));
  const double south = std::ceil(request.bbox.south * 4 - 1e-8) / 4;
  const double north = std::floor(request.bbox.north * 4 + 1e-8) / 4;
  const long rows = static_cast<long>(std::llround((north - south) * 4)) + 1;
  auto same_longitude = [](double a, double b) {
    return std::abs(std::remainder(a - b, 360.0)) < 1e-6;
  };
  std::set<std::pair<int, std::string>> seen;
  std::map<int, std::vector<unsigned char>> result;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < 20 ||
        std::memcmp(bytes.data() + offset, "GRIB", 4) || bytes[offset + 7] != 2)
      throw HostedGfsServiceError(
          "Hosted GFS result is not a clean GRIB2 stream");
    std::uint64_t length = 0;
    for (int i = 8; i < 16; ++i) length = (length << 8) | bytes[offset + i];
    if (length < 20 || length > bytes.size() - offset ||
        std::memcmp(bytes.data() + offset + length - 4, "7777", 4))
      throw HostedGfsServiceError("Hosted GFS result is truncated");
    std::unique_ptr<codes_handle, decltype(&codes_handle_delete)> handle(
        codes_handle_new_from_message_copy(nullptr, bytes.data() + offset,
                                           length),
        &codes_handle_delete);
    if (!handle)
      throw HostedGfsServiceError("Hosted GFS result cannot be decoded");
    const auto name = String(handle.get(), "shortName");
    const int hour = static_cast<int>(Long(handle.get(), "endStep"));
    if (Long(handle.get(), "dataDate") != std::stol(cycle.date) ||
        Long(handle.get(), "dataTime") != std::stol(cycle.cycle) * 100 ||
        Long(handle.get(), "stepUnits") != 1 ||
        std::find(hours.begin(), hours.end(), hour) == hours.end() ||
        !fields.contains(name) || !seen.emplace(hour, name).second)
      throw HostedGfsServiceError(
          "Hosted GFS result changed the requested cycle, times or fields");
    if (String(handle.get(), "gridType") != "regular_ll" ||
        std::abs(Number(handle.get(), "iDirectionIncrementInDegrees") - .25) >
            1e-6 ||
        std::abs(Number(handle.get(), "jDirectionIncrementInDegrees") - .25) >
            1e-6)
      throw HostedGfsServiceError(
          "Hosted GFS result changed the native grid spacing");
    const bool ascending = Long(handle.get(), "jScansPositively") != 0;
    if (columns < 1 || rows < 1 || Long(handle.get(), "Ni") != columns ||
        Long(handle.get(), "Nj") != rows ||
        Long(handle.get(), "iScansNegatively") != 0 ||
        !same_longitude(
            Number(handle.get(), "longitudeOfFirstGridPointInDegrees"), west) ||
        !same_longitude(
            Number(handle.get(), "longitudeOfLastGridPointInDegrees"),
            west + (columns - 1) * .25) ||
        std::abs(Number(handle.get(), "latitudeOfFirstGridPointInDegrees") -
                 (ascending ? south : north)) > 1e-6 ||
        std::abs(Number(handle.get(), "latitudeOfLastGridPointInDegrees") -
                 (ascending ? north : south)) > 1e-6)
      throw HostedGfsServiceError(
          "Hosted GFS result changed the requested geographical region");
    const auto level = String(handle.get(), "typeOfLevel");
    const auto height = Number(handle.get(), "level");
    if (((name == "10u" || name == "10v") &&
         (level != "heightAboveGround" || height != 10)) ||
        (name == "2t" && (level != "heightAboveGround" || height != 2)) ||
        (name == "prmsl" && level != "meanSea") ||
        (request.waves && level != "surface"))
      throw HostedGfsServiceError(
          "Hosted GFS result changed the requested field levels");
    auto& segment = result[hour];
    segment.insert(segment.end(), bytes.begin() + offset,
                   bytes.begin() + offset + length);
    offset += length;
  }
  if (seen.size() != fields.size() * hours.size())
    throw HostedGfsServiceError(
        "Hosted GFS result is missing requested fields or forecast times");
  return result;
}

std::vector<unsigned char> DownloadHostedGfs(const GFSRequest& request,
                                             const GFSCycle& cycle,
                                             const std::vector<int>& hours,
                                             ProgressCallback progress,
                                             HostedHttp http,
                                             RetrySleeper sleeper) {
  if (sodium_init() < 0)
    throw Error("Hosted GFS checksum initialization failed");
  if (!http) http = CurlRequest;
  if (!sleeper)
    sleeper = [](int milliseconds) {
      for (int elapsed = 0; elapsed < milliseconds; elapsed += 100) {
        CheckCancellation();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min(100, milliseconds - elapsed)));
      }
    };
  std::array<unsigned char, 16> random{};
  randombytes_buf(random.data(), random.size());
  const auto client = Hex(random.data(), random.size());
  Json::Value wanted(Json::objectValue);
  wanted["cycle"] = cycle.date + cycle.cycle;
  wanted["hours"] = Json::Value(Json::arrayValue);
  for (int hour : hours) wanted["hours"].append(hour);
  for (const auto& field : Fields(request))
    wanted["fields"][request.waves ? "waves" : "weather"].append(field);
  wanted["bbox"]["west"] = request.bbox.west;
  wanted["bbox"]["south"] = request.bbox.south;
  wanted["bbox"]["east"] = request.bbox.east;
  wanted["bbox"]["north"] = request.bbox.north;
  wanted["stride"] = 1;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::minutes(10);
  auto call = [&](const std::string& path, const std::string& method,
                  const std::string& body = "") {
    for (int attempt = 0;; ++attempt) {
      CheckCancellation();
      const auto remaining = std::chrono::duration<double>(
                                 deadline - std::chrono::steady_clock::now())
                                 .count();
      if (remaining <= 0)
        throw HostedGfsServiceError("Hosted GFS job timed out");
      auto reply =
          http(std::string(kGfsFallbackOrigin) + path, method, body, client,
               std::min(remaining, std::max(1.0, request.timeout_seconds)));
      if (reply.status == 429 || reply.status == 503) {
        if (reply.retry_after_seconds <= 0 || attempt >= 2 ||
            reply.retry_after_seconds >= remaining)
          throw HostedGfsServiceError(
              "Hosted GFS service is temporarily unavailable (HTTP " +
              std::to_string(reply.status) + "); retry later");
        if (progress) {
          Json::Value details;
          details["delaySeconds"] = reply.retry_after_seconds;
          progress("waiting for hosted GFS service", details);
        }
        sleeper(reply.retry_after_seconds * 1000);
        continue;
      }
      if (reply.status < 200 || reply.status >= 300)
        throw HostedGfsServiceError("Hosted GFS request failed with HTTP " +
                                    std::to_string(reply.status));
      return reply;
    }
  };
  const auto capabilities = ParseJson(call("/v1/capabilities", "GET").body);
  if (!capabilities["coverage"].isObject())
    throw HostedGfsServiceError(
        "Hosted GFS service returned invalid coverage information");
  const auto available = capabilities["coverage"][cycle.date + cycle.cycle]
                                     [request.waves ? "waves" : "weather"];
  for (int hour : hours)
    if (std::none_of(available.begin(), available.end(),
                     [&](const Json::Value& v) {
                       return v.isInt() && v.asInt() == hour;
                     }))
      throw HostedGfsCoverageUnavailable(
          "Exact GFS forecast cycle/times are not cached by the hosted "
          "service");
  const auto expected_request = Json::writeString(writer, wanted);
  auto job = ParseJson(call("/v1/subsets", "POST", expected_request).body);
  if (!job["status_url"].isString())
    throw HostedGfsServiceError("Hosted GFS service returned no status path");
  const auto path = job["status_url"].asString();
  if (!std::regex_match(path, std::regex("/v1/jobs/[0-9a-f]{32}")))
    throw HostedGfsServiceError(
        "Hosted GFS service returned an invalid status path");
  while (job["state"] == "queued" || job["state"] == "running") {
    if (progress) {
      Json::Value details;
      details["cycle"] = cycle.CycleTime();
      details["state"] = job["state"];
      details["progress"] = job["progress"];
      progress("creating hosted GFS regional forecast", details);
    }
    sleeper(1000);
    job = ParseJson(call(path, "GET").body);
  }
  if (job["state"] != "complete")
    throw HostedGfsServiceError("Hosted GFS regional generation failed");
  const auto result = job["result"];
  if (!result.isObject() || !result["result_url"].isString() ||
      !result["sha256"].isString())
    throw HostedGfsServiceError(
        "Hosted GFS service returned invalid result information");
  const auto result_path = result["result_url"].asString();
  const auto sha = result["sha256"].asString();
  if (!std::regex_match(result_path,
                        std::regex("/v1/results/[0-9a-f]{64}\\.grib2")) ||
      !std::regex_match(sha, std::regex("[0-9a-f]{64}")) ||
      result["request"] != wanted || !result["bytes"].isUInt64() ||
      result["bytes"].asUInt64() > kMaximumResult)
    throw HostedGfsServiceError(
        "Hosted GFS result metadata does not match the request");
  auto bytes = call(result_path, "GET").body;
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  crypto_hash_sha256(digest.data(), bytes.data(), bytes.size());
  if (bytes.size() != result["bytes"].asUInt64() ||
      Hex(digest.data(), digest.size()) != sha)
    throw HostedGfsServiceError("Hosted GFS result checksum/length mismatch");
  ValidateHostedGfsBytes(bytes, request, cycle, hours);
  return bytes;
}
}  // namespace environmental_grib
