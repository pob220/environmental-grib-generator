#include "environmental_grib/copernicus.h"

#include <blosc.h>
#include <curl/curl.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>

#include "environmental_grib/error.h"
#include "environmental_grib/arco.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/model.h"
#include "environmental_grib/platform.h"
#include "environmental_grib/weather.h"

namespace environmental_grib {
namespace {
constexpr const char* kDataset = "cmems_mod_nws_phy-cur_anfc_1.5km-2D_PT1H-i";
constexpr const char* kProduct = "NWSHELF_ANALYSISFORECAST_PHY_004_013";
constexpr const char* kTokenUrl =
    "https://auth.marine.copernicus.eu/realms/MIS/protocol/openid-connect/token";
constexpr const char* kUserInfoUrl =
    "https://auth.marine.copernicus.eu/realms/MIS/protocol/openid-connect/userinfo";

Json::Value ParseJson(const std::vector<unsigned char>& bytes,
                      const std::string& context);

std::size_t CurlWrite(char* data, std::size_t size, std::size_t count,
                      void* user_data) {
  auto* output = static_cast<std::string*>(user_data);
  const std::size_t bytes = size * count;
  if (output->size() + bytes > 1024 * 1024) return 0;
  output->append(data, bytes);
  return bytes;
}

bool ValidateCredentialsImpl(const std::string& username,
                             const std::string& password, double timeout) {
  static const int initialized = [] { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
  if (initialized != CURLE_OK) throw ValidationError("initializing HTTP for Copernicus authentication failed");
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl) throw ValidationError("creating Copernicus authentication request failed");
  char* escaped_user = curl_easy_escape(curl.get(), username.c_str(), username.size());
  char* escaped_password = curl_easy_escape(curl.get(), password.c_str(), password.size());
  if (!escaped_user || !escaped_password) {
    if (escaped_user) curl_free(escaped_user);
    if (escaped_password) curl_free(escaped_password);
    throw ValidationError("encoding Copernicus credentials failed");
  }
  const std::string form = std::string("client_id=toolbox&grant_type=password&username=") +
                           escaped_user + "&password=" + escaped_password +
                           "&scope=openid%20profile%20email";
  curl_free(escaped_user);
  curl_free(escaped_password);
  std::string response;
  curl_easy_setopt(curl.get(), CURLOPT_URL, kTokenUrl);
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, form.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(form.size()));
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, CurlWrite);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(timeout * 1000.0));
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  const auto status = curl_easy_perform(curl.get());
  long http = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http);
  if (status != CURLE_OK) throw ValidationError(std::string("Copernicus authentication connection failed: ") + curl_easy_strerror(status));
  if (http == 401) return false;
  if (http != 200) throw ValidationError("Copernicus authentication service returned HTTP " + std::to_string(http));
  const auto token_json = ParseJson(std::vector<unsigned char>(response.begin(), response.end()), "Copernicus authentication");
  const auto token = token_json["access_token"].asString();
  if (token.empty()) throw ValidationError("Copernicus authentication response contained no access token");
  response.clear();
  const std::string authorization = "Authorization: Bearer " + token;
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
      curl_slist_append(nullptr, authorization.c_str()), curl_slist_free_all);
  curl_easy_reset(curl.get());
  curl_easy_setopt(curl.get(), CURLOPT_URL, kUserInfoUrl);
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, CurlWrite);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(timeout * 1000.0));
  const auto user_status = curl_easy_perform(curl.get());
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http);
  if (user_status != CURLE_OK || http != 200) throw ValidationError("Copernicus user-information validation failed");
  const auto user = ParseJson(std::vector<unsigned char>(response.begin(), response.end()), "Copernicus user information");
  return !user["preferred_username"].asString().empty();
}

Json::Value ParseJson(const std::vector<unsigned char>& bytes,
                      const std::string& context) {
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  std::string errors;
  const char* begin = reinterpret_cast<const char*>(bytes.data());
  if (!reader->parse(begin, begin + bytes.size(), &value, &errors))
    throw ValidationError("invalid JSON from " + context + ": " + errors);
  return value;
}

std::string QuerySuffix(const std::string& username) {
  std::ostringstream value;
  value << "?x-cop-client=environmental-grib-generator&x-cop-client-version=0.1.2";
  if (!username.empty()) {
    value << "&x-cop-user=";
    for (unsigned char c : username) {
      if (std::isalnum(c) || c == '-' || c == '_' || c == '.') value << c;
      else { const char hex[] = "0123456789ABCDEF"; value << '%' << hex[c >> 4] << hex[c & 15]; }
    }
  }
  return value.str();
}

