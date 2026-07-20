#include "environmental_grib/rtofs.h"

#include <libqhull_r/qhull_ra.h>
#include <netcdf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <unordered_map>

#include "environmental_grib/error.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/platform.h"
#include "environmental_grib/weather.h"

namespace environmental_grib {
namespace {
constexpr const char* kNomadsBase =
    "https://nomads.ncep.noaa.gov/pub/data/nccf/com/rtofs/prod";

const std::map<std::string, BoundingBox> kRegions{
    {"US_east", {-100.0, 0.0, -35.0, 55.0}},
    {"US_west", {-170.0, 10.0, -105.0, 65.0}},
    {"alaska", {-180.0, 45.0, -120.0, 75.0}},
};

void Nc(int status, const std::string& action) {
  if (status != NC_NOERR) throw ValidationError(action + ": " + nc_strerror(status));
}

class NcFile {
 public:
  explicit NcFile(const std::filesystem::path& path) {
    const auto utf8_path = PathToUtf8(path);
    Nc(nc_open(utf8_path.c_str(), NC_NOWRITE, &id_),
       "opening RTOFS NetCDF");
  }
  ~NcFile() { if (id_ >= 0) nc_close(id_); }
  int id() const { return id_; }
 private:
  int id_{-1};
};

int Var(int file, const char* name) {
  int id = -1;
  Nc(nc_inq_varid(file, name, &id), std::string("finding RTOFS variable ") + name);
  return id;
}

std::vector<int> Dims(int file, int variable) {
  int count = 0;
  Nc(nc_inq_varndims(file, variable, &count), "reading RTOFS dimensions");
  std::vector<int> dims(static_cast<std::size_t>(count));
  Nc(nc_inq_vardimid(file, variable, dims.data()), "reading RTOFS dimension ids");
  return dims;
}

std::size_t DimSize(int file, int dim) {
  std::size_t size = 0;
  Nc(nc_inq_dimlen(file, dim, &size), "reading RTOFS dimension size");
  return size;
}

std::vector<double> ReadAll(int file, int variable) {
  std::size_t count = 1;
  for (int dim : Dims(file, variable)) count *= DimSize(file, dim);
  std::vector<double> values(count);
  Nc(nc_get_var_double(file, variable, values.data()), "reading RTOFS variable");
  return values;
}

std::vector<double> ReadSurface(int file, int variable, std::size_t ny,
                                std::size_t nx) {
  const auto dims = Dims(file, variable);
  if (dims.size() < 2) throw ValidationError("RTOFS u/v variable has too few dimensions");
  std::vector<std::size_t> start(dims.size(), 0), count(dims.size(), 1);
  count[dims.size() - 2] = ny;
  count[dims.size() - 1] = nx;
  std::vector<double> values(ny * nx);
  Nc(nc_get_vara_double(file, variable, start.data(), count.data(), values.data()),
     "reading RTOFS surface current");
  double fill = std::numeric_limits<double>::quiet_NaN();
  const bool have_fill = nc_get_att_double(file, variable, "_FillValue", &fill) == NC_NOERR ||
                         nc_get_att_double(file, variable, "missing_value", &fill) == NC_NOERR;
  for (double& value : values) {
    if ((have_fill && value == fill) || !std::isfinite(value))
      value = std::numeric_limits<double>::quiet_NaN();
  }
  return values;
}

struct Point { double x{}, y{}, u{}, v{}; };
struct Triangle {
  std::array<std::size_t, 3> point{};
  double min_x{}, max_x{}, min_y{}, max_y{};
};

class DelaunayInterpolator {
 public:
  explicit DelaunayInterpolator(std::vector<Point> points)
      : points_(std::move(points)) {
    if (points_.size() < 3) throw ValidationError("RTOFS source subset has fewer than three valid points");
    std::vector<coordT> coordinates(points_.size() * 2);
    for (std::size_t i = 0; i < points_.size(); ++i) {
      coordinates[i * 2] = points_[i].x;
      coordinates[i * 2 + 1] = points_[i].y;
    }
    qhT state;
    qhT* qh = &state;
    qh_zero(qh, stderr);
    char options[] = "qhull d Qbb Qt Qc Qz";
    const int status = qh_new_qhull(qh, 2, static_cast<int>(points_.size()),
                                    coordinates.data(), 0, options, nullptr, stderr);
    if (status != qh_ERRnone) {
      qh_freeqhull(qh, !qh_ALL);
      int current = 0, maximum = 0;
      qh_memfreeshort(qh, &current, &maximum);
      throw ValidationError("Qhull could not triangulate the RTOFS source grid");
    }
    facetT* facet;
    vertexT* vertex;
    vertexT** vertexp;
    FORALLfacets {
      if (facet->upperdelaunay) continue;
      Triangle triangle;
      std::size_t count = 0;
      FOREACHvertex_(facet->vertices) {
        const int id = qh_pointid(qh, vertex->point);
        if (id >= 0 && static_cast<std::size_t>(id) < points_.size() && count < 3)
          triangle.point[count++] = static_cast<std::size_t>(id);
      }
      if (count != 3) continue;
      const auto& a = points_[triangle.point[0]];
      const auto& b = points_[triangle.point[1]];
      const auto& c = points_[triangle.point[2]];
      triangle.min_x = std::min({a.x, b.x, c.x});
      triangle.max_x = std::max({a.x, b.x, c.x});
      triangle.min_y = std::min({a.y, b.y, c.y});
      triangle.max_y = std::max({a.y, b.y, c.y});
      triangles_.push_back(triangle);
    }
    qh_freeqhull(qh, !qh_ALL);
    int current = 0, maximum = 0;
    qh_memfreeshort(qh, &current, &maximum);
    if (triangles_.empty()) throw ValidationError("RTOFS triangulation produced no finite triangles");
    min_x_ = min_y_ = std::numeric_limits<double>::infinity();
    max_x_ = max_y_ = -std::numeric_limits<double>::infinity();
    for (const auto& point : points_) {
      min_x_ = std::min(min_x_, point.x); max_x_ = std::max(max_x_, point.x);
      min_y_ = std::min(min_y_, point.y); max_y_ = std::max(max_y_, point.y);
    }
    for (std::size_t index = 0; index < triangles_.size(); ++index) {
      const auto& triangle = triangles_[index];
      const int x0 = BinX(triangle.min_x), x1 = BinX(triangle.max_x);
      const int y0 = BinY(triangle.min_y), y1 = BinY(triangle.max_y);
      for (int by = y0; by <= y1; ++by)
        for (int bx = x0; bx <= x1; ++bx) bins_[by * kBins + bx].push_back(index);
    }
  }

