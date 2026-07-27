#include "environmental_grib/metno.h"

#include <netcdf.h>
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
#include <proj.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numbers>
#include <numeric>
#include <span>
#include <sstream>
#include <thread>

#include "environmental_grib/error.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/parallel.h"
#include "environmental_grib/platform.h"

namespace environmental_grib {
namespace {

void Nc(int status, const std::string& action) {
  if (status != NC_NOERR)
    throw ValidationError(action + ": " + nc_strerror(status));
}

class NcFile {
public:
  explicit NcFile(const std::string& location) {
    Nc(nc_open(location.c_str(), NC_NOWRITE, &id_),
       "opening MET Norway Nordic NetCDF");
  }
  ~NcFile() {
    if (id_ >= 0) nc_close(id_);
  }
  int id() const { return id_; }

private:
  int id_{-1};
};

std::string AttributeText(int file, int variable, const char* name) {
  std::size_t size = 0;
  if (nc_inq_attlen(file, variable, name, &size) != NC_NOERR) return {};
  std::string value(size, '\0');
  Nc(nc_get_att_text(file, variable, name, value.data()),
     "reading MET Norway text attribute");
  return value;
}

double AttributeDouble(int file, int variable, const char* name,
                       double fallback) {
  double value = fallback;
  return nc_get_att_double(file, variable, name, &value) == NC_NOERR ? value
                                                                     : fallback;
}

int Variable(int file, const char* name) {
  int id = -1;
  Nc(nc_inq_varid(file, name, &id),
     std::string("finding MET Norway variable ") + name);
  return id;
}

std::vector<int> Dims(int file, int variable) {
  int count = 0;
  Nc(nc_inq_varndims(file, variable, &count), "reading MET Norway dimensions");
  std::vector<int> result(static_cast<std::size_t>(count));
  Nc(nc_inq_vardimid(file, variable, result.data()),
     "reading MET Norway dimension ids");
  return result;
}

std::size_t DimSize(int file, int dimension) {
  std::size_t result = 0;
  Nc(nc_inq_dimlen(file, dimension, &result),
     "reading MET Norway dimension size");
  return result;
}

std::vector<double> Read1D(int file, int variable) {
  const auto dims = Dims(file, variable);
  if (dims.size() != 1)
    throw ValidationError(
        "MET Norway projected coordinate must be one-dimensional");
  std::vector<double> result(DimSize(file, dims.front()));
  Nc(nc_get_var_double(file, variable, result.data()),
     "reading MET Norway projected coordinate");
  return result;
}

double TimeUnitSeconds(const std::string& units) {
  if (units.starts_with("seconds since ")) return 1.0;
  if (units.starts_with("hours since ")) return 3600.0;
  throw ValidationError("unsupported MET Norway time units: " + units);
}

std::string CfTimeOrigin(const std::string& units) {
  const auto separator = units.find(" since ");
  if (separator == std::string::npos)
    throw ValidationError("MET Norway time coordinate lacks CF units");
  auto origin = units.substr(separator + 7);
  while (!origin.empty() && origin.back() == ' ') origin.pop_back();
  if (origin.ends_with(" +00:00")) origin.erase(origin.size() - 7);
  if (origin.find('T') == std::string::npos && origin.size() >= 11)
    origin[10] = 'T';
  if (!origin.ends_with('Z')) origin += 'Z';
  return origin;
}

TimePoint ReadReferenceTime(int file) {
  const int variable = Variable(file, "forecast_reference_time");
  double value = 0.0;
  Nc(nc_get_var_double(file, variable, &value),
     "reading MET Norway forecast reference time");
  const auto units = AttributeText(file, variable, "units");
  return ParseUtcDateTime(CfTimeOrigin(units)) +
         std::chrono::seconds(static_cast<long long>(
             std::llround(value * TimeUnitSeconds(units))));
}

struct ProjectionMetadata {
  double lat0{};
  double lon0{};
  double lat1{};
  double lat2{};
  double radius{};
};

ProjectionMetadata ReadProjection(int file) {
  int mapping = -1;
  if (nc_inq_varid(file, "projection_lcc", &mapping) != NC_NOERR) {
    const int pressure = Variable(file, "air_pressure_at_sea_level");
    const auto name = AttributeText(file, pressure, "grid_mapping");
    if (name.empty() || nc_inq_varid(file, name.c_str(), &mapping) != NC_NOERR)
      throw ValidationError("MET Norway source lacks its LCC grid mapping");
  }
  if (AttributeText(file, mapping, "grid_mapping_name") !=
      "lambert_conformal_conic")
    throw ValidationError(
        "unsupported MET Norway projection; expected Lambert conformal conic");
  std::array<double, 2> parallels{};
  std::size_t parallel_count = 0;
  if (nc_inq_attlen(file, mapping, "standard_parallel", &parallel_count) !=
          NC_NOERR ||
      parallel_count == 0 || parallel_count > parallels.size())
    throw ValidationError("MET Norway projection has invalid parallels");
  Nc(nc_get_att_double(file, mapping, "standard_parallel", parallels.data()),
     "reading MET Norway standard parallels");
  if (parallel_count == 1) parallels[1] = parallels[0];
  return {AttributeDouble(file, mapping, "latitude_of_projection_origin",
                          std::numeric_limits<double>::quiet_NaN()),
          AttributeDouble(file, mapping, "longitude_of_central_meridian",
                          std::numeric_limits<double>::quiet_NaN()),
          parallels[0], parallels[1],
          AttributeDouble(file, mapping, "earth_radius",
                          std::numeric_limits<double>::quiet_NaN())};
}

#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
class Projection {
public:
  explicit Projection(const ProjectionMetadata& metadata) {
    const std::array<double, 5> values{metadata.lat0, metadata.lon0,
                                       metadata.lat1, metadata.lat2,
                                       metadata.radius};
    if (!std::all_of(values.begin(), values.end(),
                     [](double value) { return std::isfinite(value); }) ||
        metadata.radius <= 0.0)
      throw ValidationError("MET Norway source has invalid projection data");
    context_ = proj_context_create();
    if (!context_)
      throw ValidationError(
          "PROJ could not create a MET Norway projection context");
    std::ostringstream pipeline;
    pipeline << "+proj=pipeline"
             << " +step +proj=unitconvert +xy_in=deg +xy_out=rad"
             << " +step +proj=lcc +lat_0=" << metadata.lat0
             << " +lon_0=" << metadata.lon0 << " +lat_1=" << metadata.lat1
             << " +lat_2=" << metadata.lat2 << " +R=" << metadata.radius
             << " +units=m +no_defs";
    projection_ = proj_create(context_, pipeline.str().c_str());
    if (!projection_) {
      proj_context_destroy(context_);
      context_ = nullptr;
      throw ValidationError(
          "PROJ could not create the MET Norway coordinate pipeline");
    }
  }
  ~Projection() {
    if (projection_) proj_destroy(projection_);
    if (context_) proj_context_destroy(context_);
  }
  std::pair<double, double> Forward(double longitude, double latitude) const {
    proj_errno_reset(projection_);
    const auto result = proj_trans(projection_, PJ_FWD,
                                   proj_coord(longitude, latitude, 0.0, 0.0));
    if (const int error = proj_errno(projection_); error != 0) {
      std::string message =
          "PROJ failed a MET Norway coordinate transformation";
      if (const char* detail = proj_errno_string(error); detail && *detail)
        message += ": " + std::string(detail);
      throw ValidationError(message);
    }
    return {result.xy.x, result.xy.y};
  }

private:
  PJ_CONTEXT* context_{};
  PJ* projection_{};
};
#endif

struct AxisSlice {
  std::size_t start{};
  std::size_t count{};
  std::vector<double> values;
};

AxisSlice SliceAxis(const std::vector<double>& axis, double minimum,
                    double maximum) {
  if (axis.size() < 2 || !std::is_sorted(axis.begin(), axis.end()))
    throw ValidationError("MET Norway projected axes must be ascending");
  auto begin = std::lower_bound(axis.begin(), axis.end(), minimum);
  auto end = std::upper_bound(axis.begin(), axis.end(), maximum);
  if (begin == axis.end() || end == axis.begin())
    throw ValidationError("MET Norway bbox is outside the source grid");
  std::size_t first = begin == axis.begin()
                          ? 0
                          : static_cast<std::size_t>(begin - axis.begin() - 1);
  std::size_t last = end == axis.end()
                         ? axis.size() - 1
                         : static_cast<std::size_t>(end - axis.begin());
  if (first > 0) --first;
  if (last + 1 < axis.size()) ++last;
  return {first, last - first + 1,
          std::vector<double>(
              axis.begin() + static_cast<std::ptrdiff_t>(first),
              axis.begin() + static_cast<std::ptrdiff_t>(last + 1))};
}

struct SourceFieldBatch {
  const AxisSlice* x{};
  const AxisSlice* y{};
  std::size_t time_count{};
  std::vector<double> values;
  std::vector<std::uint8_t> mask;