std::vector<unsigned char> Get(BinaryDownload& download, const std::string& url,
                               const CopernicusRequest& request) {
  return download(url + QuerySuffix(request.username), request.timeout_seconds);
}

struct Axis { double minimum{}, maximum{}, step{}; std::size_t size{}; };
Axis ReadAxis(const Json::Value& asset, const char* name) {
  const auto& coordinates = asset["viewDims"][name]["coords"];
  Axis result{coordinates["min"].asDouble(), coordinates["max"].asDouble(),
              coordinates["step"].asDouble(), coordinates["len"].asUInt64()};
  if (result.size < 1 || result.step <= 0.0) throw ValidationError(std::string("invalid Copernicus ") + name + " axis metadata");
  return result;
}

std::vector<std::int16_t> DecodeInt16Blosc(const std::vector<unsigned char>& compressed,
                                           std::size_t expected_values) {
  if (compressed.empty()) throw ValidationError("Copernicus Zarr chunk is empty");
  std::vector<std::int16_t> values(expected_values);
  const int decoded = blosc_decompress(compressed.data(), values.data(), values.size() * sizeof(std::int16_t));
  if (decoded < 0 || static_cast<std::size_t>(decoded) != values.size() * sizeof(std::int16_t))
    throw ValidationError("Copernicus Zarr chunk failed Blosc decompression or had unexpected size");
  if constexpr (std::endian::native == std::endian::big) {
    for (auto& value : values)
      value = static_cast<std::int16_t>(
          ByteSwap16(static_cast<std::uint16_t>(value)));
  }
  return values;
}

std::vector<double> DecodeComponent(BinaryDownload& download,
                                    const CopernicusRequest& request,
                                    const std::string& root,
                                    const std::string& variable,
                                    std::size_t time_index,
                                    std::size_t source_points,
                                    double scale, double offset,
                                    std::int16_t fill) {
  const auto bytes = Get(download, root + "/" + variable + "/" +
                                      std::to_string(time_index) + ".0.0", request);
  const auto packed = DecodeInt16Blosc(bytes, source_points);
  std::vector<double> result(source_points, std::numeric_limits<double>::quiet_NaN());
  for (std::size_t i = 0; i < packed.size(); ++i)
    if (packed[i] != fill) result[i] = packed[i] * scale + offset;
  return result;
}

std::pair<double, bool> Bilinear(const std::vector<double>& values,
                                 const Axis& lat, const Axis& lon,
                                 double latitude, double longitude) {
  const double fy = (latitude - lat.minimum) / lat.step;
  const double fx = (longitude - lon.minimum) / lon.step;
  if (fx < 0.0 || fy < 0.0 || fx > lon.size - 1 || fy > lat.size - 1) return {0.0, false};
  const auto x0 = static_cast<std::size_t>(std::floor(fx));
  const auto y0 = static_cast<std::size_t>(std::floor(fy));
  const auto x1 = std::min(x0 + 1, lon.size - 1);
  const auto y1 = std::min(y0 + 1, lat.size - 1);
  const std::array<std::size_t, 4> index{y0 * lon.size + x0, y0 * lon.size + x1,
                                        y1 * lon.size + x0, y1 * lon.size + x1};
  for (auto i : index) if (!std::isfinite(values[i])) return {0.0, false};
  const double tx = fx - x0, ty = fy - y0;
  const double lower = values[index[0]] * (1.0 - tx) + values[index[1]] * tx;
  const double upper = values[index[2]] * (1.0 - tx) + values[index[3]] * tx;
  return {lower * (1.0 - ty) + upper * ty, true};
}
}  // namespace

bool ValidateCopernicusCredentials(const std::string& username,
                                   const std::string& password,
                                   double timeout_seconds) {
  return ValidateCredentialsImpl(username, password, timeout_seconds);
}

