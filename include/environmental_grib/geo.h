#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace environmental_grib {

using TimePoint = std::chrono::sys_seconds;

struct BoundingBox {
  double west{};
  double south{};
  double east{};
  double north{};

  void Validate() const;
  [[nodiscard]] double Width() const;
  [[nodiscard]] double UnwrappedEast() const { return west + Width(); }
  [[nodiscard]] bool CrossesAntimeridian() const { return west > east; }
  [[nodiscard]] bool ContainsLongitude(double longitude, double tolerance = 0.0) const;
  [[nodiscard]] bool Contains(const BoundingBox& other) const;
  bool operator==(const BoundingBox&) const = default;
};

// Longitude geometry is continuous internally. Only provider/GRIB boundaries
// convert to their required convention; never sort the endpoints of a box.
double Longitude360(double longitude);
double Longitude180(double longitude);
double UnwrapLongitude(double longitude, double west);
bool IsCyclicLongitudeAxis(double step, std::size_t count);
struct LongitudeAxis {
  std::vector<double> coordinates;
  std::vector<std::size_t> indices;
  bool cyclic{};
};
// Sort columns eastward, remove repeated seam columns, and identify the real
// regional extent (the complement of the largest gap), including wrapped axes.
LongitudeAxis OrderLongitudeAxis(const std::vector<double>& coordinates);

struct RegularGrid {
  std::vector<double> latitudes;
  std::vector<double> longitudes;
  double spacing_deg{};
  double latitude_spacing_deg{};
  double longitude_spacing_deg{};

  [[nodiscard]] std::size_t nx() const { return longitudes.size(); }
  [[nodiscard]] std::size_t ny() const { return latitudes.size(); }
  [[nodiscard]] std::size_t size() const { return nx() * ny(); }
};

TimePoint ParseUtcDateTime(const std::string& value);
std::string FormatUtcDateTime(TimePoint value);
RegularGrid BuildRegularGrid(const BoundingBox& bbox, double spacing_deg);
// Same validation/rounding as BuildRegularGrid, without allocating coordinates.
std::pair<std::size_t, std::size_t> RegularGridDimensions(
    const BoundingBox& bbox, double spacing_deg);
std::vector<TimePoint> BuildTimeSequence(TimePoint start, int hours,
                                         int step_hours);

}  // namespace environmental_grib