  std::pair<double, double> At(double x, double y) const {
    static const std::vector<std::size_t> empty;
    const auto found = bins_.find(BinY(y) * kBins + BinX(x));
    const auto& candidates = found == bins_.end() ? empty : found->second;
    for (std::size_t triangle_index : candidates) {
      const auto& triangle = triangles_[triangle_index];
      if (x < triangle.min_x || x > triangle.max_x || y < triangle.min_y || y > triangle.max_y) continue;
      const auto& a = points_[triangle.point[0]];
      const auto& b = points_[triangle.point[1]];
      const auto& c = points_[triangle.point[2]];
      const double denominator = (b.y - c.y) * (a.x - c.x) +
                                 (c.x - b.x) * (a.y - c.y);
      if (std::abs(denominator) < 1e-15) continue;
      const double wa = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / denominator;
      const double wb = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / denominator;
      const double wc = 1.0 - wa - wb;
      constexpr double tolerance = -1e-10;
      if (wa >= tolerance && wb >= tolerance && wc >= tolerance)
        return {wa * a.u + wb * b.u + wc * c.u,
                wa * a.v + wb * b.v + wc * c.v};
    }
    const Point* nearest = nullptr;
    double best = std::numeric_limits<double>::infinity();
    for (const auto& point : points_) {
      const double distance = std::hypot(point.x - x, point.y - y);
      if (distance < best) { best = distance; nearest = &point; }
    }
    return {nearest->u, nearest->v};
  }

