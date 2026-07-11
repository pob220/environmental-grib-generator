#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "environmental_grib/geo.h"

namespace environmental_grib {

inline constexpr double kKnotToMps = 0.514444;
inline constexpr double kMpsToKnot = 1.0 / kKnotToMps;

struct CurrentGrid {
  TimePoint time;
  RegularGrid grid;
  std::vector<double> u_mps;
  std::vector<double> v_mps;
  std::vector<std::uint8_t> mask;

  void Validate() const;
  [[nodiscard]] bool has_mask() const { return !mask.empty(); }
};

std::pair<double, double> ComponentsToSpeedDirection(double u_mps,
                                                     double v_mps);
double DirectionErrorDegrees(double predicted, double reference);
std::pair<double, double> SpeedDirectionToComponents(
    double speed, double direction_degrees, std::string_view units = "mps");

}  // namespace environmental_grib

