#pragma once

#include "environmental_grib/environment.h"

namespace environmental_grib {
// Offline, bounded calculation only: no forecast, model or credential access.
// Unknown components never silently contribute zero to a complete total.
Json::Value EstimateEnvironment(const EnvironmentRequest& request);
// Compare a pre-generation estimate with the final merged inspection. The
// allowlisted request contains no credentials, URLs or local source paths.
Json::Value BuildSizeComparison(const EnvironmentRequest& request,
                               const Json::Value& estimate,
                               const EnvironmentResult& generated);
}
