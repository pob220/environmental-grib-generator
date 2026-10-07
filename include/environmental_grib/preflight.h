#pragma once

#include <map>
#include <set>
#include "environmental_grib/environment.h"
#include "environmental_grib/error.h"

namespace environmental_grib {
// Settings-only: no I/O, credentials, grid decoding or provider requests.
Json::Value PreflightEnvironment(const EnvironmentRequest& request);

class PreflightError : public ValidationError {
public:
  explicit PreflightError(Json::Value issue)
      : ValidationError(issue["message"].asString()),
        issue_(std::move(issue)) {}
  const Json::Value& issue() const { return issue_; }

private:
  Json::Value issue_;
};

using GribTimeInventory = std::map<std::string, std::set<TimePoint>>;
// Worker-only: reads encoded message headers, never decodes grid values.
GribTimeInventory ReadGribTimeInventory(const std::filesystem::path& path);
using TimedGribInputs =
    std::vector<std::pair<std::string, std::filesystem::path>>;
Json::Value PreflightGribTimes(const TimedGribInputs& inputs);
// Review throws only when components lack a usable common period. Partial
// field coverage is advisory and retains all records. Explicit trimming
// policies filter original messages without interpolation or extrapolation.
Json::Value ApplyGribTimePolicy(TimedGribInputs& inputs,
                                const std::string& policy,
                                const std::filesystem::path& workspace);
void ApplyPreflightAction(EnvironmentRequest& request,
                          const Json::Value& action);
}  // namespace environmental_grib
