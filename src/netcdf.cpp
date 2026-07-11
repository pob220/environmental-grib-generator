#include "environmental_grib/netcdf.h"

#include <netcdf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

#include "environmental_grib/error.h"

namespace environmental_grib {
namespace {

constexpr std::array<const char*, 7> kUCandidates{
    "u", "water_u", "u_velocity", "eastward_sea_water_velocity", "uo",
    "surface_eastward_sea_water_velocity", "barotropic_eastward_sea_water_velocity"};
constexpr std::array<const char*, 7> kVCandidates{
    "v", "water_v", "v_velocity", "northward_sea_water_velocity", "vo",
    "surface_northward_sea_water_velocity", "barotropic_northward_sea_water_velocity"};
constexpr std::array<const char*, 4> kLatCandidates{"latitude", "lat", "nav_lat", "y"};
constexpr std::array<const char*, 4> kLonCandidates{"longitude", "lon", "nav_lon", "x"};
constexpr std::array<const char*, 2> kTimeCandidates{"time", "datetime"};

void NcCheck(int status, const std::string& action) {
  if (status != NC_NOERR) throw ValidationError(action + ": " + nc_strerror(status));
}

class NcFile {
 public:
  explicit NcFile(const std::filesystem::path& path) {
    NcCheck(nc_open(path.c_str(), NC_NOWRITE, &id_), "opening NetCDF " + path.string());
  }
  ~NcFile() { if (id_ >= 0) nc_close(id_); }
  NcFile(const NcFile&) = delete;
  int id() const { return id_; }
 private:
  int id_{-1};
};

std::optional<int> FindVar(int file, const std::string& name) {
  int id = -1;
  return nc_inq_varid(file, name.c_str(), &id) == NC_NOERR ? std::optional<int>{id} : std::nullopt;
}

template <std::size_t N>
std::pair<int, std::string> DetectVar(int file, const std::optional<std::string>& explicit_name,
                                     const std::array<const char*, N>& candidates,
                                     const std::string& label) {
  if (explicit_name) {
    auto id = FindVar(file, *explicit_name);
    if (!id) throw ValidationError(label + " '" + *explicit_name + "' not found in NetCDF file");
    return {*id, *explicit_name};
  }
  for (const char* candidate : candidates) {
    if (auto id = FindVar(file, candidate)) return {*id, candidate};
  }
  throw ValidationError("could not auto-detect " + label + "; provide it explicitly");
}

std::string AttributeText(int file, int variable, const char* name) {
  std::size_t size = 0;
  if (nc_inq_attlen(file, variable, name, &size) != NC_NOERR) return {};
  std::string value(size, '\0');
  NcCheck(nc_get_att_text(file, variable, name, value.data()), std::string("reading attribute ") + name);
  while (!value.empty() && value.back() == '\0') value.pop_back();
  return value;
}

std::vector<int> VarDimensions(int file, int variable) {
  int count = 0;
  NcCheck(nc_inq_varndims(file, variable, &count), "reading variable dimensions");
  std::vector<int> dimensions(static_cast<std::size_t>(count));
  NcCheck(nc_inq_vardimid(file, variable, dimensions.data()), "reading variable dimension IDs");
  return dimensions;
}

std::size_t DimSize(int file, int dimension) {
  std::size_t size = 0;
  NcCheck(nc_inq_dimlen(file, dimension, &size), "reading dimension size");
  return size;
}

std::string DimName(int file, int dimension) {
  std::array<char, NC_MAX_NAME + 1> name{};
  NcCheck(nc_inq_dimname(file, dimension, name.data()), "reading dimension name");
  return name.data();
}

int SoleDimension(int file, int variable, const std::string& label) {
  auto dimensions = VarDimensions(file, variable);
  if (dimensions.size() != 1) throw ValidationError(label + " coordinate must be one-dimensional");
  return dimensions.front();
}

std::vector<double> ReadCoordinate(int file, int variable) {
  const int dimension = SoleDimension(file, variable, "spatial/time");
  std::vector<double> values(DimSize(file, dimension));
  NcCheck(nc_get_var_double(file, variable, values.data()), "reading coordinate values");
  return values;
}

struct Spec {
  int u{}, v{}, lat{}, lon{}, time{};
  int lat_dim{}, lon_dim{}, time_dim{};
  std::string u_name, v_name, lat_name, lon_name, time_name;
};

Spec DetectSpec(int file, const NetCDFOptions& options) {
  Spec spec;
  std::tie(spec.u, spec.u_name) = DetectVar(file, options.u_variable, kUCandidates, "u current variable");
  std::tie(spec.v, spec.v_name) = DetectVar(file, options.v_variable, kVCandidates, "v current variable");
  std::tie(spec.lat, spec.lat_name) = DetectVar(file, options.lat_variable, kLatCandidates, "latitude coordinate");
  std::tie(spec.lon, spec.lon_name) = DetectVar(file, options.lon_variable, kLonCandidates, "longitude coordinate");
  std::tie(spec.time, spec.time_name) = DetectVar(file, options.time_variable, kTimeCandidates, "time coordinate");
  spec.lat_dim = SoleDimension(file, spec.lat, "latitude");
  spec.lon_dim = SoleDimension(file, spec.lon, "longitude");
  spec.time_dim = SoleDimension(file, spec.time, "time");
  return spec;
}

std::pair<double, double> Range(const std::vector<double>& values) {
  auto [minimum, maximum] = std::minmax_element(values.begin(), values.end());
  if (minimum == values.end()) throw ValidationError("empty NetCDF coordinate");
  return {*minimum, *maximum};
}

double DisplayLongitude(double value, bool source_360) {
  if (!source_360) return value;
  double result = std::fmod(value + 180.0, 360.0);
  if (result < 0.0) result += 360.0;
  return result - 180.0;
}

double SourceLongitude(double value, bool source_360) {
  double result = source_360 ? std::fmod(value, 360.0) : value;
  if (source_360 && result < 0.0) result += 360.0;
  return result;
}

bool IsSource360(const std::vector<double>& longitudes) {
  const auto [minimum, maximum] = Range(longitudes);
  return minimum >= 0.0 && maximum > 180.0;
}

double RegularSpacing(const std::vector<double>& sorted, const std::string& label,
                      double tolerance) {
  if (sorted.size() < 2) throw ValidationError(label + " coordinate has too few points");
  std::vector<double> differences;
  differences.reserve(sorted.size() - 1);
  for (std::size_t i = 1; i < sorted.size(); ++i) differences.push_back(sorted[i] - sorted[i - 1]);
  auto median_values = differences;
  std::sort(median_values.begin(), median_values.end());
  const double spacing = median_values[median_values.size() / 2];
  double deviation = 0.0;
  for (double value : differences) deviation = std::max(deviation, std::abs(value - spacing));
  const double allowed = std::max(tolerance, std::abs(spacing) * tolerance);
  if (deviation > allowed) {
    throw ValidationError(label + " coordinate is not regular enough for GRIB output; omit source-grid mode or increase tolerance");
  }
  return spacing;
}

TimePoint ParseCfTimeOrigin(std::string value) {
  std::replace(value.begin(), value.end(), ' ', 'T');
  if (value.find('T') == std::string::npos) value += "T00:00:00";
  const auto t = value.find('T');
  if (value.size() == t + 6) value += ":00";
  if (value.back() != 'Z' && value.find('+', t) == std::string::npos) value += 'Z';
  return ParseUtcDateTime(value);
}

std::vector<TimePoint> ReadTimes(int file, const Spec& spec) {
  const auto values = ReadCoordinate(file, spec.time);
  const std::string units = AttributeText(file, spec.time, "units");
  const auto since = units.find(" since ");
  if (since == std::string::npos) throw ValidationError("NetCDF time units must use '<unit> since <UTC origin>'");
  const std::string unit = units.substr(0, since);
  const TimePoint origin = ParseCfTimeOrigin(units.substr(since + 7));
  double seconds = 0.0;
  if (unit == "seconds" || unit == "second" || unit == "sec" || unit == "s") seconds = 1.0;
  else if (unit == "minutes" || unit == "minute" || unit == "min") seconds = 60.0;
  else if (unit == "hours" || unit == "hour" || unit == "h") seconds = 3600.0;
  else if (unit == "days" || unit == "day" || unit == "d") seconds = 86400.0;
  else throw ValidationError("unsupported NetCDF time unit: " + unit);
  std::vector<TimePoint> times;
  times.reserve(values.size());
  for (double value : values) times.push_back(origin + std::chrono::seconds{static_cast<long long>(std::llround(value * seconds))});
  return times;
}

std::size_t SelectTime(const std::vector<TimePoint>& times, TimePoint target,
                       bool nearest) {
  if (times.empty()) throw ValidationError("NetCDF time coordinate is empty");
  const auto [minimum, maximum] = std::minmax_element(times.begin(), times.end());
  if (target < *minimum || target > *maximum) throw ValidationError("requested time is outside source time range");
  auto exact = std::find(times.begin(), times.end(), target);
  if (exact != times.end()) return static_cast<std::size_t>(exact - times.begin());
  if (!nearest) throw ValidationError("requested time is not available in NetCDF file; enable nearest-time selection");
  auto best = times.begin();
  for (auto it = times.begin() + 1; it != times.end(); ++it) {
    if (std::abs((*it - target).count()) < std::abs((*best - target).count())) best = it;
  }
  return static_cast<std::size_t>(best - times.begin());
}

std::size_t SelectDepth(int file, int variable, const Spec& spec,
                        const NetCDFOptions& options, int& depth_dim) {
  std::vector<int> extra;
  for (int dim : VarDimensions(file, variable)) {
    if (dim != spec.time_dim && dim != spec.lat_dim && dim != spec.lon_dim) extra.push_back(dim);
  }
  if (extra.empty()) { depth_dim = -1; return 0; }
  if (extra.size() > 1) throw ValidationError("unsupported multiple extra dimensions on current variable");
  depth_dim = extra.front();
  const std::size_t size = DimSize(file, depth_dim);
  if (options.depth_index && options.depth_value) throw ValidationError("use either depth-index or depth-value, not both");
  if (options.depth_index) {
    if (*options.depth_index < 0 || static_cast<std::size_t>(*options.depth_index) >= size) throw ValidationError("depth-index is outside available range");
    return static_cast<std::size_t>(*options.depth_index);
  }
  if (options.depth_value) {
    const auto name = DimName(file, depth_dim);
    auto coordinate = FindVar(file, name);
    if (!coordinate) throw ValidationError("cannot use depth-value because depth dimension has no coordinate values");
    auto values = ReadCoordinate(file, *coordinate);
    auto best = std::min_element(values.begin(), values.end(), [&](double a, double b) {
      return std::abs(a - *options.depth_value) < std::abs(b - *options.depth_value);
    });
    return static_cast<std::size_t>(best - values.begin());
  }
  if (size == 1) return 0;
  throw ValidationError("current variable has depth/extra dimension; provide depth-index or depth-value");
}

struct Field2D {
  std::vector<double> latitudes;
  std::vector<double> longitudes;
  std::vector<double> values;
  std::vector<std::uint8_t> mask;
  std::string units;
};

Field2D ReadField(int file, int variable, const Spec& spec,
                  std::size_t time_index, const NetCDFOptions& options) {
  const auto dims = VarDimensions(file, variable);
  std::vector<std::size_t> start(dims.size(), 0), count(dims.size(), 1);
  int depth_dim = -1;
  const std::size_t depth_index = SelectDepth(file, variable, spec, options, depth_dim);
  int lat_position = -1, lon_position = -1;
  for (std::size_t position = 0; position < dims.size(); ++position) {
    const int dim = dims[position];
    if (dim == spec.time_dim) start[position] = time_index;
    else if (dim == spec.lat_dim) { lat_position = static_cast<int>(position); count[position] = DimSize(file, dim); }
    else if (dim == spec.lon_dim) { lon_position = static_cast<int>(position); count[position] = DimSize(file, dim); }
    else if (dim == depth_dim) start[position] = depth_index;
    else throw ValidationError("unsupported dimension on current variable");
  }
  if (lat_position < 0 || lon_position < 0) throw ValidationError("current variable must have latitude and longitude dimensions");
  const auto latitudes_raw = ReadCoordinate(file, spec.lat);
  const auto longitudes_raw = ReadCoordinate(file, spec.lon);
  const std::size_t points = latitudes_raw.size() * longitudes_raw.size();
  std::vector<double> packed(points);
  NcCheck(nc_get_vara_double(file, variable, start.data(), count.data(), packed.data()), "reading current variable");

  double fill = std::numeric_limits<double>::quiet_NaN();
  bool have_fill = nc_get_att_double(file, variable, "_FillValue", &fill) == NC_NOERR;
  if (!have_fill) have_fill = nc_get_att_double(file, variable, "missing_value", &fill) == NC_NOERR;
  double scale = 1.0, offset = 0.0;
  nc_get_att_double(file, variable, "scale_factor", &scale);
  nc_get_att_double(file, variable, "add_offset", &offset);
  Field2D field{latitudes_raw, longitudes_raw, std::vector<double>(points), std::vector<std::uint8_t>(points), AttributeText(file, variable, "units")};
  for (std::size_t y = 0; y < field.latitudes.size(); ++y) {
    for (std::size_t x = 0; x < field.longitudes.size(); ++x) {
      std::vector<std::size_t> local(dims.size(), 0);
      local[static_cast<std::size_t>(lat_position)] = y;
      local[static_cast<std::size_t>(lon_position)] = x;
      std::size_t packed_index = 0, stride = 1;
      for (std::size_t position = dims.size(); position-- > 0;) {
        packed_index += local[position] * stride;
        stride *= count[position];
      }
      const double raw = packed[packed_index];
      const std::size_t index = y * field.longitudes.size() + x;
      field.mask[index] = std::isnan(raw) || (have_fill && raw == fill);
      field.values[index] = field.mask[index] ? 0.0 : raw * scale + offset;
    }
  }
  if (field.latitudes.front() > field.latitudes.back()) {
    std::reverse(field.latitudes.begin(), field.latitudes.end());
    for (std::size_t y = 0; y < field.latitudes.size() / 2; ++y) {
      const std::size_t other = field.latitudes.size() - 1 - y;
      for (std::size_t x = 0; x < field.longitudes.size(); ++x) {
        std::swap(field.values[y * field.longitudes.size() + x], field.values[other * field.longitudes.size() + x]);
        std::swap(field.mask[y * field.longitudes.size() + x], field.mask[other * field.longitudes.size() + x]);
      }
    }
  }
  if (field.longitudes.front() > field.longitudes.back()) {
    std::reverse(field.longitudes.begin(), field.longitudes.end());
    for (std::size_t y = 0; y < field.latitudes.size(); ++y) {
      for (std::size_t x = 0; x < field.longitudes.size() / 2; ++x) {
        const std::size_t other = field.longitudes.size() - 1 - x;
        std::swap(field.values[y * field.longitudes.size() + x], field.values[y * field.longitudes.size() + other]);
        std::swap(field.mask[y * field.longitudes.size() + x], field.mask[y * field.longitudes.size() + other]);
      }
    }
  }
  return field;
}

double UnitMultiplier(std::string units, const std::optional<std::string>& assumed,
                      const std::string& variable) {
  if (assumed) units = *assumed;
  units.erase(std::remove_if(units.begin(), units.end(), [](unsigned char c) { return std::isspace(c); }), units.end());
  std::transform(units.begin(), units.end(), units.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (units == "mps" || units == "m/s" || units == "ms-1" || units == "m.s-1" || units == "metersecond-1" || units == "metresecond-1") return 1.0;
  if (units == "cmps" || units == "cm/s" || units == "cms-1" || units == "cm.s-1" || units == "centimetersecond-1" || units == "centimetresecond-1") return 0.01;
  throw ValidationError("unknown units for '" + variable + "': '" + units + "'; provide assume-units mps or cmps");
}

struct Bracket { std::size_t low{}, high{}; double fraction{}; bool valid{}; };

Bracket FindBracket(const std::vector<double>& coordinates, double value) {
  if (value < coordinates.front() || value > coordinates.back()) return {};
  auto high = std::lower_bound(coordinates.begin(), coordinates.end(), value);
  if (high == coordinates.end()) return {coordinates.size() - 1, coordinates.size() - 1, 0.0, true};
  if (*high == value || high == coordinates.begin()) {
    const auto index = static_cast<std::size_t>(high - coordinates.begin());
    return {index, index, 0.0, true};
  }
  const auto hi = static_cast<std::size_t>(high - coordinates.begin());
  const auto lo = hi - 1;
  return {lo, hi, (value - coordinates[lo]) / (coordinates[hi] - coordinates[lo]), true};
}

std::pair<double, bool> Interpolate(const Field2D& field, double latitude,
                                    double longitude) {
  const auto y = FindBracket(field.latitudes, latitude);
  const auto x = FindBracket(field.longitudes, longitude);
  if (!x.valid || !y.valid) return {0.0, false};
  const std::size_t nx = field.longitudes.size();
  const std::array<std::size_t, 4> indices{y.low * nx + x.low, y.low * nx + x.high,
                                          y.high * nx + x.low, y.high * nx + x.high};
  for (auto index : indices) if (field.mask[index]) return {0.0, false};
  const double lower = field.values[indices[0]] * (1.0 - x.fraction) + field.values[indices[1]] * x.fraction;
  const double upper = field.values[indices[2]] * (1.0 - x.fraction) + field.values[indices[3]] * x.fraction;
  return {lower * (1.0 - y.fraction) + upper * y.fraction, true};
}

}  // namespace

NetCDFCurrentSource::NetCDFCurrentSource(std::filesystem::path path,
                                         NetCDFOptions options)
    : path_(std::move(path)), options_(std::move(options)) {
  if (!std::filesystem::is_regular_file(path_)) throw ValidationError("NetCDF file does not exist: " + path_.string());
}

BoundingBox NetCDFCurrentSource::SourceBounds() const {
  NcFile file(path_);
  const auto spec = DetectSpec(file.id(), options_);
  auto latitudes = ReadCoordinate(file.id(), spec.lat);
  auto longitudes = ReadCoordinate(file.id(), spec.lon);
  const bool source_360 = IsSource360(longitudes);
  for (double& value : longitudes) value = DisplayLongitude(value, source_360);
  const auto [south, north] = Range(latitudes);
  const auto [west, east] = Range(longitudes);
  return {west, south, east, north};
}

BoundingBox NetCDFCurrentSource::ClipBboxToSource(const BoundingBox& bbox) const {
  const auto source = SourceBounds();
  BoundingBox clipped{std::max(bbox.west, source.west), std::max(bbox.south, source.south),
                      std::min(bbox.east, source.east), std::min(bbox.north, source.north)};
  clipped.Validate();
  return clipped;
}

RegularGrid NetCDFCurrentSource::BuildSourceGrid(const BoundingBox& bbox) const {
  NcFile file(path_);
  const auto spec = DetectSpec(file.id(), options_);
  auto latitudes = ReadCoordinate(file.id(), spec.lat);
  auto longitudes = ReadCoordinate(file.id(), spec.lon);
  const bool source_360 = IsSource360(longitudes);
  for (double& value : longitudes) value = DisplayLongitude(value, source_360);
  std::erase_if(latitudes, [&](double value) { return value < bbox.south || value > bbox.north; });
  std::erase_if(longitudes, [&](double value) { return value < bbox.west || value > bbox.east; });
  std::sort(latitudes.begin(), latitudes.end());
  std::sort(longitudes.begin(), longitudes.end());
  if (latitudes.size() < 2 || longitudes.size() < 2) throw ValidationError("source grid selection must contain at least two latitude and longitude points");
  const double lat_spacing = RegularSpacing(latitudes, "latitude", options_.source_grid_regularity_tolerance);
  const double lon_spacing = RegularSpacing(longitudes, "longitude", options_.source_grid_regularity_tolerance);
  return {latitudes, longitudes, std::max(std::abs(lat_spacing), std::abs(lon_spacing)),
          std::abs(lat_spacing), std::abs(lon_spacing)};
}

CurrentGrid NetCDFCurrentSource::GetCurrentGrid(const BoundingBox& bbox,
                                                TimePoint time,
                                                const RegularGrid& grid) const {
  NcFile file(path_);
  const auto spec = DetectSpec(file.id(), options_);
  auto source_lats = ReadCoordinate(file.id(), spec.lat);
  auto source_lons = ReadCoordinate(file.id(), spec.lon);
  const bool source_360 = IsSource360(source_lons);
  auto display_lons = source_lons;
  for (double& value : display_lons) value = DisplayLongitude(value, source_360);
  const auto [lat_min, lat_max] = Range(source_lats);
  const auto [lon_min, lon_max] = Range(display_lons);
  if (bbox.south < lat_min - options_.coverage_tolerance_deg || bbox.north > lat_max + options_.coverage_tolerance_deg) throw ValidationError("requested bbox latitude range is outside source; use clipping or an inset bbox");
  if (bbox.west < lon_min - options_.coverage_tolerance_deg || bbox.east > lon_max + options_.coverage_tolerance_deg) throw ValidationError("requested bbox longitude range is outside source; use clipping or an inset bbox");
  const std::size_t time_index = SelectTime(ReadTimes(file.id(), spec), time, options_.nearest_time);
  auto u = ReadField(file.id(), spec.u, spec, time_index, options_);
  auto v = ReadField(file.id(), spec.v, spec, time_index, options_);
  const double u_multiplier = UnitMultiplier(u.units, options_.assume_units, spec.u_name);
  const double v_multiplier = UnitMultiplier(v.units, options_.assume_units, spec.v_name);
  CurrentGrid result{time, grid, std::vector<double>(grid.size()), std::vector<double>(grid.size()),
                     std::vector<std::uint8_t>(grid.size())};
  for (std::size_t y = 0; y < grid.ny(); ++y) {
    for (std::size_t x = 0; x < grid.nx(); ++x) {
      const double target_lon = SourceLongitude(grid.longitudes[x], source_360);
      const auto [u_value, u_valid] = Interpolate(u, grid.latitudes[y], target_lon);
      const auto [v_value, v_valid] = Interpolate(v, grid.latitudes[y], target_lon);
      const std::size_t index = y * grid.nx() + x;
      result.mask[index] = !(u_valid && v_valid);
      result.u_mps[index] = u_valid ? u_value * u_multiplier : 0.0;
      result.v_mps[index] = v_valid ? v_value * v_multiplier : 0.0;
    }
  }
  if (std::none_of(result.mask.begin(), result.mask.end(), [](auto value) { return value != 0; })) result.mask.clear();
  return result;
}

Json::Value NetCDFCurrentSource::Inspect() const { return InspectNetCDF(path_); }

Json::Value InspectNetCDF(const std::filesystem::path& path) {
  NcFile file(path);
  NetCDFOptions options;
  Json::Value result(Json::objectValue);
  result["path"] = path.string();
  int dimensions = 0, variables = 0;
  NcCheck(nc_inq(file.id(), &dimensions, &variables, nullptr, nullptr), "inspecting NetCDF");
  for (int id = 0; id < dimensions; ++id) result["dimensions"][DimName(file.id(), id)] = Json::UInt64(DimSize(file.id(), id));
  for (int id = 0; id < variables; ++id) {
    std::array<char, NC_MAX_NAME + 1> name{};
    NcCheck(nc_inq_varname(file.id(), id, name.data()), "reading variable name");
    result["variable_units"][name.data()] = AttributeText(file.id(), id, "units");
  }
  try {
    const auto spec = DetectSpec(file.id(), options);
    result["detected_u_variable"] = spec.u_name;
    result["detected_v_variable"] = spec.v_name;
    auto latitudes = ReadCoordinate(file.id(), spec.lat);
    auto longitudes = ReadCoordinate(file.id(), spec.lon);
    auto times = ReadTimes(file.id(), spec);
    const auto [lat_min, lat_max] = Range(latitudes);
    const auto [lon_min, lon_max] = Range(longitudes);
    result["latitude_range"].append(lat_min); result["latitude_range"].append(lat_max);
    result["longitude_range"].append(lon_min); result["longitude_range"].append(lon_max);
    result["time_range"].append(FormatUtcDateTime(times.front()));
    result["time_range"].append(FormatUtcDateTime(times.back()));
  } catch (const ValidationError& error) {
    result["detection_error"] = error.what();
  }
  return result;
}

std::vector<NetCDFScalarField> ReadNetCDFScalarFields(
    const std::filesystem::path& path, const BoundingBox& bbox,
    const std::vector<TimePoint>& times, const RegularGrid& grid,
    const std::map<std::string, std::vector<std::string>>& aliases) {
  NcFile file(path);
  const auto [lat_id, lat_name] = DetectVar(file.id(), std::nullopt, kLatCandidates, "latitude coordinate");
  const auto [lon_id, lon_name] = DetectVar(file.id(), std::nullopt, kLonCandidates, "longitude coordinate");
  const auto [time_id, time_name] = DetectVar(file.id(), std::nullopt, kTimeCandidates, "time coordinate");
  Spec spec;
  spec.lat = lat_id; spec.lon = lon_id; spec.time = time_id;
  spec.lat_name = lat_name; spec.lon_name = lon_name; spec.time_name = time_name;
  spec.lat_dim = SoleDimension(file.id(), lat_id, "latitude");
  spec.lon_dim = SoleDimension(file.id(), lon_id, "longitude");
  spec.time_dim = SoleDimension(file.id(), time_id, "time");
  const auto source_times = ReadTimes(file.id(), spec);
  auto source_lats = ReadCoordinate(file.id(), lat_id);
  auto source_lons = ReadCoordinate(file.id(), lon_id);
  const bool source_360 = IsSource360(source_lons);
  auto display_lons = source_lons;
  for (double& value : display_lons) value = DisplayLongitude(value, source_360);
  const auto [lat_min, lat_max] = Range(source_lats);
  const auto [lon_min, lon_max] = Range(display_lons);
  if (bbox.south < lat_min - 0.02 || bbox.north > lat_max + 0.02 ||
      bbox.west < lon_min - 0.02 || bbox.east > lon_max + 0.02) {
    throw ValidationError("requested scalar-field bbox is outside NetCDF coverage");
  }
  std::map<std::string, std::pair<int, std::string>> variables;
  for (const auto& [name, candidates] : aliases) {
    std::optional<std::pair<int, std::string>> found;
    for (const auto& candidate : candidates) {
      if (auto id = FindVar(file.id(), candidate)) { found = std::make_pair(*id, candidate); break; }
    }
    if (!found) throw ValidationError("NetCDF is missing required scalar variable for " + name);
    variables[name] = *found;
  }
  std::vector<NetCDFScalarField> result;
  NetCDFOptions options;
  options.depth_index = 0;
  for (const auto time : times) {
    const auto time_index = SelectTime(source_times, time, false);
    for (const auto& [name, variable] : variables) {
      auto field = ReadField(file.id(), variable.first, spec, time_index, options);
      NetCDFScalarField output{time, name, variable.second, field.units,
                               std::vector<double>(grid.size()),
                               std::vector<std::uint8_t>(grid.size())};
      for (std::size_t y = 0; y < grid.ny(); ++y) {
        for (std::size_t x = 0; x < grid.nx(); ++x) {
          const auto [value, valid] = Interpolate(
              field, grid.latitudes[y], SourceLongitude(grid.longitudes[x], source_360));
          const auto index = y * grid.nx() + x;
          output.values[index] = valid ? value : 0.0;
          output.mask[index] = !valid;
        }
      }
      if (std::none_of(output.mask.begin(), output.mask.end(), [](auto value) { return value != 0; })) output.mask.clear();
      result.push_back(std::move(output));
    }
  }
  return result;
}

}  // namespace environmental_grib
