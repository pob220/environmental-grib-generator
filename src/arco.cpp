#include "environmental_grib/arco.h"

#include <blosc.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>

#include "environmental_grib/error.h"
#include "environmental_grib/platform.h"
#include "environmental_grib/weather.h"

namespace environmental_grib {
namespace {
constexpr const char* kClientConfig =
    "https://stac.marine.copernicus.eu/clients-config-v1";
constexpr const char* kWaw3Root =
    "https://s3.waw3-1.cloudferro.com/mdl-metadata/metadata";
constexpr const char* kWaw4Root =
    "https://s3.waw4-1.cloudferro.com/mdl-metadata/metadata";

Json::Value Parse(const std::vector<unsigned char>& bytes,
                  const std::string& context) {
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value; std::string errors;
  const char* begin = reinterpret_cast<const char*>(bytes.data());
  if (!reader->parse(begin, begin + bytes.size(), &value, &errors))
    throw ValidationError("invalid " + context + " JSON: " + errors);
  return value;
}

std::string TrimSlash(std::string value) {
  while (!value.empty() && value.back() == '/') value.pop_back();
  return value;
}

void AddRoot(std::vector<std::string>& roots, std::set<std::string>& seen,
             std::string root) {
  root = TrimSlash(std::move(root));
  if (root.starts_with("https://") && seen.insert(root).second)
    roots.push_back(std::move(root));
}

std::vector<std::string> MetadataRoots(BinaryDownload& download,
                                       double timeout) {
  std::vector<std::string> roots;
  std::set<std::string> seen;
  try {
    const auto config = Parse(
        download(kClientConfig, std::min(timeout, 10.0)),
        "Copernicus client configuration");
    if (config["catalogues"].isArray()) {
      for (const auto& catalogue : config["catalogues"])
        AddRoot(roots, seen, catalogue["stacRoot"].asString());
    }
  } catch (const std::exception&) {
    // Static official roots retain compatibility when the CDN config is down.
  }
  AddRoot(roots, seen, kWaw3Root);
  AddRoot(roots, seen, kWaw4Root);
  return roots;
}

std::string DatasetUrl(const std::string& root, const std::string& product,
                       const std::string& href) {
  const std::string marker = "/" + product + "/";
  if (const auto at = href.find(marker); at != std::string::npos)
    return root + marker + href.substr(at + marker.size());
  if (href.starts_with("https://")) return href;
  return root + marker + href;
}

std::string Suffix(const std::string& username) {
  std::string result =
      "?x-cop-client=environmental-grib-generator&x-cop-client-version=0.1.2";
  if (!username.empty()) {
    result += "&x-cop-user=";
    for (unsigned char c : username) {
      if (std::isalnum(c) || c == '-' || c == '_' || c == '.') result += c;
      else {
        const char hex[] = "0123456789ABCDEF";
        result += '%'; result += hex[c >> 4]; result += hex[c & 15];
      }
    }
  }
  return result;
}

struct Axis { double minimum{}, step{}; std::size_t size{}, chunk{}; };

Axis AxisFor(const Json::Value& asset, const std::string& variable,
             const char* name) {
  const auto& dim = asset["viewDims"][name];
  const auto& coords = dim["coords"];
  Axis axis;
  axis.size = coords.isMember("len") ? coords["len"].asUInt64()
                                      : dim["len"].asUInt64();
  axis.chunk = dim["chunkLen"][variable].asUInt64();
  if (coords["type"].asString() == "explicit") {
    axis.minimum = coords["values"][0].asDouble();
    axis.step = 1.0;
  } else {
    axis.minimum = coords["min"].asDouble();
    axis.step = coords["step"].asDouble();
  }
  if (!axis.size || !axis.chunk || axis.step <= 0.0)
    throw ValidationError(std::string("invalid ARCO axis metadata: ") + name);
  return axis;
}

std::vector<unsigned char> Decompress(const std::vector<unsigned char>& input) {
  size_t bytes = 0, compressed = 0, block = 0;
  if (input.empty()) throw ValidationError("empty ARCO chunk");
  blosc_cbuffer_sizes(input.data(), &bytes, &compressed, &block);
  if (!bytes) throw ValidationError("invalid ARCO Blosc chunk header");
  std::vector<unsigned char> output(bytes);
  const int decoded = blosc_decompress(input.data(), output.data(), output.size());
  if (decoded < 0 || static_cast<std::size_t>(decoded) != output.size())
    throw ValidationError("ARCO Blosc chunk decompression failed");
  return output;
}

template <typename T>
T Little(const unsigned char* data) {
  T value; std::memcpy(&value, data, sizeof(T));
  if constexpr (std::endian::native == std::endian::big) {
    if constexpr (sizeof(T) == 2)
      value = std::bit_cast<T>(ByteSwap16(std::bit_cast<std::uint16_t>(value)));
    if constexpr (sizeof(T) == 4)
      value = std::bit_cast<T>(ByteSwap32(std::bit_cast<std::uint32_t>(value)));
  }
  return value;
}

class VariableReader {
 public:
  VariableReader(const ArcoDataset& dataset, std::string variable,
                 std::string username, BinaryDownload download, double timeout)
      : root_(dataset.service_url), variable_(std::move(variable)),
        username_(std::move(username)), download_(std::move(download)),
        timeout_(timeout) {
    const auto& asset = dataset.item["assets"]["timeChunked"];
    const auto& metadata = dataset.item["properties"]["cube:variables"][variable_];
    for (const auto& dimension : metadata["dimensions"]) dimensions_.push_back(dimension.asString());
    latitude_ = AxisFor(asset, variable_, "latitude");
    longitude_ = AxisFor(asset, variable_, "longitude");
    time_ = AxisFor(asset, variable_, "time");
    if (asset["viewDims"].isMember("elevation")) elevation_ = AxisFor(asset, variable_, "elevation");
    dtype_ = asset["viewVariables"][variable_]["dtype"].asString();
    scale_ = metadata["scale"].isNull() ? 1.0 : metadata["scale"].asDouble();
    offset_ = metadata["offset"].isNull() ? 0.0 : metadata["offset"].asDouble();
    missing_ = metadata["missingValue"].asDouble();
  }

