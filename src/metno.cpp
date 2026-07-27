#include "environmental_grib/metno.h"

#include <netcdf.h>
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
#include <proj.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <sstream>

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

struct SourceField {
  const AxisSlice* x{};
  const AxisSlice* y{};
  std::vector<double> values;
  std::vector<std::uint8_t> mask;
};

SourceField ReadField(int file, const char* name, std::size_t time_index,
                      const AxisSlice& x, const AxisSlice& y) {
  const int variable = Variable(file, name);
  const auto dimensions = Dims(file, variable);
  if (dimensions.size() != 3)
    throw ValidationError(std::string("MET Norway field ") + name +
                          " must use time/y/x dimensions");
  const std::array<std::size_t, 3> start{time_index, y.start, x.start};
  const std::array<std::size_t, 3> count{1, y.count, x.count};
  std::vector<double> values(x.count * y.count);
  Nc(nc_get_vara_double(file, variable, start.data(), count.data(),
                        values.data()),
     std::string("reading MET Norway field ") + name);
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
  return {&x, &y, std::move(values), std::move(mask)};
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

std::pair<double, bool> Interpolate(const SourceField& field, double x,
                                    double y) {
  const auto bx = Bracket(field.x->values, x);
  const auto by = Bracket(field.y->values, y);
  if (bx[0] < 0.0 || by[0] < 0.0) return {0.0, false};
  const auto x0 = static_cast<std::size_t>(bx[0]);
  const auto x1 = static_cast<std::size_t>(bx[1]);
  const auto y0 = static_cast<std::size_t>(by[0]);
  const auto y1 = static_cast<std::size_t>(by[1]);
  const std::array<std::size_t, 4> index{
      y0 * field.x->count + x0, y0 * field.x->count + x1,
      y1 * field.x->count + x0, y1 * field.x->count + x1};
  for (const auto i : index)
    if (field.mask[i]) return {0.0, false};
  const double lower =
      field.values[index[0]] * (1.0 - bx[2]) + field.values[index[1]] * bx[2];
  const double upper =
      field.values[index[2]] * (1.0 - bx[2]) + field.values[index[3]] * bx[2];
  return {lower * (1.0 - by[2]) + upper * by[2], true};
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

  const auto source_x = Read1D(file.id(), Variable(file.id(), "x"));
  const auto source_y = Read1D(file.id(), Variable(file.id(), "y"));
  const auto projection_metadata = ReadProjection(file.id());
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
  const auto grid = BuildRegularGrid(request.bbox, request.grid_spacing_deg);
  std::vector<Grib2Field> output_fields;
  const auto regrid = [&](const SourceField& field, double scale = 1.0) {
    std::pair<std::vector<double>, std::vector<std::uint8_t>> result{
        std::vector<double>(grid.size()),
        std::vector<std::uint8_t>(grid.size())};
    for (std::size_t row = 0; row < grid.ny(); ++row)
      for (std::size_t column = 0; column < grid.nx(); ++column) {
        const auto [projected_x, projected_y] =
            projection.Forward(grid.longitudes[column], grid.latitudes[row]);
        const auto interpolated = Interpolate(field, projected_x, projected_y);
        const auto index = row * grid.nx() + column;
        result.first[index] = interpolated.first * scale;
        result.second[index] = !interpolated.second;
      }
    const auto missing =
        std::count_if(result.second.begin(), result.second.end(),
                      [](auto value) { return value != 0; });
    if (100.0 * missing / grid.size() > 0.5)
      throw ValidationError(
          "MET Norway regridded field has more than 0.5% missing cells");
    return result;
  };
  for (const int hour : requested_hours) {
    if (progress) {
      Json::Value details;
      details["hour"] = hour;
      progress("converting MET Norway forecast hour", details);
    }
    const auto read = [&](const char* name) {
      return ReadField(file.id(), name, time_indices.at(hour), x, y);
    };
    const auto append = [&](const std::string& short_name,
                            const SourceField& source, double scale = 1.0,
                            std::optional<std::string> level_type =
                                std::nullopt,
                            std::optional<double> level = std::nullopt,
                            std::optional<std::string> step_type = std::nullopt,
                            int interval_hours = 0) {
      auto [values, mask] = regrid(source, scale);
      output_fields.push_back({hour, short_name, std::move(values),
                               std::move(mask), std::move(level_type), level,
                               std::move(step_type), interval_hours});
    };
    const auto speed = read("wind_speed_10m");
    const auto direction = read("wind_direction_10m");
    SourceField u = speed, v = speed;
    for (std::size_t i = 0; i < speed.values.size(); ++i) {
      const bool missing = speed.mask[i] || direction.mask[i];
      u.mask[i] = v.mask[i] = missing;
      if (!missing) {
        const double radians = direction.values[i] * std::numbers::pi / 180.0;
        u.values[i] = -speed.values[i] * std::sin(radians);
        v.values[i] = -speed.values[i] * std::cos(radians);
      }
    }
    append("10u", u);
    append("10v", v);
    if (request.preset != "minimal") {
      append("prmsl", read("air_pressure_at_sea_level"));
      append("2t", read("air_temperature_2m"));
    }
    if (request.preset == "marine" || request.preset == "all") {
      append("gust", read("wind_speed_of_gust"), 1.0, std::string("surface"),
             0.0);
      append("tcc", read("cloud_area_fraction"), 100.0,
             std::string("entireAtmosphere"), 0.0);
      if (hour > 0)
        append("tp", read("precipitation_amount"), 1.0, std::string("surface"),
               0.0, std::string("accum"), 1);
    }
    if (request.preset == "all")
      append("r", read("relative_humidity_2m"), 100.0,
             std::string("heightAboveGround"), 2.0);
  }
  WriteRegularLatLonGrib2(grid, reference, output_fields, request.output);
#endif
  const auto inspection = InspectGrib(request.output);
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
       {"licence", "CC BY 4.0 / Norwegian Licence for Open Government Data"}}};
}

}  // namespace environmental_grib
