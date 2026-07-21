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
#include <sstream>

#include "environmental_grib/error.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/platform.h"

namespace environmental_grib {
namespace {
constexpr const char* kEndpoint =
    "https://met-office-atmospheric-model-data.s3.eu-west-2.amazonaws.com/";
const std::map<std::string, std::string> kFields{
    {"prmsl", "pressure_at_mean_sea_level"},
    {"2t", "temperature_at_screen_level"},
    {"speed", "wind_speed_at_10m"},
    {"direction", "wind_direction_at_10m"},
};

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

ProjectedField ReadProjectedField(const std::filesystem::path& path) {
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
    context_ = proj_context_create();
    std::ostringstream destination;
    destination << "+proj=laea +lat_0=" << field.lat0
                << " +lon_0=" << field.lon0 << " +x_0=" << field.false_easting
                << " +y_0=" << field.false_northing
                << " +a=" << field.semi_major << " +b=" << field.semi_minor
                << " +units=m +no_defs +type=crs";
    PJ* raw = proj_create_crs_to_crs(context_, "EPSG:4326",
                                     destination.str().c_str(), nullptr);
    projection_ =
        raw ? proj_normalize_for_visualization(context_, raw) : nullptr;
    if (raw) proj_destroy(raw);
    if (!projection_)
      throw ValidationError(
          "PROJ could not create UKV coordinate transformation");
  }
  ~Projection() {
    if (projection_) proj_destroy(projection_);
    if (context_) proj_context_destroy(context_);
  }
  std::pair<double, double> Forward(double lon, double lat) const {
    const auto coordinate =
        proj_trans(projection_, PJ_FWD, proj_coord(lon, lat, 0, 0));
    if (proj_errno(projection_))
      throw ValidationError("PROJ failed UKV coordinate transformation");
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
  request.bbox.Validate();
  if (!kUkvDomain.Contains(request.bbox))
    throw ValidationError("UKV bbox is outside UK/Ireland domain");
  if (!UkvProjectionAvailable())
    throw UnsupportedSourceError("UKV requires the native PROJ library");
  if (request.grid_spacing_deg <= 0.0)
    throw ValidationError("UKV grid spacing must be positive");
  if (std::filesystem::exists(request.output) && !request.overwrite)
    throw ValidationError("output already exists: " + PathToUtf8(request.output));
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
    try {
      for (int hour : hours)
        for (const auto& [short_name, token] : kFields) {
          const auto key = UkvSourceKey(cycle, hour, token);
          const auto url = std::string(kEndpoint) + key;
          Json::Value detail;
          detail["cycle"] = cycle;
          detail["hour"] = hour;
          detail["field"] = token;
          if (progress) progress("downloading Met Office UKV source", detail);
          const auto bytes = download(url, request.timeout_seconds);
          if (bytes.empty())
            throw ValidationError("UKV source download was empty");
          auto path = std::filesystem::temp_directory_path() /
                      ("environmental-ukv-" + std::to_string(ProcessId()) +
                       "-" + std::to_string(hour) + "-" + short_name + ".nc");
          std::ofstream out(path, std::ios::binary | std::ios::trunc);
          out.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
          if (!out) throw ValidationError("writing UKV source file failed");
          files[{hour, short_name}] = path;
          urls.push_back(url);
        }
    } catch (...) {
      complete = false;
      for (const auto& [key, path] : files) {
        (void)key;
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
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
  const auto grid = BuildRegularGrid(request.bbox, request.grid_spacing_deg);
  std::vector<Grib2Field> output_fields;
  for (int hour : hours) {
    const auto pressure = ReadProjectedField(files.at({hour, "prmsl"}));
    const auto temperature = ReadProjectedField(files.at({hour, "2t"}));
    const auto speed = ReadProjectedField(files.at({hour, "speed"}));
    const auto direction = ReadProjectedField(files.at({hour, "direction"}));
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
    Projection projection(pressure);
    ProjectedField source_u = speed, source_v = speed;
    for (std::size_t i = 0; i < speed.values.size(); ++i) {
      const bool missing = speed.mask[i] || direction.mask[i];
      source_u.mask[i] = source_v.mask[i] = missing;
      if (!missing) {
        const double radians = direction.values[i] * std::numbers::pi / 180.0;
        source_u.values[i] = -speed.values[i] * std::sin(radians);
        source_v.values[i] = -speed.values[i] * std::cos(radians);
      }
    }
    std::map<std::string, std::vector<double>> values;
    std::map<std::string, std::vector<std::uint8_t>> masks;
    for (const auto& name : {"prmsl", "2t", "10u", "10v"}) {
      values[name].resize(grid.size());
      masks[name].resize(grid.size());
    }
    for (std::size_t y = 0; y < grid.ny(); ++y)
      for (std::size_t x = 0; x < grid.nx(); ++x) {
        const auto [px, py] =
            projection.Forward(grid.longitudes[x], grid.latitudes[y]);
        const auto p = Interpolate(pressure, px, py),
                   t = Interpolate(temperature, px, py);
        const auto u = Interpolate(source_u, px, py),
                   v = Interpolate(source_v, px, py);
        const auto index = y * grid.nx() + x;
        values["prmsl"][index] = p.first;
        values["2t"][index] = t.first;
        values["10u"][index] = u.first;
        values["10v"][index] = v.first;
        masks["prmsl"][index] = !p.second;
        masks["2t"][index] = !t.second;
        masks["10u"][index] = masks["10v"][index] = !(u.second && v.second);
      }
    for (const auto& name : {"10u", "10v", "prmsl", "2t"}) {
      const auto missing = std::count_if(masks[name].begin(), masks[name].end(),
                                         [](auto value) { return value != 0; });
      if (100.0 * missing / grid.size() > 0.5)
        throw ValidationError(
            "UKV regridded field has more than 0.5% missing cells");
      output_fields.push_back(
          {hour, name, std::move(values[name]), std::move(masks[name])});
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