  NetCDFScalarField Read(TimePoint instant, const RegularGrid& grid) {
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(instant.time_since_epoch()).count();
    const double raw_time = (milliseconds - time_.minimum) / time_.step;
    const auto time_index = static_cast<long long>(std::llround(raw_time));
    // Forecast products are commonly published on a coarser UTC cycle than
    // the user's requested start time (for example, three-hour wave fields
    // requested at 17:00).  Select the nearest published frame when it is no
    // farther than half one source interval.  This is bounded sampling, not
    // extrapolation: requests outside the time axis or beyond that tolerance
    // remain hard failures.
    constexpr double kIndexEpsilon = 1e-6;
    if (time_index < 0 || static_cast<std::size_t>(time_index) >= time_.size ||
        std::abs(raw_time - time_index) > 0.5 + kIndexEpsilon)
      throw ValidationError(
          "requested time is outside the usable ARCO forecast timeline");
    NetCDFScalarField result{instant, variable_, variable_, "",
                             std::vector<double>(grid.size()),
                             std::vector<std::uint8_t>(grid.size())};
    for (std::size_t y = 0; y < grid.ny(); ++y) for (std::size_t x = 0; x < grid.nx(); ++x) {
      const auto value = Bilinear(static_cast<std::size_t>(time_index),
                                  grid.latitudes[y], grid.longitudes[x]);
      const auto index = y * grid.nx() + x;
      result.values[index] = value.first;
      result.mask[index] = !value.second;
    }
    if (std::none_of(result.mask.begin(), result.mask.end(), [](auto value) { return value != 0; }))
      result.mask.clear();
    return result;
  }

 private:
  std::pair<double, bool> Bilinear(std::size_t time_index, double lat, double lon) {
    const double fy = (lat - latitude_.minimum) / latitude_.step;
    const double fx = (lon - longitude_.minimum) / longitude_.step;
    if (fx < 0 || fy < 0 || fx > longitude_.size - 1 || fy > latitude_.size - 1) return {0.0, false};
    const std::size_t x0 = std::floor(fx), y0 = std::floor(fy);
    const std::size_t x1 = std::min(x0 + 1, longitude_.size - 1), y1 = std::min(y0 + 1, latitude_.size - 1);
    const auto a = At(time_index, y0, x0), b = At(time_index, y0, x1);
    const auto c = At(time_index, y1, x0), d = At(time_index, y1, x1);
    if (!a.second || !b.second || !c.second || !d.second) return {0.0, false};
    const double tx = fx - x0, ty = fy - y0;
    return {(a.first * (1 - tx) + b.first * tx) * (1 - ty) +
            (c.first * (1 - tx) + d.first * tx) * ty, true};
  }

