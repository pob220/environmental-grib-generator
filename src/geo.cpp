#include "environmental_grib/geo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>

#include "environmental_grib/error.h"

namespace environmental_grib {

void BoundingBox::Validate() const {
  if (!std::isfinite(west) || !std::isfinite(east) || west < -180.0 ||
      west > 180.0 || east < -180.0 || east > 180.0) {
    throw ValidationError("bbox longitudes must be within [-180, 180]");
  }
  if (!std::isfinite(south) || !std::isfinite(north) || south < -90.0 ||
      south > 90.0 || north < -90.0 || north > 90.0) {
    throw ValidationError("bbox latitudes must be within [-90, 90]");
  }
  if (west >= east) {
    throw ValidationError(
        "bbox west must be less than east; antimeridian boxes are not supported yet");
  }
  if (south >= north) {
    throw ValidationError("bbox south must be less than north");
  }
}

bool BoundingBox::Contains(const BoundingBox& other) const {
  return other.west >= west && other.east <= east && other.south >= south &&
         other.north <= north;
}

TimePoint ParseUtcDateTime(const std::string& value) {
  std::string raw = value;
  if (raw.empty()) throw ValidationError("datetime cannot be empty");

  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  char separator = 0;
  int consumed = 0;
  if (std::sscanf(raw.c_str(), "%d-%d-%d%c%d:%d:%d%n", &year, &month, &day,
                  &separator, &hour, &minute, &second, &consumed) != 7 ||
      (separator != 'T' && separator != ' ')) {
    throw ValidationError("invalid datetime '" + value +
                          "'; use ISO-8601, e.g. 2026-07-01T00:00:00Z");
  }
  int offset_seconds = 0;
  const std::string suffix = raw.substr(static_cast<std::size_t>(consumed));
  if (suffix == "Z" || suffix == "+00:00") {
    offset_seconds = 0;
  } else if (suffix.size() == 6 && (suffix[0] == '+' || suffix[0] == '-')) {
    int offset_hour = 0, offset_minute = 0;
    if (std::sscanf(suffix.c_str() + 1, "%d:%d", &offset_hour,
                    &offset_minute) != 2 ||
        offset_hour > 23 || offset_minute > 59) {
      throw ValidationError("invalid datetime timezone offset");
    }
    offset_seconds = (offset_hour * 3600 + offset_minute * 60) *
                     (suffix[0] == '+' ? 1 : -1);
  } else {
    throw ValidationError("datetime must include a timezone; UTC 'Z' is recommended");
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
      minute > 59 || second > 60) {
    throw ValidationError("invalid datetime fields");
  }
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = std::min(second, 59);
  const std::time_t utc = timegm(&tm);
  std::tm check{};
  gmtime_r(&utc, &check);
  if (check.tm_year != tm.tm_year || check.tm_mon != tm.tm_mon ||
      check.tm_mday != tm.tm_mday || check.tm_hour != tm.tm_hour ||
      check.tm_min != tm.tm_min) {
    throw ValidationError("invalid calendar date");
  }
  return TimePoint{std::chrono::seconds{utc - offset_seconds}};
}

std::string FormatUtcDateTime(TimePoint value) {
  const auto seconds = value.time_since_epoch().count();
  const std::time_t raw = static_cast<std::time_t>(seconds);
  std::tm tm{};
  gmtime_r(&raw, &tm);
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

RegularGrid BuildRegularGrid(const BoundingBox& bbox, double spacing_deg) {
  bbox.Validate();
  if (!std::isfinite(spacing_deg) || spacing_deg <= 0.0) {
    throw ValidationError("grid spacing must be greater than zero");
  }
  const double width = bbox.east - bbox.west;
  const double height = bbox.north - bbox.south;
  const double tolerance = std::max(1e-12, spacing_deg * 1e-9);
  if (spacing_deg > width + tolerance || spacing_deg > height + tolerance) {
    throw ValidationError(
        "grid spacing must be smaller than both bbox width and height");
  }
  const auto nx = static_cast<std::size_t>(std::llround(width / spacing_deg)) + 1;
  const auto ny = static_cast<std::size_t>(std::llround(height / spacing_deg)) + 1;
  if (nx < 2 || ny < 2) throw ValidationError("grid must contain at least two points in each dimension");
  if (nx > 5'000'000 / ny) {
    throw ValidationError("grid is too large; reduce bbox or increase spacing");
  }
  RegularGrid grid;
  grid.spacing_deg = spacing_deg;
  grid.latitude_spacing_deg = spacing_deg;
  grid.longitude_spacing_deg = spacing_deg;
  grid.longitudes.resize(nx);
  grid.latitudes.resize(ny);
  for (std::size_t i = 0; i < nx; ++i) grid.longitudes[i] = bbox.west + static_cast<double>(i) * spacing_deg;
  for (std::size_t j = 0; j < ny; ++j) grid.latitudes[j] = bbox.south + static_cast<double>(j) * spacing_deg;
  grid.longitudes.back() = bbox.east;
  grid.latitudes.back() = bbox.north;
  return grid;
}

std::vector<TimePoint> BuildTimeSequence(TimePoint start, int hours,
                                         int step_hours) {
  if (hours < 0) throw ValidationError("hours must be zero or greater");
  if (step_hours <= 0) throw ValidationError("step-hours must be greater than zero");
  if (hours % step_hours != 0) throw ValidationError("hours must be evenly divisible by step-hours");
  std::vector<TimePoint> result;
  result.reserve(static_cast<std::size_t>(hours / step_hours + 1));
  for (int hour = 0; hour <= hours; hour += step_hours) {
    result.push_back(start + std::chrono::hours{hour});
  }
  return result;
}

}  // namespace environmental_grib

