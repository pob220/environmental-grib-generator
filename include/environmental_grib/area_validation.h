#pragma once

#include <cmath>

namespace environmental_grib {

enum class AreaCoordinate { West, South, East, North };

inline const char* AreaCoordinateLabel(AreaCoordinate field) {
  switch (field) {
    case AreaCoordinate::West: return "West longitude";
    case AreaCoordinate::South: return "South latitude";
    case AreaCoordinate::East: return "East longitude";
    case AreaCoordinate::North: return "North latitude";
  }
  return "Coordinate";
}

struct AreaValidationIssue {
  AreaCoordinate field{AreaCoordinate::West};
  const char* message{};
};

// Shared by both GUIs, presets and the engine. No I/O, provider discovery or
// allocation on the valid path. Longitude endpoints must never be sorted:
// west > east is a valid eastward box crossing the date line.
inline AreaValidationIssue ValidateDownloadArea(double west, double south,
                                                double east, double north) {
  if (!std::isfinite(west)) return {AreaCoordinate::West, "West longitude must be a finite number."};
  if (!std::isfinite(south)) return {AreaCoordinate::South, "South latitude must be a finite number."};
  if (!std::isfinite(east)) return {AreaCoordinate::East, "East longitude must be a finite number."};
  if (!std::isfinite(north)) return {AreaCoordinate::North, "North latitude must be a finite number."};
  if (west < -180 || west > 180) return {AreaCoordinate::West, "West longitude must be between -180 and 180 degrees."};
  if (east < -180 || east > 180) return {AreaCoordinate::East, "East longitude must be between -180 and 180 degrees."};
  if (south < -90 || south > 90) return {AreaCoordinate::South, "South latitude must be between -90 and 90 degrees."};
  if (north < -90 || north > 90) return {AreaCoordinate::North, "North latitude must be between -90 and 90 degrees."};
  if (west == east || (west == 180 && east == -180))
    return {AreaCoordinate::East, "West and east longitude must define an area with non-zero width."};
  if (south >= north)
    return {AreaCoordinate::North, "South latitude must be less than north latitude."};
  return {};
}

}  // namespace environmental_grib