  std::pair<double, bool> At(std::size_t time_index, std::size_t y, std::size_t x) {
    const std::size_t tc = time_index / time_.chunk;
    const std::size_t yc = y / latitude_.chunk, xc = x / longitude_.chunk;
    std::vector<std::size_t> chunk_indices;
    for (const auto& dimension : dimensions_) {
      if (dimension == "time") chunk_indices.push_back(tc);
      else if (dimension == "latitude") chunk_indices.push_back(yc);
      else if (dimension == "longitude") chunk_indices.push_back(xc);
      else if (dimension == "elevation" || dimension == "depth") chunk_indices.push_back(0);
      else throw ValidationError("unsupported ARCO variable dimension: " + dimension);
    }
    std::ostringstream key;
    for (std::size_t i = 0; i < chunk_indices.size(); ++i) { if (i) key << '.'; key << chunk_indices[i]; }
    auto found = cache_.find(key.str());
    if (found == cache_.end()) {
      const auto bytes = download_(root_ + "/" + variable_ + "/" + key.str() + Suffix(username_), timeout_);
      found = cache_.emplace(key.str(), Decompress(bytes)).first;
    }
    const std::size_t time_count = std::min(time_.chunk, time_.size - tc * time_.chunk);
    const std::size_t lat_count = std::min(latitude_.chunk, latitude_.size - yc * latitude_.chunk);
    const std::size_t lon_count = std::min(longitude_.chunk, longitude_.size - xc * longitude_.chunk);
    const std::size_t elevation_count = elevation_ ? std::min(elevation_->chunk, elevation_->size) : 1;
    std::map<std::string, std::size_t> local{{"time", time_index % time_.chunk}, {"latitude", y % latitude_.chunk},
                                             {"longitude", x % longitude_.chunk}, {"elevation", 0}, {"depth", 0}};
    std::map<std::string, std::size_t> sizes{{"time", time_count}, {"latitude", lat_count},
                                             {"longitude", lon_count}, {"elevation", elevation_count}, {"depth", elevation_count}};
    std::size_t flat = 0;
    for (const auto& dimension : dimensions_) flat = flat * sizes[dimension] + local[dimension];
    const auto& raw = found->second;
    double value = 0.0;
    if (dtype_ == "<i2") {
      if ((flat + 1) * 2 > raw.size()) throw ValidationError("ARCO int16 chunk index is out of bounds");
      const auto packed = Little<std::int16_t>(raw.data() + flat * 2);
      if (packed == static_cast<std::int16_t>(missing_)) return {0.0, false};
      value = packed * scale_ + offset_;
    } else if (dtype_ == "<f4") {
      if ((flat + 1) * 4 > raw.size()) throw ValidationError("ARCO float32 chunk index is out of bounds");
      const auto packed = Little<float>(raw.data() + flat * 4);
      if (!std::isfinite(packed) || packed == static_cast<float>(missing_)) return {0.0, false};
      value = packed * scale_ + offset_;
    } else throw ValidationError("unsupported ARCO dtype: " + dtype_);
    return {value, true};
  }

  std::string root_, variable_, username_, dtype_;
  BinaryDownload download_;
  double timeout_{}, scale_{1.0}, offset_{}, missing_{};
  Axis latitude_, longitude_, time_;
  std::optional<Axis> elevation_;
  std::vector<std::string> dimensions_;
  std::unordered_map<std::string, std::vector<unsigned char>> cache_;
};
}  // namespace

ArcoDataset DiscoverArcoDataset(const std::string& product,
                                const std::string& dataset,
                                const std::string& username,
                                BinaryDownload download, double timeout) {
  if (!download) download = CurlHttpGet;
  std::vector<std::string> failures;
  const double attempt_timeout = std::min(timeout, 20.0);
  for (const auto& root : MetadataRoots(download, timeout)) {
    try {
      const auto product_json = Parse(
          download(root + "/" + product + "/product.stac.json" +
                       Suffix(username),
                   attempt_timeout),
          "STAC product");
      std::vector<std::string> matches;
      for (const auto& link : product_json["links"]) {
        const auto href = link["href"].asString();
        if (link["rel"].asString() == "item" &&
            href.find(dataset) != std::string::npos)
          matches.push_back(href);
      }
      if (matches.empty())
        throw ValidationError("dataset is not listed");
      std::sort(matches.begin(), matches.end());
      const auto item = Parse(
          download(DatasetUrl(root, product, matches.back()) +
                       Suffix(username),
                   attempt_timeout),
          "STAC dataset");
      const auto service =
          item["assets"]["timeChunked"]["href"].asString();
      if (service.empty())
        throw ValidationError("dataset has no timeChunked service");
      return {dataset, item["id"].asString(), service, root, item};
    } catch (const std::exception& error) {
      failures.push_back(root + ": " + error.what());
    }
  }
  std::ostringstream message;
  message << "Copernicus metadata discovery failed for " << dataset;
  for (const auto& failure : failures) message << "\n  " << failure;
  throw ValidationError(message.str());
}

std::map<std::string, std::vector<NetCDFScalarField>> ReadArcoFields(
    const ArcoDataset& dataset, const std::vector<std::string>& variables,
    const BoundingBox& bbox, const std::vector<TimePoint>& times,
    const RegularGrid& target_grid, const std::string& username,
    BinaryDownload download, double timeout) {
  if (!download) download = CurlHttpGet;
  std::map<std::string, std::vector<NetCDFScalarField>> result;
  for (const auto& variable : variables) {
    VariableReader reader(dataset, variable, username, download, timeout);
    for (const auto instant : times) result[variable].push_back(reader.Read(instant, target_grid));
  }
  (void)bbox;
  return result;
}

}  // namespace environmental_grib