  [[nodiscard]] std::size_t spatial_size() const { return x->count * y->count; }
  [[nodiscard]] std::span<const double> Values(std::size_t time) const {
    return {values.data() + time * spatial_size(), spatial_size()};
  }
  [[nodiscard]] std::span<const std::uint8_t> Mask(std::size_t time) const {
    return {mask.data() + time * spatial_size(), spatial_size()};
  }
};

struct BatchRead {
  SourceFieldBatch field;
  std::size_t requests{};
};

BatchRead ReadFieldBatch(int file, const char* name,
                         const std::vector<std::size_t>& time_indices,
                         const AxisSlice& x, const AxisSlice& y,
                         bool allow_strided_time_read) {
  if (time_indices.empty())
    throw ValidationError("MET Norway field batch must not be empty");
  const int variable = Variable(file, name);
  const auto dimensions = Dims(file, variable);
  if (dimensions.size() != 3)
    throw ValidationError(std::string("MET Norway field ") + name +
                          " must use time/y/x dimensions");
  const std::size_t spatial_size = x.count * y.count;
  std::vector<double> values(time_indices.size() * spatial_size);
  std::size_t requests = 0;
  bool regularly_strided = allow_strided_time_read;
  std::size_t time_stride = 1;
  if (time_indices.size() > 1) {
    if (time_indices[1] <= time_indices[0]) {
      regularly_strided = false;
    } else {
      time_stride = time_indices[1] - time_indices[0];
    }
    for (std::size_t i = 2; i < time_indices.size(); ++i)
      if (time_indices[i] <= time_indices[i - 1] ||
          time_indices[i] - time_indices[i - 1] != time_stride)
        regularly_strided = false;
  }
  if (regularly_strided) {
    const std::array<std::size_t, 3> start{time_indices.front(), y.start,
                                           x.start};
    const std::array<std::size_t, 3> count{time_indices.size(), y.count,
                                           x.count};
    if (time_indices.size() == 1) {
      Nc(nc_get_vara_double(file, variable, start.data(), count.data(),
                            values.data()),
         std::string("reading MET Norway field ") + name);
    } else {
      const std::array<std::ptrdiff_t, 3> stride{
          static_cast<std::ptrdiff_t>(time_stride), 1, 1};
      Nc(nc_get_vars_double(file, variable, start.data(), count.data(),
                            stride.data(), values.data()),
         std::string("reading batched MET Norway field ") + name);
    }
    requests = 1;
  } else {
    for (std::size_t i = 0; i < time_indices.size(); ++i) {
      const std::array<std::size_t, 3> start{time_indices[i], y.start, x.start};
      const std::array<std::size_t, 3> count{1, y.count, x.count};
      Nc(nc_get_vara_double(file, variable, start.data(), count.data(),
                            values.data() + i * spatial_size),
         std::string("reading MET Norway field ") + name);
      ++requests;
    }
  }
  double fill = std::numeric_limits<double>::quiet_NaN();
  const bool have_fill =
      nc_get_att_double(file, variable, "_FillValue", &fill) == NC_NOERR ||
      nc_get_att_double(file, variable, "missing_value", &fill) == NC_NOERR;
  const double scale = AttributeDouble(file, variable, "scale_factor", 1.0);
  const double offset = AttributeDouble(file, variable, "add_offset", 0.0);
  std::vector<std::uint8_t> mask(values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    mask[i] = !std::isfinite(values[i]) || (have_fill && values[i] == fill);
    if (!mask[i]) values[i] = values[i] * scale + offset;
  }
  return {{&x, &y, time_indices.size(), std::move(values), std::move(mask)},
          requests};
}

std::array<double, 3> Bracket(const std::vector<double>& axis, double value) {
  const auto upper = std::lower_bound(axis.begin(), axis.end(), value);
  if (upper == axis.end() || (upper == axis.begin() && *upper != value))
    return {-1.0, -1.0, 0.0};
  if (*upper == value) {
    const auto index = static_cast<double>(upper - axis.begin());
    return {index, index, 0.0};
  }
  const auto high = static_cast<std::size_t>(upper - axis.begin());
  const auto low = high - 1;
  return {static_cast<double>(low), static_cast<double>(high),
          (value - axis[low]) / (axis[high] - axis[low])};
}

#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
struct InterpolationPoint {
  std::array<std::size_t, 4> index{};
  double x_weight{};
  double y_weight{};
  bool covered{};
};

std::vector<InterpolationPoint> BuildInterpolationPlan(
    const Projection& projection, const RegularGrid& grid, const AxisSlice& x,
    const AxisSlice& y) {
  std::vector<InterpolationPoint> plan(grid.size());
  for (std::size_t row = 0; row < grid.ny(); ++row)
    for (std::size_t column = 0; column < grid.nx(); ++column) {
      const auto [projected_x, projected_y] =
          projection.Forward(grid.longitudes[column], grid.latitudes[row]);
      const auto bx = Bracket(x.values, projected_x);
      const auto by = Bracket(y.values, projected_y);
      auto& point = plan[row * grid.nx() + column];
      if (bx[0] < 0.0 || by[0] < 0.0) continue;
      const auto x0 = static_cast<std::size_t>(bx[0]);
      const auto x1 = static_cast<std::size_t>(bx[1]);
      const auto y0 = static_cast<std::size_t>(by[0]);
      const auto y1 = static_cast<std::size_t>(by[1]);
      point.index = {y0 * x.count + x0, y0 * x.count + x1, y1 * x.count + x0,
                     y1 * x.count + x1};
      point.x_weight = bx[2];
      point.y_weight = by[2];
      point.covered = true;
    }
  return plan;
}

std::pair<double, bool> Interpolate(std::span<const double> values,
                                    std::span<const std::uint8_t> mask,
                                    const InterpolationPoint& point) {
  if (!point.covered) return {0.0, false};
  for (const auto i : point.index)
    if (mask[i]) return {0.0, false};
  const double lower = values[point.index[0]] * (1.0 - point.x_weight) +
                       values[point.index[1]] * point.x_weight;
  const double upper = values[point.index[2]] * (1.0 - point.x_weight) +
                       values[point.index[3]] * point.x_weight;
  return {lower * (1.0 - point.y_weight) + upper * point.y_weight, true};
}

std::pair<std::vector<double>, std::vector<std::uint8_t>> Regrid(
    std::span<const double> source_values,
    std::span<const std::uint8_t> source_mask,
    const std::vector<InterpolationPoint>& plan, const RegularGrid& grid,
    std::size_t workers, double scale) {
  std::pair<std::vector<double>, std::vector<std::uint8_t>> result{
      std::vector<double>(grid.size()), std::vector<std::uint8_t>(grid.size())};
  std::vector<std::size_t> rows(grid.ny());
  std::iota(rows.begin(), rows.end(), 0);
  ParallelMapOrdered(rows, workers, [&](const std::size_t& row) {
    const std::size_t first = row * grid.nx();
    for (std::size_t column = 0; column < grid.nx(); ++column) {
      const std::size_t index = first + column;
      const auto interpolated =
          Interpolate(source_values, source_mask, plan[index]);
      result.first[index] = interpolated.first * scale;
      result.second[index] = !interpolated.second;
    }
    return true;
  });
  return result;
}

std::filesystem::path TemporaryMetNoOutput(
    const std::filesystem::path& output) {
  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    auto candidate = output;
    candidate += "." + std::to_string(ProcessId()) + "." +
                 std::to_string(attempt) + ".metno.tmp";
    if (!std::filesystem::exists(candidate)) return candidate;
  }
  throw ValidationError("unable to allocate temporary MET Norway output");
}

double ElapsedSeconds(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
}

std::size_t RegridWorkerCount(std::size_t rows) {
  const std::size_t detected =
      std::max<std::size_t>(1, std::thread::hardware_concurrency());
  return std::min<std::size_t>({rows, detected, 8});
}
#endif

struct MetNoCacheEntry {
  std::filesystem::path data;
  std::filesystem::path metadata;
  std::string fingerprint;
};

std::size_t ExpectedMessageCount(const std::vector<int>& hours,
                                 const std::string& preset) {
  std::size_t result = 0;
  for (const int hour : hours) {
    result += 2;
    if (preset != "minimal") result += 2;
    if (preset == "marine" || preset == "all") {
      result += 2;
      if (hour > 0) ++result;
    }
    if (preset == "all") ++result;
  }
  return result;
}

std::string CacheFingerprint(const MetNoRequest& request, TimePoint reference,
                             const std::vector<int>& hours) {
  std::ostringstream value;
  value << "metno-weather-cache-v1|generator-0.1.5|"
        << FormatUtcDateTime(reference) << '|' << request.dataset_url << '|'
        << std::setprecision(17) << request.bbox.west << ','
        << request.bbox.south << ',' << request.bbox.east << ','
        << request.bbox.north << '|' << request.grid_spacing_deg << '|'
        << request.preset << '|';
  for (const int hour : hours) value << hour << ',';
  return value.str();
}

std::string FingerprintHash(const std::string& value) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  std::ostringstream result;
  result << std::hex << std::setw(16) << std::setfill('0') << hash;
  return result.str();
}

