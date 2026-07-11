#pragma once

#include "environmental_grib/model.h"

namespace environmental_grib {

CurrentGrid MakeConstantCurrent(const BoundingBox& bbox, TimePoint time,
                                const RegularGrid& grid, double u, double v,
                                const std::string& units = "mps");
CurrentGrid MakeSyntheticRotaryCurrent(const BoundingBox& bbox, TimePoint time,
                                       const RegularGrid& grid,
                                       double peak_speed_knots = 2.2,
                                       double period_hours = 12.42);

}  // namespace environmental_grib

