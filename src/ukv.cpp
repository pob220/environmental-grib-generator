#include "environmental_grib/ukv.h"

#include <netcdf.h>
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
#include <proj.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <set>
#include <span>
#include <sstream>
#include <thread>

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
    for (std::size_t dimension_index = 0; dimension_index + 2 < dims.size();
         ++dimension_index) {
      const int dimension = dims[dimension_index];
      for (int id = 0; id < variables; ++id) {
        const auto coordinate_dims = Dims(file.id(), id);
        if (coordinate_dims.size() != 1 || coordinate_dims.front() != dimension)
          continue;
        char variable_name[NC_MAX_NAME + 1]{};
        Nc(nc_inq_varname(file.id(), id, variable_name),
           "reading UKV coordinate name");
        const auto standard_name =
            AttributeText(file.id(), id, "standard_name");
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
std::array<double, 3> Bracket(const std::vector<double>& axis, double value) {
  auto upper = std::lower_bound(axis.begin(), axis.end(), value);
  if (upper == axis.end() || (upper == axis.begin() && *upper != value))
    return std::array<double, 3>{-1, -1, 0};
  if (*upper == value) {
    const auto i = static_cast<double>(upper - axis.begin());
    return std::array<double, 3>{i, i, 0};
  }
  const auto hi = static_cast<std::size_t>(upper - axis.begin()), lo = hi - 1;
  return std::array<double, 3>{static_cast<double>(lo), static_cast<double>(hi),
                               (value - axis[lo]) / (axis[hi] - axis[lo])};
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

struct UkvInterpolationPoint {
  std::array<std::size_t, 4> index{};
  double x_weight{};
  double y_weight{};
  bool covered{};
};

struct UkvInterpolationPlan {
  std::vector<double> x;
  std::vector<double> y;
  std::array<double, 6> projection_parameters{};
  std::vector<UkvInterpolationPoint> points;
};

std::array<double, 6> ProjectionParameters(const ProjectedField& field) {
  return {field.lat0,           field.lon0,       field.false_easting,
          field.false_northing, field.semi_major, field.semi_minor};
}

bool SameGeometry(const UkvInterpolationPlan& plan,
                  const ProjectedField& field) {
  return plan.x == field.x && plan.y == field.y &&
         plan.projection_parameters == ProjectionParameters(field);
}

UkvInterpolationPlan BuildInterpolationPlan(const ProjectedField& field,
                                            const RegularGrid& grid) {
  Projection projection(field);
  UkvInterpolationPlan plan{field.x, field.y, ProjectionParameters(field),
                            std::vector<UkvInterpolationPoint>(grid.size())};
  for (std::size_t row = 0; row < grid.ny(); ++row)
    for (std::size_t column = 0; column < grid.nx(); ++column) {
      const auto [projected_x, projected_y] =
          projection.Forward(grid.longitudes[column], grid.latitudes[row]);
      const auto bx = Bracket(field.x, projected_x);
      const auto by = Bracket(field.y, projected_y);
      auto& point = plan.points[row * grid.nx() + column];
      if (bx[0] < 0.0 || by[0] < 0.0) continue;
      const auto x0 = static_cast<std::size_t>(bx[0]);
      const auto x1 = static_cast<std::size_t>(bx[1]);
      const auto y0 = static_cast<std::size_t>(by[0]);
      const auto y1 = static_cast<std::size_t>(by[1]);
      point.index = {y0 * field.x.size() + x0, y0 * field.x.size() + x1,
                     y1 * field.x.size() + x0, y1 * field.x.size() + x1};
      point.x_weight = bx[2];
      point.y_weight = by[2];
      point.covered = true;
    }
  return plan;
}

std::pair<double, bool> Interpolate(std::span<const double> values,
                                    std::span<const std::uint8_t> mask,
                                    const UkvInterpolationPoint& point) {
  if (!point.covered) return {0.0, false};
  for (const auto index : point.index)
    if (mask[index]) return {0.0, false};
  const double lower = values[point.index[0]] * (1.0 - point.x_weight) +
                       values[point.index[1]] * point.x_weight;
  const double upper = values[point.index[2]] * (1.0 - point.x_weight) +
                       values[point.index[3]] * point.x_weight;
  return {lower * (1.0 - point.y_weight) + upper * point.y_weight, true};
}

std::pair<std::vector<double>, std::vector<std::uint8_t>> Regrid(
    const ProjectedField& field, const UkvInterpolationPlan& plan,
    const RegularGrid& grid, std::size_t workers, double scale) {
  std::pair<std::vector<double>, std::vector<std::uint8_t>> result{
      std::vector<double>(grid.size()), std::vector<std::uint8_t>(grid.size())};
  std::vector<std::size_t> rows(grid.ny());
  std::iota(rows.begin(), rows.end(), 0);
  ParallelMapOrdered(rows, workers, [&](const std::size_t& row) {
    const std::size_t first = row * grid.nx();
    for (std::size_t column = 0; column < grid.nx(); ++column) {
      const std::size_t index = first + column;
      const auto value =
          Interpolate(field.values, field.mask, plan.points[index]);
      result.first[index] = value.first * scale;
      result.second[index] = !value.second;
    }
    return true;
  });
  return result;
}
#endif

std::size_t RegridWorkerCount(std::size_t rows) {
  const std::size_t detected =
      std::max<std::size_t>(1, std::thread::hardware_concurrency());
  return std::min<std::size_t>({rows, detected, 8});
}

double ElapsedSeconds(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
}

std::filesystem::path TemporaryUkvOutput(const std::filesystem::path& output) {
  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    auto candidate = output;
    candidate += "." + std::to_string(ProcessId()) + "." +
                 std::to_string(attempt) + ".ukv.tmp";
    if (!std::filesystem::exists(candidate)) return candidate;
  }
  throw ValidationError("unable to allocate temporary UKV output");
}

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
  const auto total_started = std::chrono::steady_clock::now();
  const auto download_started = std::chrono::steady_clock::now();
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
            return DownloadedField{{field_request.hour, field_request.token},
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
  const double download_seconds = ElapsedSeconds(download_started);
  std::lock_guard netcdf_lock(NetcdfApiMutex());
  const auto grid = BuildRegularGrid(request.bbox, request.grid_spacing_deg);
  std::size_t partial_field_count = 0;
  double maximum_missing_percent = 0.0;
  double plan_seconds = 0.0;
  double read_seconds = 0.0;
  double regrid_seconds = 0.0;
  double write_seconds = 0.0;
  double inspection_seconds = 0.0;
  std::size_t output_field_count = 0;
  const std::size_t worker_count = RegridWorkerCount(grid.ny());
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
  std::vector<UkvInterpolationPlan> interpolation_plans;
#endif
  const auto cycle_time = ParseUtcDateTime(
      selected.substr(0, 4) + "-" + selected.substr(4, 2) + "-" +
      selected.substr(6, 2) + "T" + selected.substr(9, 2) + ":00:00Z");
  std::filesystem::create_directories(request.output.parent_path().empty()
                                          ? "."
                                          : request.output.parent_path());
  const auto temporary_output = TemporaryUkvOutput(request.output);
  bool first_chunk = true;
  bool reported_partial_coverage = false;
  try {
    for (int hour : hours) {
      std::vector<Grib2Field> output_fields;
      const auto source = [&](const std::string& token,
                              std::optional<double> pressure = std::nullopt) {
        const auto started = std::chrono::steady_clock::now();
        auto result = ReadProjectedField(files.at({hour, token}), pressure);
        read_seconds += ElapsedSeconds(started);
        return result;
      };
      const auto speed = source("wind_speed_at_10m");
      const auto direction = source("wind_direction_at_10m");
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
      const auto plan_for =
          [&](const ProjectedField& field) -> const UkvInterpolationPlan& {
        const auto existing = std::find_if(
            interpolation_plans.begin(), interpolation_plans.end(),
            [&](const auto& plan) { return SameGeometry(plan, field); });
        if (existing != interpolation_plans.end()) return *existing;
        const auto started = std::chrono::steady_clock::now();
        interpolation_plans.push_back(BuildInterpolationPlan(field, grid));
        plan_seconds += ElapsedSeconds(started);
        if (progress) {
          Json::Value details;
          details["gridCells"] = Json::UInt64(grid.size());
          details["planCount"] = Json::UInt64(interpolation_plans.size());
          details["workers"] = Json::UInt64(worker_count);
          details["seconds"] = plan_seconds;
          progress("prepared reusable UKV regridding map", details);
        }
        return interpolation_plans.back();
      };
      const auto regrid = [&](const ProjectedField& field,
                              const std::string& short_name,
                              double scale = 1.0) {
        const auto started = std::chrono::steady_clock::now();
        auto result = Regrid(field, plan_for(field), grid, worker_count, scale);
        regrid_seconds += ElapsedSeconds(started);
        const auto missing = static_cast<std::size_t>(
            std::count_if(result.second.begin(), result.second.end(),
                          [](auto value) { return value != 0; }));
        if (missing == grid.size())
          throw ValidationError("UKV field " + short_name +
                                " at forecast hour " + std::to_string(hour) +
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
            details["hour"] = hour;
            details["missingCells"] = Json::UInt64(missing);
            details["gridCells"] = Json::UInt64(grid.size());
            details["missingPercent"] = missing_percent;
            progress("retaining partial UKV coverage with a GRIB bitmap",
                     details);
            reported_partial_coverage = true;
          }
        }
        return result;
      };
      const auto append =
          [&](const std::string& short_name, const ProjectedField& field,
              double scale = 1.0,
              std::optional<std::string> level_type = std::nullopt,
              std::optional<double> level = std::nullopt,
              std::optional<std::string> step_type = std::nullopt,
              int interval_hours = 0) {
            auto [values, mask] = regrid(field, short_name, scale);
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
              const ProjectedField& wind_direction, const std::string& u_name,
              const std::string& v_name,
              std::optional<std::string> level_type = std::nullopt,
              std::optional<double> level = std::nullopt) {
            ProjectedField source_u = wind_speed, source_v = wind_speed;
            for (std::size_t i = 0; i < wind_speed.values.size(); ++i) {
              const bool missing = wind_speed.mask[i] || wind_direction.mask[i];
              source_u.mask[i] = source_v.mask[i] = missing;
              if (!missing) {
                const double radians =
                    wind_direction.values[i] * std::numbers::pi / 180.0;
                source_u.values[i] = -wind_speed.values[i] * std::sin(radians);
                source_v.values[i] = -wind_speed.values[i] * std::cos(radians);
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
        append("gust", source("wind_gust_at_10m"), 1.0, std::string("surface"),
               0.0);
        append("tcc", source("cloud_amount_of_total_cloud"), 100.0,
               std::string("entireAtmosphere"), 0.0);
        if (hour > 0) {
          const int interval = hour <= 54 ? 1 : 3;
          const std::string token = interval == 1
                                        ? "precipitation_accumulation-PT01H"
                                        : "precipitation_accumulation-PT03H";
          append("tp", source(token), 1000.0, std::string("surface"), 0.0,
                 std::string("accum"), interval);
        }
      }
      if (request.preset == "all") {
        append("cape", source("CAPE_surface"), 1.0, std::string("surface"),
               0.0);
        append("r", source("relative_humidity_at_screen_level"), 100.0,
               std::string("heightAboveGround"), 2.0);
        for (const double pressure_pa : {85000.0, 70000.0, 50000.0, 30000.0}) {
          const double pressure_hpa = pressure_pa / 100.0;
          const auto upper_speed =
              source("wind_speed_on_pressure_levels", pressure_pa);
          const auto upper_direction =
              source("wind_direction_on_pressure_levels", pressure_pa);
          append_wind(upper_speed, upper_direction, "u", "v",
                      std::string("isobaricInhPa"), pressure_hpa);
          append("t", source("temperature_on_pressure_levels", pressure_pa),
                 1.0, std::string("isobaricInhPa"), pressure_hpa);
          append("r",
                 source("relative_humidity_on_pressure_levels", pressure_pa),
                 100.0, std::string("isobaricInhPa"), pressure_hpa);
          append("gh",
                 source("geopotential_height_on_pressure_levels", pressure_pa),
                 1.0, std::string("isobaricInhPa"), pressure_hpa);
        }
      }
#endif
      const auto write_started = std::chrono::steady_clock::now();
      const auto written = WriteRegularLatLonGrib2Chunk(
          grid, cycle_time, output_fields, temporary_output, !first_chunk);
      write_seconds += ElapsedSeconds(write_started);
      first_chunk = false;
      output_field_count += written.message_count;
      if (progress) {
        Json::Value details;
        details["hour"] = hour;
        details["messagesWritten"] = Json::UInt64(output_field_count);
        details["readSeconds"] = read_seconds;
        details["regridSeconds"] = regrid_seconds;
        details["writeSeconds"] = write_seconds;
        progress("streamed Met Office UKV forecast hour", details);
      }
    }
    const auto scan = ScanGribMessages(temporary_output);
    if (scan.message_count != output_field_count)
      throw ValidationError("streamed UKV GRIB message count mismatch");
    std::error_code error;
    if (std::filesystem::exists(request.output)) {
      std::filesystem::remove(request.output, error);
      if (error)
        throw ValidationError("unable to replace UKV output: " +
                              error.message());
    }
    std::filesystem::rename(temporary_output, request.output, error);
    if (error)
      throw ValidationError("unable to install UKV output: " + error.message());
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary_output, ignored);
    for (const auto& [key, path] : files) {
      (void)key;
      std::filesystem::remove(path, ignored);
    }
    throw;
  }
  for (const auto& [key, path] : files) {
    (void)key;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
  const auto inspection_started = std::chrono::steady_clock::now();
  const auto inspection = InspectGrib(request.output);
  inspection_seconds = ElapsedSeconds(inspection_started);
  const double total_seconds = ElapsedSeconds(total_started);
  if (progress) {
    Json::Value details;
    details["downloadSeconds"] = download_seconds;
    details["sourceReadSeconds"] = read_seconds;
    details["planSeconds"] = plan_seconds;
    details["regridSeconds"] = regrid_seconds;
    details["gribWriteSeconds"] = write_seconds;
    details["inspectionSeconds"] = inspection_seconds;
    details["totalSeconds"] = total_seconds;
    progress("completed Met Office UKV conversion", details);
  }
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
      {{"weather_grid_spacing_deg", std::to_string(request.grid_spacing_deg)},
       {"partial_field_count", std::to_string(partial_field_count)},
       {"maximum_missing_percent", std::to_string(maximum_missing_percent)},
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
       {"regrid_plan_count", std::to_string(interpolation_plans.size())},
#endif
       {"regrid_workers", std::to_string(worker_count)},
       {"timing_download_seconds", std::to_string(download_seconds)},
       {"timing_source_read_seconds", std::to_string(read_seconds)},
       {"timing_plan_seconds", std::to_string(plan_seconds)},
       {"timing_regrid_seconds", std::to_string(regrid_seconds)},
       {"timing_grib_write_seconds", std::to_string(write_seconds)},
       {"timing_inspection_seconds", std::to_string(inspection_seconds)},
       {"timing_total_seconds", std::to_string(total_seconds)}}};
}

}  // namespace environmental_grib