MetNoCacheEntry CacheEntry(const MetNoRequest& request, TimePoint reference,
                           const std::vector<int>& hours) {
  const auto fingerprint = CacheFingerprint(request, reference, hours);
  const auto base = *request.cache_directory / FingerprintHash(fingerprint);
  auto data = base;
  data += ".grb2";
  auto metadata = base;
  metadata += ".json";
  return {std::move(data), std::move(metadata), fingerprint};
}

std::optional<Json::Value> ReadCacheMetadata(
    const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  Json::CharReaderBuilder builder;
  Json::Value value;
  std::string errors;
  if (!Json::parseFromStream(builder, input, &value, &errors))
    return std::nullopt;
  return value;
}

void ReplaceWithCopy(const std::filesystem::path& source,
                     const std::filesystem::path& destination) {
  std::filesystem::create_directories(
      destination.parent_path().empty() ? "." : destination.parent_path());
  auto temporary = destination;
  temporary += "." + std::to_string(ProcessId()) + ".cache.tmp";
  try {
    std::filesystem::copy_file(
        source, temporary, std::filesystem::copy_options::overwrite_existing);
    std::error_code error;
    if (std::filesystem::exists(destination)) {
      std::filesystem::remove(destination, error);
      if (error)
        throw ValidationError("unable to replace cached MET Norway output: " +
                              error.message());
    }
    std::filesystem::rename(temporary, destination, error);
    if (error)
      throw ValidationError("unable to install cached MET Norway output: " +
                            error.message());
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

std::optional<std::pair<std::size_t, double>> RestoreCache(
    const MetNoCacheEntry& entry, const std::filesystem::path& output,
    std::size_t expected_messages) {
  const auto metadata = ReadCacheMetadata(entry.metadata);
  if (!metadata || (*metadata)["schemaVersion"].asInt() != 1 ||
      (*metadata)["fingerprint"].asString() != entry.fingerprint ||
      (*metadata)["messageCount"].asUInt64() != expected_messages ||
      !std::filesystem::is_regular_file(entry.data))
    return std::nullopt;
  try {
    const auto scan = ScanGribMessages(entry.data);
    if (scan.message_count != expected_messages ||
        scan.byte_count != (*metadata)["byteCount"].asUInt64())
      throw ValidationError("cached MET Norway GRIB validation failed");
    ReplaceWithCopy(entry.data, output);
    return std::pair<std::size_t, double>{
        (*metadata)["partialFieldCount"].asUInt64(),
        (*metadata)["maximumMissingPercent"].asDouble()};
  } catch (const std::exception&) {
    std::error_code ignored;
    std::filesystem::remove(entry.data, ignored);
    std::filesystem::remove(entry.metadata, ignored);
    return std::nullopt;
  }
}

void SaveCache(const MetNoCacheEntry& entry,
               const std::filesystem::path& output,
               std::size_t partial_field_count,
               double maximum_missing_percent) {
  std::filesystem::create_directories(entry.data.parent_path());
  auto data_temporary = entry.data;
  data_temporary += "." + std::to_string(ProcessId()) + ".tmp";
  auto metadata_temporary = entry.metadata;
  metadata_temporary += "." + std::to_string(ProcessId()) + ".tmp";
  try {
    std::filesystem::copy_file(
        output, data_temporary,
        std::filesystem::copy_options::overwrite_existing);
    const auto scan = ScanGribMessages(data_temporary);
    Json::Value metadata(Json::objectValue);
    metadata["schemaVersion"] = 1;
    metadata["fingerprint"] = entry.fingerprint;
    metadata["messageCount"] = Json::UInt64(scan.message_count);
    metadata["byteCount"] = Json::UInt64(scan.byte_count);
    metadata["partialFieldCount"] = Json::UInt64(partial_field_count);
    metadata["maximumMissingPercent"] = maximum_missing_percent;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    {
      std::ofstream stream(metadata_temporary,
                           std::ios::binary | std::ios::trunc);
      if (!stream)
        throw ValidationError("unable to create MET Norway cache metadata");
      stream << Json::writeString(builder, metadata) << '\n';
      if (!stream)
        throw ValidationError("unable to write MET Norway cache metadata");
    }
    std::error_code ignored;
    std::filesystem::remove(entry.data, ignored);
    std::filesystem::rename(data_temporary, entry.data);
    std::filesystem::remove(entry.metadata, ignored);
    std::filesystem::rename(metadata_temporary, entry.metadata);
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& item :
         std::filesystem::directory_iterator(entry.data.parent_path())) {
      std::error_code error;
      const auto age = now - item.last_write_time(error);
      if (!error && age > std::chrono::hours(8))
        std::filesystem::remove(item.path(), error);
    }
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(data_temporary, ignored);
    std::filesystem::remove(metadata_temporary, ignored);
    throw;
  }
}

std::string CompactDate(TimePoint instant) {
  auto value = FormatUtcDateTime(instant).substr(0, 10);
  value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
  return value;
}

}  // namespace

