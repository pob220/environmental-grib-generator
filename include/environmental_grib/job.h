#pragma once

#include <filesystem>
#include <string>

#include <json/json.h>

#include "environmental_grib/environment.h"

namespace environmental_grib {

inline constexpr int kJobSchemaVersion = 1;
inline constexpr const char* kGeneratorVersion = "0.1.2";

struct GeneratorJob {
  int schema_version{kJobSchemaVersion};
  std::string operation;
  EnvironmentRequest request;
  std::string copernicus_password_environment{
      "ENVIRONMENTAL_GRIB_COPERNICUS_PASSWORD"};
};

GeneratorJob ParseGeneratorJob(const Json::Value& value);
Json::Value GeneratorCapabilitiesJson();
Json::Value JobStatusJson(const std::string& status);
void WriteJsonFileAtomic(const std::filesystem::path& path,
                         const Json::Value& value);

}  // namespace environmental_grib
