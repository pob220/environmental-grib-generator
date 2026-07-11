#include "environmental_grib/sources.h"

#include <cmath>

#include "environmental_grib/error.h"

namespace environmental_grib {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
}

CurrentGrid MakeConstantCurrent(const BoundingBox&, TimePoint time,
                                const RegularGrid& grid, double u, double v,
                                const std::string& units) {
  const double multiplier = units == "knots" ? kKnotToMps : 1.0;
  if (units != "knots" && units != "mps") {
    throw ValidationError("units must be 'knots' or 'mps'");
  }
  CurrentGrid result{time, grid, std::vector<double>(grid.size(), u * multiplier),
                     std::vector<double>(grid.size(), v * multiplier), {}};
  result.Validate();
  return result;
}

CurrentGrid MakeSyntheticRotaryCurrent(const BoundingBox& bbox, TimePoint time,
                                       const RegularGrid& grid,
                                       double peak_speed_knots,
                                       double period_hours) {
  bbox.Validate();
  if (period_hours <= 0.0) throw ValidationError("period_hours must be positive");
  CurrentGrid result{time, grid, std::vector<double>(grid.size()),
                     std::vector<double>(grid.size()), {}};
  const double unix_hours = static_cast<double>(time.time_since_epoch().count()) / 3600.0;
  const double temporal_phase = 2.0 * kPi * (unix_hours / period_hours);
  for (std::size_t y = 0; y < grid.ny(); ++y) {
    const double lat_norm = (grid.latitudes[y] - bbox.south) /
                            std::max(bbox.north - bbox.south, 1e-9);
    for (std::size_t x = 0; x < grid.nx(); ++x) {
      const double lon_norm = (grid.longitudes[x] - bbox.west) /
                              std::max(bbox.east - bbox.west, 1e-9);
      const double spatial_phase = 1.4 * lon_norm - 0.9 * lat_norm;
      double amplitude_knots =
          peak_speed_knots * (0.35 + 0.65 * (0.25 + 0.75 * lon_norm));
      amplitude_knots *= 0.82 + 0.18 * std::cos(kPi * (lat_norm - 0.35));
      const double amplitude_mps = amplitude_knots * kKnotToMps;
      const double phase = temporal_phase + spatial_phase;
      const double ellipticity = 0.68 + 0.18 * std::sin(kPi * lat_norm);
      const double shear = 0.12 * amplitude_mps * std::sin(2.0 * kPi * lon_norm);
      const std::size_t index = y * grid.nx() + x;
      result.u_mps[index] = amplitude_mps * std::cos(phase) + shear;
      result.v_mps[index] = amplitude_mps * ellipticity * std::sin(phase);
    }
  }
  return result;
}

}  // namespace environmental_grib