bool MetNoProjectionAvailable() {
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
  return true;
#else
  return false;
#endif
}

std::vector<int> MetNoForecastHours(int hours, int step_hours) {
  if (hours < 0 || hours > 56)
    throw ValidationError("MET Norway hours must be between 0 and 56");
  if (step_hours != 1 && step_hours != 3 && step_hours != 6 && step_hours != 12)
    throw ValidationError("MET Norway step-hours must be 1, 3, 6, or 12");
  if (hours % step_hours != 0)
    throw ValidationError("MET Norway hours must be divisible by step-hours");
  return ForecastHourSequence(hours, step_hours);
}

WeatherGenerateResult GenerateMetNoNordic(const MetNoRequest& request,
                                          ProgressCallback progress) {
  request.bbox.Validate();
  if (!kMetNoNordicDomain.Contains(request.bbox))
    throw ValidationError(
        "MET Norway bbox is outside the Nordic forecast domain");
  if (!MetNoProjectionAvailable())
    throw UnsupportedSourceError("MET Norway requires the native PROJ library");
  if (request.grid_spacing_deg <= 0.0)
    throw ValidationError("MET Norway output grid spacing must be positive");
  if (request.preset != "minimal" && request.preset != "routing" &&
      request.preset != "marine" && request.preset != "all")
    throw ValidationError(
        "weather-preset must be minimal, routing, marine, or all");
  if (std::filesystem::exists(request.output) && !request.overwrite)
    throw ValidationError("output already exists: " +
                          PathToUtf8(request.output));
  const auto requested_hours =
      MetNoForecastHours(request.hours, request.step_hours);
  if (request.dry_run) {
    return {"metno_nordic",
            "MET Norway Nordic 1 km forecast",
            "met_forecast_1_0km_nordic",
            {},
            request.bbox,
            requested_hours,
            request.output,
            0,
            0,
            Json::Value(Json::objectValue),
            {request.dataset_url},
            {{"weather_grid_spacing_deg",
              std::to_string(request.grid_spacing_deg)}}};
  }

  const auto total_started = std::chrono::steady_clock::now();
  if (progress) {
    Json::Value details;
    details["dataset"] = request.dataset_url;
    progress("opening MET Norway Nordic forecast", details);
  }
  std::lock_guard netcdf_lock(NetcdfApiMutex());
  NcFile file(request.dataset_url);
  const auto reference = ReadReferenceTime(file.id());
  const int time_variable = Variable(file.id(), "time");
  const auto time_values = Read1D(file.id(), time_variable);
  const auto time_units = AttributeText(file.id(), time_variable, "units");
  const double time_unit_seconds = TimeUnitSeconds(time_units);
  const auto time_origin = ParseUtcDateTime(CfTimeOrigin(time_units));
  std::map<int, std::size_t> time_indices;
  for (std::size_t i = 0; i < time_values.size(); ++i) {
    const auto valid =
        time_origin + std::chrono::seconds(static_cast<long long>(
                          std::llround(time_values[i] * time_unit_seconds)));
    const auto lead =
        std::chrono::duration_cast<std::chrono::hours>(valid - reference)
            .count();
    if (lead >= 0) time_indices[static_cast<int>(lead)] = i;
  }
  for (const int hour : requested_hours)
    if (!time_indices.contains(hour))
      throw ValidationError("MET Norway latest dataset lacks forecast hour " +
                            std::to_string(hour));

  std::filesystem::create_directories(request.output.parent_path().empty()
                                          ? "."
                                          : request.output.parent_path());
  const bool allow_strided_source_reads =
      !request.dataset_url.starts_with("http://") &&
      !request.dataset_url.starts_with("https://");
  const std::string source_read_strategy = allow_strided_source_reads
                                               ? "strided-local-batch"
                                               : "single-time-opendap";
  std::optional<MetNoCacheEntry> cache_entry;
  if (request.cache_directory) {
    cache_entry = CacheEntry(request, reference, requested_hours);
    const auto cache_started = std::chrono::steady_clock::now();
    if (const auto restored = RestoreCache(
            *cache_entry, request.output,
            ExpectedMessageCount(requested_hours, request.preset))) {
      const auto inspection_started = std::chrono::steady_clock::now();
      const auto inspection = InspectGrib(request.output);
      const double inspection_seconds = ElapsedSeconds(inspection_started);
      const double total_seconds = ElapsedSeconds(total_started);
      if (progress) {
        Json::Value details;
        details["messages"] = inspection["message_count"];
        details["cacheRestoreSeconds"] = ElapsedSeconds(cache_started);
        details["inspectionSeconds"] = inspection_seconds;
        details["totalSeconds"] = total_seconds;
        progress("reused validated MET Norway forecast cache", details);
      }
      const auto reference_text = FormatUtcDateTime(reference);
      return {
          "metno_nordic",
          "MET Norway Nordic 1 km forecast",
          "met_forecast_1_0km_nordic",
          GFSCycle{CompactDate(reference), reference_text.substr(11, 2)},
          request.bbox,
          requested_hours,
          request.output,
          std::filesystem::file_size(request.output),
          inspection["message_count"].asUInt64(),
          inspection,
          {request.dataset_url},
          {{"weather_grid_spacing_deg",
            std::to_string(request.grid_spacing_deg)},
           {"source_grid", "MET Norway postprocessed Nordic 1 km"},
           {"partial_field_count", std::to_string(restored->first)},
           {"maximum_missing_percent", std::to_string(restored->second)},
           {"source_read_strategy", source_read_strategy},
           {"cache_hit", "true"},
           {"cache_restore_seconds",
            std::to_string(ElapsedSeconds(cache_started))},
           {"timing_inspection_seconds", std::to_string(inspection_seconds)},
           {"timing_total_seconds", std::to_string(total_seconds)},
           {"licence",
            "CC BY 4.0 / Norwegian Licence for Open Government Data"}}};
    }
  }

  const auto source_x = Read1D(file.id(), Variable(file.id(), "x"));
  const auto source_y = Read1D(file.id(), Variable(file.id(), "y"));
  const auto projection_metadata = ReadProjection(file.id());
  const double metadata_seconds = ElapsedSeconds(total_started);
  std::size_t partial_field_count = 0;
  double maximum_missing_percent = 0.0;
  double plan_seconds = 0.0;
  double read_seconds = 0.0;
  double regrid_seconds = 0.0;
  double write_seconds = 0.0;
  double inspection_seconds = 0.0;
  std::size_t remote_read_requests = 0;
  std::size_t source_field_slices = 0;
  std::size_t output_field_count = 0;
  std::size_t covered_plan_cells = 0;
  std::size_t worker_count = 1;
  constexpr std::size_t kTimeBatchSize = 4;
  const auto grid = BuildRegularGrid(request.bbox, request.grid_spacing_deg);
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
  Projection projection(projection_metadata);
  std::vector<std::pair<double, double>> perimeter;
  for (int i = 0; i <= 8; ++i) {
    const double fraction = static_cast<double>(i) / 8.0;
    const double longitude =
        request.bbox.west + fraction * (request.bbox.east - request.bbox.west);
    const double latitude =
        request.bbox.south +
        fraction * (request.bbox.north - request.bbox.south);
    perimeter.push_back(projection.Forward(longitude, request.bbox.south));
    perimeter.push_back(projection.Forward(longitude, request.bbox.north));
    perimeter.push_back(projection.Forward(request.bbox.west, latitude));
    perimeter.push_back(projection.Forward(request.bbox.east, latitude));
  }
  const auto x_limits = std::minmax_element(
      perimeter.begin(), perimeter.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  const auto y_limits = std::minmax_element(
      perimeter.begin(), perimeter.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
  const auto x =
      SliceAxis(source_x, x_limits.first->first, x_limits.second->first);
  const auto y =
      SliceAxis(source_y, y_limits.first->second, y_limits.second->second);
  worker_count = RegridWorkerCount(grid.ny());
  const auto plan_started = std::chrono::steady_clock::now();
  const auto interpolation_plan =
      BuildInterpolationPlan(projection, grid, x, y);
  plan_seconds = ElapsedSeconds(plan_started);
  covered_plan_cells = static_cast<std::size_t>(
      std::count_if(interpolation_plan.begin(), interpolation_plan.end(),
                    [](const auto& point) { return point.covered; }));
  if (progress) {
    Json::Value details;
    details["gridCells"] = Json::UInt64(grid.size());
    details["coveredCells"] = Json::UInt64(covered_plan_cells);
    details["workers"] = Json::UInt64(worker_count);
    details["seconds"] = plan_seconds;
    progress("prepared reusable MET Norway regridding map", details);
  }

  bool reported_partial_coverage = false;
  const auto append =
      [&](std::vector<Grib2Field>& destination, const std::string& short_name,
          const SourceFieldBatch& source, std::size_t source_time,
          int forecast_hour, double scale = 1.0,
          std::optional<std::string> level_type = std::nullopt,
          std::optional<double> level = std::nullopt,
          std::optional<std::string> step_type = std::nullopt,
          int interval_hours = 0) {
        const auto regrid_started = std::chrono::steady_clock::now();
        auto [values, mask] =
            Regrid(source.Values(source_time), source.Mask(source_time),
                   interpolation_plan, grid, worker_count, scale);
        regrid_seconds += ElapsedSeconds(regrid_started);
        const auto missing = static_cast<std::size_t>(std::count_if(
            mask.begin(), mask.end(), [](auto value) { return value != 0; }));
        if (missing == grid.size())
          throw ValidationError("MET Norway field " + short_name +
                                " at forecast hour " +
                                std::to_string(forecast_hour) +
                                " has no coverage in the requested bbox");
        if (missing > 0) {
          ++partial_field_count;
          const double missing_percent = 100.0 * static_cast<double>(missing) /
                                         static_cast<double>(grid.size());
          maximum_missing_percent =
              std::max(maximum_missing_percent, missing_percent);
          if (progress && !reported_partial_coverage) {
            Json::Value details;
            details["field"] = short_name;
            details["hour"] = forecast_hour;
            details["missingCells"] = Json::UInt64(missing);
            details["gridCells"] = Json::UInt64(grid.size());
            details["missingPercent"] = missing_percent;
            progress("retaining partial MET Norway coverage with a GRIB bitmap",
                     details);
            reported_partial_coverage = true;
          }
        }
        destination.push_back({forecast_hour, short_name, std::move(values),
                               std::move(mask), std::move(level_type), level,
                               std::move(step_type), interval_hours});
      };

  const auto temporary_output = TemporaryMetNoOutput(request.output);
  bool first_chunk = true;
  try {
    for (std::size_t batch_begin = 0; batch_begin < requested_hours.size();
         batch_begin += kTimeBatchSize) {
      const std::size_t batch_end =
          std::min(requested_hours.size(), batch_begin + kTimeBatchSize);
      std::vector<std::size_t> positions(batch_end - batch_begin);
      std::iota(positions.begin(), positions.end(), batch_begin);
      std::vector<std::size_t> indices;
      indices.reserve(positions.size());
      for (const auto position : positions)
        indices.push_back(time_indices.at(requested_hours[position]));
      std::vector<std::vector<Grib2Field>> fields_by_time(positions.size());

      if (progress) {
        Json::Value details;
        details["firstHour"] = requested_hours[batch_begin];
        details["lastHour"] = requested_hours[batch_end - 1];
        details["completedHours"] = Json::UInt64(batch_begin);
        details["totalHours"] = Json::UInt64(requested_hours.size());
        details["sourceReadStrategy"] = source_read_strategy;
        progress("reading MET Norway forecast data", details);
      }
      const auto read = [&](const char* name,
                            const std::vector<std::size_t>& selected_indices) {
        const auto started = std::chrono::steady_clock::now();
        auto result = ReadFieldBatch(file.id(), name, selected_indices, x, y,
                                     allow_strided_source_reads);
        read_seconds += ElapsedSeconds(started);
        remote_read_requests += result.requests;
        source_field_slices += selected_indices.size();
        return result.field;
      };
      const auto append_scalar =
          [&](const char* source_name, const std::string& short_name,
              double scale = 1.0,
              std::optional<std::string> level_type = std::nullopt,
              std::optional<double> level = std::nullopt,
              std::optional<std::string> step_type = std::nullopt,
              int interval_hours = 0, bool positive_hours_only = false) {
            std::vector<std::size_t> selected_positions;
            std::vector<std::size_t> selected_indices;
            for (std::size_t local = 0; local < positions.size(); ++local) {
              const int hour = requested_hours[positions[local]];
              if (positive_hours_only && hour == 0) continue;
              selected_positions.push_back(local);
              selected_indices.push_back(indices[local]);
            }
            if (selected_indices.empty()) return;
            const auto source = read(source_name, selected_indices);
            for (std::size_t selected = 0; selected < selected_positions.size();
                 ++selected) {
              const std::size_t local = selected_positions[selected];
              const int hour = requested_hours[positions[local]];
              append(fields_by_time[local], short_name, source, selected, hour,
                     scale, level_type, level, step_type, interval_hours);
            }
          };

      auto speed = read("wind_speed_10m", indices);
      const auto direction = read("wind_direction_10m", indices);
      SourceFieldBatch u = std::move(speed);
      SourceFieldBatch v{&x, &y, u.time_count,
                         std::vector<double>(u.values.size()),
                         std::vector<std::uint8_t>(u.mask.size())};
      for (std::size_t i = 0; i < u.values.size(); ++i) {
        const bool missing = u.mask[i] || direction.mask[i];
        u.mask[i] = v.mask[i] = missing;
        if (!missing) {
          const double radians = direction.values[i] * std::numbers::pi / 180.0;
          const double wind_speed = u.values[i];
          u.values[i] = -wind_speed * std::sin(radians);
          v.values[i] = -wind_speed * std::cos(radians);
        }
      }
      for (std::size_t local = 0; local < positions.size(); ++local) {
        const int hour = requested_hours[positions[local]];
        append(fields_by_time[local], "10u", u, local, hour);
        append(fields_by_time[local], "10v", v, local, hour);
      }
      if (request.preset != "minimal") {
        append_scalar("air_pressure_at_sea_level", "prmsl");
        append_scalar("air_temperature_2m", "2t");
      }
      if (request.preset == "marine" || request.preset == "all") {
        append_scalar("wind_speed_of_gust", "gust", 1.0, std::string("surface"),
                      0.0);
        append_scalar("cloud_area_fraction", "tcc", 100.0,
                      std::string("entireAtmosphere"), 0.0);
        append_scalar("precipitation_amount", "tp", 1.0, std::string("surface"),
                      0.0, std::string("accum"), 1, true);
      }
      if (request.preset == "all")
        append_scalar("relative_humidity_2m", "r", 100.0,
                      std::string("heightAboveGround"), 2.0);

      std::vector<Grib2Field> chunk;
      for (auto& fields : fields_by_time)
        for (auto& field : fields) chunk.push_back(std::move(field));
      const auto write_started = std::chrono::steady_clock::now();
      const auto written = WriteRegularLatLonGrib2Chunk(
          grid, reference, chunk, temporary_output, !first_chunk);
      write_seconds += ElapsedSeconds(write_started);
      first_chunk = false;
      output_field_count += written.message_count;
      if (progress) {
        Json::Value details;
        details["completedHours"] = Json::UInt64(batch_end);
        details["totalHours"] = Json::UInt64(requested_hours.size());
        details["messagesWritten"] = Json::UInt64(output_field_count);
        details["readSeconds"] = read_seconds;
        details["regridSeconds"] = regrid_seconds;
        details["writeSeconds"] = write_seconds;
        progress("streamed MET Norway forecast batch", details);
      }
    };
    const auto scan = ScanGribMessages(temporary_output);
    if (scan.message_count != output_field_count)
      throw ValidationError("streamed MET Norway GRIB message count mismatch");
    std::error_code error;
    if (std::filesystem::exists(request.output)) {
      std::filesystem::remove(request.output, error);
      if (error)
        throw ValidationError("unable to replace MET Norway output: " +
                              error.message());
    }
    std::filesystem::rename(temporary_output, request.output, error);
    if (error)
      throw ValidationError("unable to install MET Norway output: " +
                            error.message());
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary_output, ignored);
    throw;
  }
#endif
  const auto inspection_started = std::chrono::steady_clock::now();
  const auto inspection = InspectGrib(request.output);
  inspection_seconds = ElapsedSeconds(inspection_started);
  bool cache_saved = false;
  if (cache_entry) {
    try {
      SaveCache(*cache_entry, request.output, partial_field_count,
                maximum_missing_percent);
      cache_saved = true;
    } catch (const std::exception& exception) {
      if (progress) {
        Json::Value details;
        details["warning"] = exception.what();
        progress("MET Norway cache save skipped", details);
      }
    }
  }
  const double total_seconds = ElapsedSeconds(total_started);
  if (progress) {
    Json::Value details;
    details["metadataSeconds"] = metadata_seconds;
    details["planSeconds"] = plan_seconds;
    details["remoteReadSeconds"] = read_seconds;
    details["regridSeconds"] = regrid_seconds;
    details["gribWriteSeconds"] = write_seconds;
    details["inspectionSeconds"] = inspection_seconds;
    details["totalSeconds"] = total_seconds;
    details["remoteReadRequests"] = Json::UInt64(remote_read_requests);
    details["sourceFieldSlices"] = Json::UInt64(source_field_slices);
    details["sourceReadStrategy"] = source_read_strategy;
    details["cacheSaved"] = cache_saved;
    progress("completed MET Norway conversion", details);
  }
  const auto reference_text = FormatUtcDateTime(reference);
  return {
      "metno_nordic",
      "MET Norway Nordic 1 km forecast",
      "met_forecast_1_0km_nordic",
      GFSCycle{CompactDate(reference), reference_text.substr(11, 2)},
      request.bbox,
      requested_hours,
      request.output,
      std::filesystem::file_size(request.output),
      inspection["message_count"].asUInt64(),
      inspection,
      {request.dataset_url},
      {{"weather_grid_spacing_deg", std::to_string(request.grid_spacing_deg)},
       {"source_grid", "MET Norway postprocessed Nordic 1 km"},
       {"partial_field_count", std::to_string(partial_field_count)},
       {"maximum_missing_percent", std::to_string(maximum_missing_percent)},
       {"regrid_plan_cells", std::to_string(grid.size())},
       {"regrid_plan_covered_cells", std::to_string(covered_plan_cells)},
       {"regrid_workers", std::to_string(worker_count)},
       {"time_batch_size", std::to_string(kTimeBatchSize)},
       {"source_read_strategy", source_read_strategy},
       {"remote_read_requests", std::to_string(remote_read_requests)},
       {"source_field_slices", std::to_string(source_field_slices)},
       {"cache_hit", "false"},
       {"cache_saved", cache_saved ? "true" : "false"},
       {"timing_metadata_seconds", std::to_string(metadata_seconds)},
       {"timing_plan_seconds", std::to_string(plan_seconds)},
       {"timing_remote_read_seconds", std::to_string(read_seconds)},
       {"timing_regrid_seconds", std::to_string(regrid_seconds)},
       {"timing_grib_write_seconds", std::to_string(write_seconds)},
       {"timing_inspection_seconds", std::to_string(inspection_seconds)},
       {"timing_total_seconds", std::to_string(total_seconds)},
       {"licence", "CC BY 4.0 / Norwegian Licence for Open Government Data"}}};
}

}  // namespace environmental_grib
