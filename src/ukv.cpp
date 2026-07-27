#include "environmental_grib/ukv.h"

#include <netcdf.h>
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
#include <proj.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <sstream>

#include "environmental_grib/error.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/parallel.h"
#include "environmental_grib/platform.h"

namespace environmental_grib {
namespace {
constexpr const char* kEndpoint =
    "https://met-office-atmospheric-model-data.s3.eu-west-2.amazonaws.com/";
std::set<std::string> UkvTokensForPreset(const std::string& preset,
                                         int forecast_hour) {
  std::set<std::string> result;
  result.insert("wind_speed_at_10m");
  result.insert("wind_direction_at_10m");
  if (preset == "minimal") return result;
  result.insert("pressure_at_mean_sea_level");
  result.insert("temperature_at_screen_level");
  if (preset == "routing") return result;
  result.insert("wind_gust_at_10m");
  result.insert("cloud_amount_of_total_cloud");
  if (forecast_hour > 0)
    result.insert(forecast_hour <= 54 ? "precipitation_accumulation-PT01H"
                                      : "precipitation_accumulation-PT03H");
  if (preset == "marine") return result;
  if (preset != "all")
    throw ValidationError(
        "weather-preset must be minimal, routing, marine, or all");
  result.insert("CAPE_surface");
  result.insert("relative_humidity_at_screen_level");
  result.insert("wind_speed_on_pressure_levels");
  result.insert("wind_direction_on_pressure_levels");
  result.insert("temperature_on_pressure_levels");
  result.insert("relative_humidity_on_pressure_levels");
  result.insert("geopotential_height_on_pressure_levels");
  return result;
}

void Nc(int status, const std::string& action) {
  if (status != NC_NOERR)
    throw ValidationError(action + ": " + nc_strerror(status));
}
class NcFile {
public:
  explicit NcFile(const std::filesystem::path& path) {
    const auto utf8_path = PathToUtf8(path);
    Nc(nc_open(utf8_path.c_str(), NC_NOWRITE, &id_), "opening UKV NetCDF");
  }
  ~NcFile() {
    if (id_ >= 0) nc_close(id_);
  }
  int id() const { return id_; }

private:
  int id_{-1};
};
std::vector<int> Dims(int file, int variable) {
  int count = 0;
  Nc(nc_inq_varndims(file, variable, &count), "reading UKV dimensions");
  std::vector<int> result(static_cast<std::size_t>(count));
  Nc(nc_inq_vardimid(file, variable, result.data()),
     "reading UKV dimension ids");
  return result;
}
std::size_t DimSize(int file, int dim) {
  std::size_t size = 0;
  Nc(nc_inq_dimlen(file, dim, &size), "reading UKV dimension size");
  return size;
}
std::string AttributeText(int file, int variable, const char* name) {
  std::size_t size = 0;
  if (nc_inq_attlen(file, variable, name, &size) != NC_NOERR) return {};
  std::string value(size, '\0');
  Nc(nc_get_att_text(file, variable, name, value.data()),
     "reading UKV text attribute");
  return value;
}
double AttributeDouble(int file, int variable, const char* name,
                       double fallback) {
  double value = fallback;
  return nc_get_att_double(file, variable, name, &value) == NC_NOERR ? value
                                                                     : fallback;
}
int FindCoordinate(int file, const char* standard_name, const char* fallback) {
  int variables = 0;
  Nc(nc_inq_nvars(file, &variables), "reading UKV variable count");
  for (int id = 0; id < variables; ++id)
    if (AttributeText(file, id, "standard_name") == standard_name) return id;
  int id = -1;
  if (nc_inq_varid(file, fallback, &id) == NC_NOERR) return id;
  throw ValidationError(std::string("UKV coordinate not found: ") +
                        standard_name);
}
std::vector<double> Read1D(int file, int variable) {
  const auto dims = Dims(file, variable);
  if (dims.size() != 1)
    throw ValidationError("UKV projected coordinate must be one-dimensional");
  std::vector<double> result(DimSize(file, dims[0]));
  Nc(nc_get_var_double(file, variable, result.data()),
     "reading UKV projected coordinate");
  return result;
}

struct ProjectedField {
  std::vector<double> x, y, values;
  std::vector<std::uint8_t> mask;
  double lat0{}, lon0{}, false_easting{}, false_northing{}, semi_major{},
      semi_minor{};
};

ProjectedField ReadProjectedField(
    const std::filesystem::path& path,
    std::optional<double> pressure_level_pa = std::nullopt) {
  NcFile file(path);
  const int x_id = FindCoordinate(file.id(), "projection_x_coordinate",
                                  "projection_x_coordinate");
  const int y_id = FindCoordinate(file.id(), "projection_y_coordinate",
                                  "projection_y_coordinate");
  const auto x = Read1D(file.id(), x_id), y = Read1D(file.id(), y_id);
  int variables = 0;
  Nc(nc_inq_nvars(file.id(), &variables), "reading UKV variable count");
  int data = -1, mapping = -1;
  for (int id = 0; id < variables; ++id) {
    const auto grid_mapping = AttributeText(file.id(), id, "grid_mapping");
    const auto dims = Dims(file.id(), id);
    if (!grid_mapping.empty() && dims.size() >= 2) {
      data = id;
      Nc(nc_inq_varid(file.id(), grid_mapping.c_str(), &mapping),
         "finding UKV grid mapping");
      break;
    }
  }
  if (data < 0 || mapping < 0)
    throw ValidationError(
        "UKV source lacks a projected data variable/grid mapping");
  if (AttributeText(file.id(), mapping, "grid_mapping_name") !=
      "lambert_azimuthal_equal_area")
    throw ValidationError(
        "unsupported UKV projection; expected Lambert azimuthal equal area");
  const auto dims = Dims(file.id(), data);
  if (dims[dims.size() - 2] != Dims(file.id(), y_id)[0] ||
      dims[dims.size() - 1] != Dims(file.id(), x_id)[0])
    throw ValidationError("UKV data dimensions do not end in projected y/x");
  std::vector<std::size_t> start(dims.size(), 0), count(dims.size(), 1);
  if (pressure_level_pa) {
    bool selected = false;
    for (std::size_t dimension_index = 0;
         dimension_index + 2 < dims.size(); ++dimension_index) {
      const int dimension = dims[dimension_index];
      for (int id = 0; id < variables; ++id) {
        const auto coordinate_dims = Dims(file.id(), id);
        if (coordinate_dims.size() != 1 ||
            coordinate_dims.front() != dimension)
          continue;
        char variable_name[NC_MAX_NAME + 1]{};
        Nc(nc_inq_varname(file.id(), id, variable_name),
           "reading UKV coordinate name");
        const auto standard_name = AttributeText(file.id(), id, "standard_name");
        const auto units = AttributeText(file.id(), id, "units");
        if (standard_name != "air_pressure" &&
            std::string(variable_name).find("pressure") == std::string::npos &&
            units != "Pa")
          continue;
        std::vector<double> levels(DimSize(file.id(), dimension));
        Nc(nc_get_var_double(file.id(), id, levels.data()),
           "reading UKV pressure levels");
        const auto nearest = std::min_element(
            levels.begin(), levels.end(), [&](double lhs, double rhs) {
              return std::abs(lhs - *pressure_level_pa) <
                     std::abs(rhs - *pressure_level_pa);
            });
        if (nearest == levels.end() ||
            std::abs(*nearest - *pressure_level_pa) > 1.0)
          throw ValidationError("requested UKV pressure level is unavailable");
        start[dimension_index] =
            static_cast<std::size_t>(nearest - levels.begin());
        selected = true;
        break;
      }
      if (selected) break;
    }
    if (!selected)
      throw ValidationError("UKV pressure-level field lacks a pressure axis");
  }
  count[dims.size() - 2] = y.size();
  count[dims.size() - 1] = x.size();
  std::vector<double> values(x.size() * y.size());
  Nc(nc_get_vara_double(file.id(), data, start.data(), count.data(),
                        values.data()),
     "reading UKV data");
  double fill = std::numeric_limits<double>::quiet_NaN();
  const bool have_fill =
      nc_get_att_double(file.id(), data, "_FillValue", &fill) == NC_NOERR ||
      nc_get_att_double(file.id(), data, "missing_value", &fill) == NC_NOERR;
  const double scale = AttributeDouble(file.id(), data, "scale_factor", 1.0);
  const double offset = AttributeDouble(file.id(), data, "add_offset", 0.0);
  std::vector<std::uint8_t> mask(values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    mask[i] = !std::isfinite(values[i]) || (have_fill && values[i] == fill);
    if (!mask[i]) values[i] = values[i] * scale + offset;
  }
  return {x,
          y,
          values,
          mask,
          AttributeDouble(file.id(), mapping, "latitude_of_projection_origin",
                          54.9),
          AttributeDouble(file.id(), mapping, "longitude_of_projection_origin",
                          -2.5),
          AttributeDouble(file.id(), mapping, "false_easting", 0.0),
          AttributeDouble(file.id(), mapping, "false_northing", 0.0),
          AttributeDouble(file.id(), mapping, "semi_major_axis", 6378137.0),
          AttributeDouble(file.id(), mapping, "semi_minor_axis",
                          6356752.314245179)};
}

#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
std::pair<double, bool> Interpolate(const ProjectedField& field, double x,
                                    double y) {
  auto bracket = [](const std::vector<double>& axis, double value) {
    auto upper = std::lower_bound(axis.begin(), axis.end(), value);
    if (upper == axis.end() || (upper == axis.begin() && *upper != value))
      return std::array<double, 3>{-1, -1, 0};
    if (*upper == value) {
      const auto i = static_cast<double>(upper - axis.begin());
      return std::array<double, 3>{i, i, 0};
    }
    const auto hi = static_cast<std::size_t>(upper - axis.begin()), lo = hi - 1;
    return std::array<double, 3>{static_cast<double>(lo),
                                 static_cast<double>(hi),
                                 (value - axis[lo]) / (axis[hi] - axis[lo])};
  };
  const auto bx = bracket(field.x, x), by = bracket(field.y, y);
  if (bx[0] < 0 || by[0] < 0) return {0.0, false};
  const auto x0 = static_cast<std::size_t>(bx[0]),
             x1 = static_cast<std::size_t>(bx[1]);
  const auto y0 = static_cast<std::size_t>(by[0]),
             y1 = static_cast<std::size_t>(by[1]);
  const std::array<std::size_t, 4> i{
      y0 * field.x.size() + x0, y0 * field.x.size() + x1,
      y1 * field.x.size() + x0, y1 * field.x.size() + x1};
  for (auto index : i)
    if (field.mask[index]) return {0.0, false};
  const double lower =
      field.values[i[0]] * (1 - bx[2]) + field.values[i[1]] * bx[2];
  const double upper =
      field.values[i[2]] * (1 - bx[2]) + field.values[i[3]] * bx[2];
  return {lower * (1 - by[2]) + upper * by[2], true};
}

class Projection {
public:
  explicit Projection(const ProjectedField& field) {
    const std::array<double, 6> parameters{
        field.lat0,           field.lon0,       field.false_easting,
        field.false_northing, field.semi_major, field.semi_minor};
    if (!std::all_of(parameters.begin(), parameters.end(),
                     [](double value) { return std::isfinite(value); }) ||
        field.semi_major <= 0.0 || field.semi_minor <= 0.0)
      throw ValidationError("UKV source has invalid projection parameters");
    context_ = proj_context_create();
    if (!context_)
      throw ValidationError("PROJ could not create a UKV projection context");

    // Use an explicit degrees-to-LAEA pipeline rather than a CRS-to-CRS
    // operation.  The latter requires the external proj.db database and a
    // normalization step even though the UKV NetCDF files already provide
    // every projection parameter.  A self-contained pipeline is both more
    // deterministic and suitable for the capability-restricted helper.
    std::ostringstream pipeline;
    pipeline << "+proj=pipeline"
             << " +step +proj=unitconvert +xy_in=deg +xy_out=rad"
             << " +step +proj=laea +lat_0=" << field.lat0
             << " +lon_0=" << field.lon0 << " +x_0=" << field.false_easting
             << " +y_0=" << field.false_northing << " +a=" << field.semi_major
             << " +b=" << field.semi_minor << " +units=m +no_defs";
    projection_ = proj_create(context_, pipeline.str().c_str());
    if (!projection_) {
      const int error = proj_context_errno(context_);
      std::string message = "PROJ could not create UKV coordinate pipeline";
      if (const char* detail = proj_errno_string(error); detail && *detail)
        message += ": " + std::string(detail);
      proj_context_destroy(context_);
      context_ = nullptr;
      throw ValidationError(message);
    }
  }
  ~Projection() {
    if (projection_) proj_destroy(projection_);
    if (context_) proj_context_destroy(context_);
  }
  std::pair<double, double> Forward(double lon, double lat) const {
    proj_errno_reset(projection_);
    const auto coordinate =
        proj_trans(projection_, PJ_FWD, proj_coord(lon, lat, 0, 0));
    const int error = proj_errno(projection_);
    if (error) {
      std::string message = "PROJ failed UKV coordinate transformation";
      if (const char* detail = proj_errno_string(error); detail && *detail)
        message += ": " + std::string(detail);
      throw ValidationError(message);
    }
    return {coordinate.xy.x, coordinate.xy.y};
  }

private:
  PJ_CONTEXT* context_{};
  PJ* projection_{};
};
#endif

std::vector<std::string> Cycles(const UkvRequest& request, TimePoint now) {
  if (request.cycle != "auto") {
    if (!request.date)
      throw ValidationError("UKV explicit cycle requires date");
    return {*request.date + "T" + request.cycle + "00Z"};
  }
  const std::array<const char*, 8> cycles{"21", "18", "15", "12",
                                          "09", "06", "03", "00"};
  std::vector<std::string> result;
  for (int day = 0; day < std::max(1, request.max_auto_days); ++day) {
    auto date = FormatUtcDateTime(now - std::chrono::days(day)).substr(0, 10);
    date.erase(std::remove(date.begin(), date.end(), '-'), date.end());
    for (const char* cycle : cycles)
      result.push_back(date + "T" + cycle + "00Z");
  }
  return result;
}
}  // namespace

bool UkvProjectionAvailable() {
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
  return true;
#else
  return false;
#endif
}

std::vector<int> UkvForecastHours(int hours, int step) {
  if (hours < 0 || hours > 120)
    throw ValidationError("UKV hours must be between 0 and 120");
  if (step == 1) {
    std::vector<int> result;
    for (int hour = 0; hour <= std::min(hours, 54); ++hour)
      result.push_back(hour);
    for (int hour = 57; hour <= hours; hour += 3) result.push_back(hour);
    return result;
  }
  if (step == 3 && hours % 3 == 0) {
    std::vector<int> result;
    for (int hour = 0; hour <= hours; hour += 3) result.push_back(hour);
    return result;
  }
  throw ValidationError(
      "UKV step-hours must be 1 or 3, with 3-hour requests divisible");
}

std::string UkvSourceKey(const std::string& cycle, int hour,
                         const std::string& field) {
  const auto time =
      ParseUtcDateTime(cycle.substr(0, 4) + "-" + cycle.substr(4, 2) + "-" +
                       cycle.substr(6, 2) + "T" + cycle.substr(9, 2) +
                       ":00:00Z") +
      std::chrono::hours(hour);
  char lead[16];
  std::snprintf(lead, sizeof(lead), "PT%04dH00M", hour);
  auto valid = FormatUtcDateTime(time);
  valid.erase(std::remove(valid.begin(), valid.end(), '-'), valid.end());
  valid.erase(std::remove(valid.begin(), valid.end(), ':'), valid.end());
  return "uk-deterministic-2km/" + cycle + "/" + valid.substr(0, 13) + "Z-" +
         lead + "-" + field + ".nc";
}

WeatherGenerateResult GenerateUkv(const UkvRequest& request, HttpGet download,
                                  std::optional<TimePoint> now,
                                  ProgressCallback progress) {
  progress = SynchronizedProgressCallback(std::move(progress));
  request.bbox.Validate();
  if (!kUkvDomain.Contains(request.bbox))
    throw ValidationError("UKV bbox is outside UK/Ireland domain");
  if (!UkvProjectionAvailable())
    throw UnsupportedSourceError("UKV requires the native PROJ library");
  if (request.grid_spacing_deg <= 0.0)
    throw ValidationError("UKV grid spacing must be positive");
  if (std::filesystem::exists(request.output) && !request.overwrite)
    throw ValidationError("output already exists: " +
                          PathToUtf8(request.output));
  const auto hours = UkvForecastHours(request.hours, request.step_hours);
  const TimePoint current =
      now.value_or(std::chrono::time_point_cast<std::chrono::seconds>(
          std::chrono::system_clock::now()));
  const auto candidates = Cycles(request, current);
  if (request.dry_run) {
    return {"ukmo_ukv",
            "Met Office UKV 2 km forecast",
            "uk_deterministic_2km",
            GFSCycle{candidates.front().substr(0, 8),
                     candidates.front().substr(9, 2)},
            request.bbox,
            hours,
            request.output,
            0,
            0,
            Json::Value(Json::objectValue),
            {},
            {}};
  }
  if (!download) download = CurlHttpGet;
  std::string selected;
  std::map<std::pair<int, std::string>, std::filesystem::path> files;
  std::vector<std::string> urls;
  for (const auto& cycle : candidates) {
    files.clear();
    urls.clear();
    bool complete = true;
    struct FieldRequest {
      int hour{};
      std::string token;
      std::filesystem::path path;
    };
    struct DownloadedField {
      std::pair<int, std::string> key;
      std::filesystem::path path;
      std::string url;
    };
    std::vector<FieldRequest> requests;
    for (int hour : hours) {
      const auto tokens = UkvTokensForPreset(request.preset, hour);
      for (const auto& token : tokens) {
        auto path = std::filesystem::temp_directory_path() /
                    ("environmental-ukv-" + std::to_string(ProcessId()) + "-" +
                     std::to_string(hour) + "-" + token + ".nc");
        requests.push_back({hour, token, std::move(path)});
      }
    }
    try {
      auto downloaded = ParallelMapOrdered(
          requests, kDefaultDownloadConcurrency,
          [&](const FieldRequest& field_request) {
            const auto key =
                UkvSourceKey(cycle, field_request.hour, field_request.token);
            const auto url = std::string(kEndpoint) + key;
            Json::Value detail;
            detail["cycle"] = cycle;
            detail["hour"] = field_request.hour;
            detail["field"] = field_request.token;
            if (progress) progress("downloading Met Office UKV source", detail);
            const auto bytes = download(url, request.timeout_seconds);
            if (bytes.empty())
              throw ValidationError("UKV source download was empty");
            std::ofstream out(field_request.path,
                              std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
            if (!out) throw ValidationError("writing UKV source file failed");
            return DownloadedField{
                {field_request.hour, field_request.token},
                field_request.path,
                url};
          });
      for (auto& item : downloaded) {
        files[item.key] = std::move(item.path);
        urls.push_back(std::move(item.url));
      }
    } catch (...) {
      complete = false;
      for (const auto& item : requests) {
        std::error_code ignored;
        std::filesystem::remove(item.path, ignored);
      }
      if (request.cycle != "auto") throw;
    }
    if (complete) {
      selected = cycle;
      break;
    }
  }
  if (selected.empty())
    throw ValidationError("no complete UKV cycle was available");
  std::lock_guard netcdf_lock(NetcdfApiMutex());
  const auto grid = BuildRegularGrid(request.bbox, request.grid_spacing_deg);
  std::vector<Grib2Field> output_fields;
  for (int hour : hours) {
    const auto source = [&](const std::string& token,
                            std::optional<double> pressure = std::nullopt) {
      return ReadProjectedField(files.at({hour, token}), pressure);
    };
    const auto speed = source("wind_speed_at_10m");
    const auto direction = source("wind_direction_at_10m");
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
    Projection projection(speed);
    const auto regrid = [&](const ProjectedField& field,
                            double scale = 1.0) {
      std::pair<std::vector<double>, std::vector<std::uint8_t>> result{
          std::vector<double>(grid.size()),
          std::vector<std::uint8_t>(grid.size())};
      for (std::size_t y = 0; y < grid.ny(); ++y)
        for (std::size_t x = 0; x < grid.nx(); ++x) {
          const auto [px, py] =
              projection.Forward(grid.longitudes[x], grid.latitudes[y]);
          const auto value = Interpolate(field, px, py);
          const auto index = y * grid.nx() + x;
          result.first[index] = value.first * scale;
          result.second[index] = !value.second;
        }
      const auto missing =
          std::count_if(result.second.begin(), result.second.end(),
                        [](auto value) { return value != 0; });
      if (100.0 * missing / grid.size() > 0.5)
        throw ValidationError(
            "UKV regridded field has more than 0.5% missing cells");
      return result;
    };
    const auto append = [&](const std::string& short_name,
                            const ProjectedField& field, double scale = 1.0,
                            std::optional<std::string> level_type = std::nullopt,
                            std::optional<double> level = std::nullopt,
                            std::optional<std::string> step_type = std::nullopt,
                            int interval_hours = 0) {
      auto [values, mask] = regrid(field, scale);
      Grib2Field output;
      output.forecast_hour = hour;
      output.short_name = short_name;
      output.values = std::move(values);
      output.mask = std::move(mask);
      output.type_of_level = std::move(level_type);
      output.level = level;
      output.step_type = std::move(step_type);
      output.interval_hours = interval_hours;
      output_fields.push_back(std::move(output));
    };
    const auto append_wind =
        [&](const ProjectedField& wind_speed,
            const ProjectedField& wind_direction,
            const std::string& u_name, const std::string& v_name,
            std::optional<std::string> level_type = std::nullopt,
            std::optional<double> level = std::nullopt) {
          ProjectedField source_u = wind_speed, source_v = wind_speed;
          for (std::size_t i = 0; i < wind_speed.values.size(); ++i) {
            const bool missing =
                wind_speed.mask[i] || wind_direction.mask[i];
            source_u.mask[i] = source_v.mask[i] = missing;
            if (!missing) {
              const double radians =
                  wind_direction.values[i] * std::numbers::pi / 180.0;
              source_u.values[i] =
                  -wind_speed.values[i] * std::sin(radians);
              source_v.values[i] =
                  -wind_speed.values[i] * std::cos(radians);
            }
          }
          append(u_name, source_u, 1.0, level_type, level);
          append(v_name, source_v, 1.0, level_type, level);
        };
    append_wind(speed, direction, "10u", "10v");
    if (request.preset != "minimal") {
      append("prmsl", source("pressure_at_mean_sea_level"));
      append("2t", source("temperature_at_screen_level"));
    }
    if (request.preset == "marine" || request.preset == "all") {
      append("gust", source("wind_gust_at_10m"), 1.0,
             std::string("surface"), 0.0);
      append("tcc", source("cloud_amount_of_total_cloud"), 100.0,
             std::string("entireAtmosphere"), 0.0);
      if (hour > 0) {
        const int interval = hour <= 54 ? 1 : 3;
        const std::string token =
            interval == 1 ? "precipitation_accumulation-PT01H"
                          : "precipitation_accumulation-PT03H";
        append("tp", source(token), 1000.0, std::string("surface"), 0.0,
               std::string("accum"), interval);
      }
    }
    if (request.preset == "all") {
      append("cape", source("CAPE_surface"), 1.0, std::string("surface"), 0.0);
      append("r", source("relative_humidity_at_screen_level"), 100.0,
             std::string("heightAboveGround"), 2.0);
      for (const double pressure_pa :
           {85000.0, 70000.0, 50000.0, 30000.0}) {
        const double pressure_hpa = pressure_pa / 100.0;
        const auto upper_speed =
            source("wind_speed_on_pressure_levels", pressure_pa);
        const auto upper_direction =
            source("wind_direction_on_pressure_levels", pressure_pa);
        append_wind(upper_speed, upper_direction, "u", "v",
                    std::string("isobaricInhPa"), pressure_hpa);
        append("t", source("temperature_on_pressure_levels", pressure_pa), 1.0,
               std::string("isobaricInhPa"), pressure_hpa);
        append("r",
               source("relative_humidity_on_pressure_levels", pressure_pa),
               100.0, std::string("isobaricInhPa"), pressure_hpa);
        append("gh",
               source("geopotential_height_on_pressure_levels", pressure_pa),
               1.0, std::string("isobaricInhPa"), pressure_hpa);
      }
    }
#endif
  }
  const auto cycle_time = ParseUtcDateTime(
      selected.substr(0, 4) + "-" + selected.substr(4, 2) + "-" +
      selected.substr(6, 2) + "T" + selected.substr(9, 2) + ":00:00Z");
  WriteRegularLatLonGrib2(grid, cycle_time, output_fields, request.output);
  for (const auto& [key, path] : files) {
    (void)key;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
  const auto inspection = InspectGrib(request.output);
  return {
      "ukmo_ukv",
      "Met Office UKV 2 km forecast",
      "uk_deterministic_2km",
      GFSCycle{selected.substr(0, 8), selected.substr(9, 2)},
      request.bbox,
      hours,
      request.output,
      std::filesystem::file_size(request.output),
      inspection["message_count"].asUInt64(),
      inspection,
      urls,
      {{"weather_grid_spacing_deg", std::to_string(request.grid_spacing_deg)}}};
}

}  // namespace environmental_grib
