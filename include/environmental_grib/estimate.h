#pragma once

#include "environmental_grib/environment.h"

namespace environmental_grib {
// Offline, bounded calculation only: no forecast, model or credential access.
// Unknown components never silently contribute zero to a complete total.
Json::Value EstimateEnvironment(const EnvironmentRequest& request);
}
