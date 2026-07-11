#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace environmental_grib {

using TimePoint = std::chrono::sys_seconds;

struct BoundingBox {
  double west{};
  double south{};
  double east{};
  double north{};

  void Validate() const;
  [[nodiscard]] bool Contains(const BoundingBox& other) const;
  bool operator==(const BoundingBox&) const = default;
};

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
std::vector<TimePoint> BuildTimeSequence(TimePoint start, int hours,
                                         int step_hours);

}  // namespace environmental_grib

