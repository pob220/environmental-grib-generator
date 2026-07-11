#include "environmental_grib/model.h"

#include <algorithm>
#include <cmath>

#include "environmental_grib/error.h"

namespace environmental_grib {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
}

void CurrentGrid::Validate() const {
  if (u_mps.size() != grid.size() || v_mps.size() != grid.size()) {
    throw ValidationError("u and v arrays must match the grid shape");
  }
  if (!mask.empty() && mask.size() != grid.size()) {
    throw ValidationError("mask must match the grid shape");
  }
}

std::pair<double, double> ComponentsToSpeedDirection(double u_mps,
                                                     double v_mps) {
  const double speed = std::hypot(u_mps, v_mps) * kMpsToKnot;
  double direction = std::atan2(u_mps, v_mps) * 180.0 / kPi;
  direction = std::fmod(direction + 360.0, 360.0);
  return {speed, direction};
}

double DirectionErrorDegrees(double predicted, double reference) {
  double value = std::fmod(predicted - reference + 180.0, 360.0);
  if (value < 0.0) value += 360.0;
  return value - 180.0;
}

std::pair<double, double> SpeedDirectionToComponents(
    double speed, double direction_degrees, std::string_view units) {
  double speed_mps = speed;
  if (units == "knots") {
    speed_mps *= kKnotToMps;
  } else if (units != "mps") {
    throw ValidationError("units must be 'knots' or 'mps'");
  }
  const double radians = direction_degrees * kPi / 180.0;
  return {speed_mps * std::sin(radians), speed_mps * std::cos(radians)};
}

}  // namespace environmental_grib