 private:
  static constexpr int kBins = 64;
  int BinX(double x) const {
    if (max_x_ <= min_x_) return 0;
    return std::clamp(static_cast<int>((x - min_x_) / (max_x_ - min_x_) * kBins), 0, kBins - 1);
  }
  int BinY(double y) const {
    if (max_y_ <= min_y_) return 0;
    return std::clamp(static_cast<int>((y - min_y_) / (max_y_ - min_y_) * kBins), 0, kBins - 1);
  }
  std::vector<Point> points_;
  std::vector<Triangle> triangles_;
  double min_x_{}, max_x_{}, min_y_{}, max_y_{};
  std::unordered_map<int, std::vector<std::size_t>> bins_;
};

CurrentGrid ConvertFile(const std::filesystem::path& path, TimePoint time,
                        const BoundingBox& bbox, const RegularGrid& grid) {
  NcFile file(path);
  const int lat_id = Var(file.id(), "Latitude");
  const int lon_id = Var(file.id(), "Longitude");
  const auto lat_dims = Dims(file.id(), lat_id);
  if (lat_dims.size() != 2) throw ValidationError("RTOFS Latitude must be two-dimensional");
  const std::size_t ny = DimSize(file.id(), lat_dims[0]);
  const std::size_t nx = DimSize(file.id(), lat_dims[1]);
  auto lat = ReadAll(file.id(), lat_id);
  auto lon = ReadAll(file.id(), lon_id);
  auto u = ReadSurface(file.id(), Var(file.id(), "u"), ny, nx);
  auto v = ReadSurface(file.id(), Var(file.id(), "v"), ny, nx);
  const double margin = std::max(0.5, grid.spacing_deg * 3.0);
  std::vector<Point> points;
  for (std::size_t i = 0; i < lat.size(); ++i) {
    if (lon[i] < bbox.west - margin || lon[i] > bbox.east + margin ||
        lat[i] < bbox.south - margin || lat[i] > bbox.north + margin ||
        !std::isfinite(u[i]) || !std::isfinite(v[i])) continue;
    points.push_back({lon[i], lat[i], u[i], v[i]});
  }
  DelaunayInterpolator interpolate(std::move(points));
  CurrentGrid result{time, grid, std::vector<double>(grid.size()),
                     std::vector<double>(grid.size()), {}};
  for (std::size_t y = 0; y < grid.ny(); ++y) {
    for (std::size_t x = 0; x < grid.nx(); ++x) {
      const auto value = interpolate.At(grid.longitudes[x], grid.latitudes[y]);
      const auto index = y * grid.nx() + x;
      result.u_mps[index] = value.first;
      result.v_mps[index] = value.second;
    }
  }
  return result;
}

std::map<int, std::set<std::string>> ParseInventory(const std::string& html) {
  const std::regex pattern(R"(rtofs_glo_3dz_f([0-9]{3})_6hrly_hvr_([A-Za-z_]+)\.nc)");
  std::map<int, std::set<std::string>> result;
  for (auto it = std::sregex_iterator(html.begin(), html.end(), pattern);
       it != std::sregex_iterator(); ++it)
    result[std::stoi((*it)[1].str())].insert((*it)[2].str());
  return result;
}

std::vector<RtofsCycle> CycleCandidates(const RtofsRequest& request) {
  if (request.cycle != "auto") {
    if (!request.date) throw ValidationError("RTOFS explicit cycle requires a date");
    return {{*request.date, request.cycle}};
  }
  const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
  std::vector<RtofsCycle> cycles;
  for (int day = 0; day < std::max(1, request.max_auto_days); ++day)
    cycles.push_back({FormatUtcDateTime(today - std::chrono::days(day)).substr(0, 10), "00"});
  for (auto& cycle : cycles)
    cycle.date.erase(std::remove(cycle.date.begin(), cycle.date.end(), '-'), cycle.date.end());
  return cycles;
}
}  // namespace

TimePoint RtofsCycle::CycleTime() const {
  return ParseUtcDateTime(date.substr(0, 4) + "-" + date.substr(4, 2) + "-" +
                          date.substr(6, 2) + "T" + cycle + ":00:00Z");
}
std::string RtofsCycle::DirectoryName() const { return "rtofs." + date; }

std::vector<int> RtofsForecastHours(int requested_hours, int step_hours) {
  if (requested_hours <= 0 || step_hours <= 0) throw ValidationError("RTOFS hours and step must be positive");
  if (requested_hours > kRtofsMaxForecastHour) throw ValidationError("RTOFS hours exceed 192-hour provider limit");
  const int step = std::max(kRtofsNativeStepHours, step_hours);
  std::vector<int> result;
  for (int hour = kRtofsNativeStepHours; hour <= requested_hours; hour += step) result.push_back(hour);
  const int rounded = requested_hours - requested_hours % step;
  if (rounded >= kRtofsNativeStepHours &&
      std::find(result.begin(), result.end(), rounded) == result.end()) result.push_back(rounded);
  std::sort(result.begin(), result.end());
  return result;
}

std::string RtofsRegionForBbox(const BoundingBox& bbox) {
  for (const auto& [name, coverage] : kRegions) if (coverage.Contains(bbox)) return name;
  throw ValidationError("RTOFS public regional NetCDF files do not cover the requested bbox");
}
std::string RtofsDirectoryUrl(const RtofsCycle& cycle) { return std::string(kNomadsBase) + "/" + cycle.DirectoryName() + "/"; }
std::string RtofsFilename(int hour, const std::string& region) {
  char value[128];
  std::snprintf(value, sizeof(value), "rtofs_glo_3dz_f%03d_6hrly_hvr_%s.nc", hour, region.c_str());
  return value;
}
std::string RtofsUrl(const RtofsCycle& cycle, int hour, const std::string& region) { return RtofsDirectoryUrl(cycle) + RtofsFilename(hour, region); }

RtofsResult GenerateRtofs(const RtofsRequest& request,
                          TextDownload text_download,
                          BinaryDownload binary_download) {
  request.bbox.Validate();
  if (std::filesystem::exists(request.output) && !request.overwrite)
    throw ValidationError("output already exists: " + PathToUtf8(request.output));
  if (!text_download) text_download = [](const std::string& url, double timeout) {
    const auto bytes = CurlHttpGet(url, timeout);
    return std::string(bytes.begin(), bytes.end());
  };
  if (!binary_download) binary_download = CurlHttpGet;
  const auto hours = RtofsForecastHours(request.hours, request.step_hours);
  const auto region = RtofsRegionForBbox(request.bbox);
  std::optional<RtofsCycle> selected;
  std::vector<std::string> errors;
  for (const auto& candidate : CycleCandidates(request)) {
    try {
      const auto inventory = ParseInventory(text_download(RtofsDirectoryUrl(candidate), 30.0));
      bool complete = true;
      for (int hour : hours) {
        auto found = inventory.find(hour);
        if (found == inventory.end() || !found->second.contains(region)) { complete = false; break; }
      }
      if (complete) { selected = candidate; break; }
      errors.push_back(candidate.DirectoryName() + ": incomplete inventory");
    } catch (const std::exception& error) { errors.push_back(candidate.DirectoryName() + ": " + error.what()); }
  }
  if (!selected) throw ValidationError("no complete RTOFS cycle found");
  std::vector<std::filesystem::path> files;
  for (int hour : hours) files.push_back(request.download_directory / selected->DirectoryName() / RtofsFilename(hour, region));
  RtofsResult result{request.output, 0, 0, FormatUtcDateTime(selected->CycleTime()), hours, files, Json::Value(Json::objectValue)};
  if (request.dry_run) return result;
  std::vector<CurrentGrid> grids;
  const auto grid = BuildRegularGrid(request.bbox, request.grid_spacing_deg);
  for (std::size_t i = 0; i < files.size(); ++i) {
    if (!std::filesystem::is_regular_file(files[i]) || std::filesystem::file_size(files[i]) == 0) {
      const auto bytes = binary_download(RtofsUrl(*selected, hours[i], region), request.timeout_seconds);
      std::filesystem::create_directories(files[i].parent_path());
      auto temporary = files[i]; temporary += ".tmp";
      std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
      stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      if (!stream) throw ValidationError("writing RTOFS download failed");
      stream.close();
      std::filesystem::rename(temporary, files[i]);
    }
    grids.push_back(ConvertFile(files[i], selected->CycleTime() + std::chrono::hours(hours[i]), request.bbox, grid));
  }
  WriteGrib1Currents(grids, request.output);
  const auto inspection = InspectGrib(request.output);
  result.message_count = inspection["message_count"].asUInt64();
  result.byte_count = std::filesystem::file_size(request.output);
  result.summary["provider"] = "noaa_rtofs_global";
  result.summary["region"] = region;
  result.summary["selected_cycle"] = result.selected_cycle;
  result.summary["regridding"] = "Qhull Delaunay linear interpolation with nearest fallback";
  result.summary["grid_points"] = Json::UInt64(grid.size());
  return result;
}

Json::Value RtofsResultJson(const RtofsResult& result) {
  Json::Value value(Json::objectValue);
  value["output"] = PathToUtf8(result.output);
  value["message_count"] = Json::UInt64(result.message_count);
  value["byte_count"] = Json::UInt64(result.byte_count);
  value["selected_cycle"] = result.selected_cycle;
  for (int hour : result.forecast_hours) value["forecast_hours"].append(hour);
  value["summary"] = result.summary;
  return value;
}

}  // namespace environmental_grib