CopernicusResult GenerateCopernicusNws(const CopernicusRequest& request,
                                       BinaryDownload download,
                                       CredentialValidator validate_credentials) {
  request.bbox.Validate();
  if (request.provider != "copernicus_nws")
    throw UnsupportedSourceError("native Copernicus provider currently supports copernicus_nws only");
  if (request.hours < 0 || request.step_hours <= 0)
    throw ValidationError("Copernicus hours must be non-negative and step must be positive");
  if (request.username.empty() || request.password.empty())
    throw ValidationError("Copernicus username and password are required");
  if (std::filesystem::exists(request.output) && !request.overwrite)
    throw ValidationError("output already exists: " + PathToUtf8(request.output));
  if (!download)
    download = BinaryDownload(MakeRetryingHttpGet(
        {}, "Copernicus NWS current", {}, {5, 1000, 8000}));
  if (!validate_credentials) validate_credentials = ValidateCredentialsImpl;
  if (!validate_credentials(request.username, request.password, request.timeout_seconds))
    throw ValidationError("invalid Copernicus username or password");
  const auto dataset = DiscoverArcoDataset(
      kProduct, kDataset, request.username, download, request.timeout_seconds);
  const auto& item = dataset.item;
  const auto& asset = item["assets"]["timeChunked"];
  const std::string root = asset["href"].asString();
  if (root.empty()) throw ValidationError("Copernicus dataset has no timeChunked Zarr asset");
  const Axis latitude = ReadAxis(asset, "latitude");
  const Axis longitude = ReadAxis(asset, "longitude");
  const Axis time = ReadAxis(asset, "time");
  if (request.bbox.west < longitude.minimum || request.bbox.east > longitude.maximum ||
      request.bbox.south < latitude.minimum || request.bbox.north > latitude.maximum)
    throw ValidationError("requested bbox is outside Copernicus NWS coverage");
  CopernicusResult result{request.output, 0, 0, kDataset, item["id"].asString(), root,
                          Json::Value(Json::objectValue)};
  result.summary["provider"] = request.provider;
  result.summary["dataset_id"] = result.dataset_id;
  result.summary["dataset_version"] = result.dataset_version;
  result.summary["metadata_root"] = dataset.metadata_root;
  result.summary["service"] = "Copernicus Marine ARCO time-chunked Zarr v2";
  if (request.dry_run) return result;
  const auto requested_times = BuildTimeSequence(request.start, request.hours, request.step_hours);
  const auto grid = BuildRegularGrid(request.bbox, request.grid_spacing_deg);
  const auto& variable_meta = item["properties"]["cube:variables"];
  std::vector<CurrentGrid> currents;
  for (const auto instant : requested_times) {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(instant.time_since_epoch()).count();
    const double raw_index = (milliseconds - time.minimum) / time.step;
    const auto index = static_cast<long long>(std::llround(raw_index));
    if (index < 0 || static_cast<std::size_t>(index) >= time.size || std::abs(raw_index - index) > 1e-6)
      throw ValidationError("requested time is unavailable in Copernicus NWS dataset");
    const auto decode = [&](const char* variable) {
      const auto& metadata = variable_meta[variable];
      return DecodeComponent(download, request, root, variable,
                             static_cast<std::size_t>(index), latitude.size * longitude.size,
                             metadata["scale"].asDouble(), metadata["offset"].asDouble(),
                             static_cast<std::int16_t>(metadata["missingValue"].asInt()));
    };
    const auto u = decode("uo"), v = decode("vo");
    CurrentGrid current{instant, grid, std::vector<double>(grid.size()),
                        std::vector<double>(grid.size()), std::vector<std::uint8_t>(grid.size())};
    for (std::size_t y = 0; y < grid.ny(); ++y) {
      for (std::size_t x = 0; x < grid.nx(); ++x) {
        const auto ui = Bilinear(u, latitude, longitude, grid.latitudes[y], grid.longitudes[x]);
        const auto vi = Bilinear(v, latitude, longitude, grid.latitudes[y], grid.longitudes[x]);
        const auto target = y * grid.nx() + x;
        current.mask[target] = !(ui.second && vi.second);
        current.u_mps[target] = ui.second ? ui.first : 0.0;
        current.v_mps[target] = vi.second ? vi.first : 0.0;
      }
    }
    if (std::none_of(current.mask.begin(), current.mask.end(), [](auto value) { return value != 0; })) current.mask.clear();
    currents.push_back(std::move(current));
  }
  WriteGrib1Currents(currents, request.output);
  const auto inspection = InspectGrib(request.output);
  result.message_count = inspection["message_count"].asUInt64();
  result.byte_count = std::filesystem::file_size(request.output);
  result.summary["grid_points"] = Json::UInt64(grid.size());
  result.summary["time_count"] = Json::UInt64(requested_times.size());
  return result;
}

Json::Value CopernicusResultJson(const CopernicusResult& result) {
  Json::Value value(Json::objectValue);
  value["output"] = PathToUtf8(result.output);
  value["message_count"] = Json::UInt64(result.message_count);
  value["byte_count"] = Json::UInt64(result.byte_count);
  value["dataset_id"] = result.dataset_id;
  value["dataset_version"] = result.dataset_version;
  value["summary"] = result.summary;
  return value;
}

CopernicusResult GenerateCopernicusArcoCurrent(
    const CopernicusRequest& request, const std::string& expected_provider,
    const std::string& provider_label, const std::string& product_id,
    const std::string& dataset_id, BinaryDownload download,
    CredentialValidator validate_credentials) {
  request.bbox.Validate();
  if (request.provider != expected_provider)
    throw UnsupportedSourceError(provider_label +
                                 " generator requires provider " +
                                 expected_provider);
  if (request.hours < 0 || request.step_hours <= 0)
    throw ValidationError("Copernicus hours must be non-negative and step must be positive");
  if (request.username.empty() || request.password.empty())
    throw ValidationError("Copernicus username and password are required");
  if (std::filesystem::exists(request.output) && !request.overwrite)
    throw ValidationError("output already exists: " + PathToUtf8(request.output));
  if (!download)
    download = BinaryDownload(MakeRetryingHttpGet(
        {}, provider_label, {}, {5, 1000, 8000}));
  if (!validate_credentials) validate_credentials = ValidateCredentialsImpl;
  if (!validate_credentials(request.username, request.password, request.timeout_seconds))
    throw ValidationError("invalid Copernicus username or password");
  const auto dataset = DiscoverArcoDataset(
      product_id, dataset_id, request.username, download,
      request.timeout_seconds);
  CopernicusResult result{request.output, 0, 0, dataset.dataset_id,
                          dataset.version_id, dataset.service_url,
                          Json::Value(Json::objectValue)};
  result.summary["provider"] = request.provider;
  result.summary["dataset_id"] = result.dataset_id;
  result.summary["dataset_version"] = result.dataset_version;
  result.summary["metadata_root"] = dataset.metadata_root;
  result.summary["service"] =
      "Copernicus Marine ARCO spatially chunked time-series";
  if (request.dry_run) return result;
  const auto times = BuildTimeSequence(request.start, request.hours, request.step_hours);
  const auto grid = BuildRegularGrid(request.bbox, request.grid_spacing_deg);
  const auto fields = ReadArcoFields(dataset, {"uo", "vo"}, request.bbox,
                                     times, grid, request.username, download,
                                     request.timeout_seconds);
  std::vector<CurrentGrid> currents;
  for (std::size_t t = 0; t < times.size(); ++t) {
    CurrentGrid current{times[t], grid, fields.at("uo")[t].values,
                        fields.at("vo")[t].values,
                        std::vector<std::uint8_t>(grid.size())};
    for (std::size_t i = 0; i < grid.size(); ++i) {
      const bool u_missing = !fields.at("uo")[t].mask.empty() && fields.at("uo")[t].mask[i];
      const bool v_missing = !fields.at("vo")[t].mask.empty() && fields.at("vo")[t].mask[i];
      current.mask[i] = u_missing || v_missing;
      if (current.mask[i]) { current.u_mps[i] = 0.0; current.v_mps[i] = 0.0; }
    }
    if (std::none_of(current.mask.begin(), current.mask.end(), [](auto value) { return value != 0; }))
      current.mask.clear();
    currents.push_back(std::move(current));
  }
  WriteGrib1Currents(currents, request.output);
  const auto inspection = InspectGrib(request.output);
  result.message_count = inspection["message_count"].asUInt64();
  result.byte_count = std::filesystem::file_size(request.output);
  result.summary["grid_points"] = Json::UInt64(grid.size());
  result.summary["time_count"] = Json::UInt64(times.size());
  return result;
}

CopernicusResult GenerateCopernicusGlobal(
    const CopernicusRequest& request, BinaryDownload download,
    CredentialValidator validate_credentials) {
  return GenerateCopernicusArcoCurrent(
      request, "copernicus_global", "Copernicus Global current",
      "GLOBAL_ANALYSISFORECAST_PHY_001_024",
      "cmems_mod_glo_phy_anfc_0.083deg_PT1H-m", std::move(download),
      std::move(validate_credentials));
}

CopernicusResult GenerateCopernicusIbi(
    const CopernicusRequest& request, BinaryDownload download,
    CredentialValidator validate_credentials) {
  return GenerateCopernicusArcoCurrent(
      request, "copernicus_ibi", "Copernicus IBI current",
      "IBI_ANALYSISFORECAST_PHY_005_001",
      "cmems_mod_ibi_phy_anfc_0.027deg-2D_PT1H-m", std::move(download),
      std::move(validate_credentials));
}

CopernicusResult GenerateCopernicusMediterranean(
    const CopernicusRequest& request, BinaryDownload download,
    CredentialValidator validate_credentials) {
  return GenerateCopernicusArcoCurrent(
      request, "copernicus_mediterranean",
      "Copernicus Mediterranean current",
      "MEDSEA_ANALYSISFORECAST_PHY_006_013",
      "cmems_mod_med_phy-cur_anfc_4.2km-2D_PT1H-m", std::move(download),
      std::move(validate_credentials));
}

}  // namespace environmental_grib
