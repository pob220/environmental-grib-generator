#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <json/json.h>

#include "environmental_grib/geo.h"
#include "environmental_grib/remote_currents.h"

namespace environmental_grib {

struct CopernicusRequest {
  std::string provider{"copernicus_nws"};
  BoundingBox bbox;
  TimePoint start;
  int hours{};
  int step_hours{1};
  double grid_spacing_deg{0.03};
  std::string username;
  std::string password;
  std::filesystem::path output;
  bool overwrite{false};
  bool dry_run{false};
  double timeout_seconds{120.0};
};

struct CopernicusResult {
  std::filesystem::path output;
  std::size_t message_count{};
  std::size_t byte_count{};
  std::string dataset_id;
  std::string dataset_version;
  std::string service_url;
  Json::Value summary;
};

using CredentialValidator =
    std::function<bool(const std::string&, const std::string&, double)>;
bool ValidateCopernicusCredentials(const std::string& username,
                                   const std::string& password,
                                   double timeout_seconds = 60.0);

CopernicusResult GenerateCopernicusNws(const CopernicusRequest& request,
                                       BinaryDownload download = {},
                                       CredentialValidator validate_credentials = {});
CopernicusResult GenerateCopernicusGlobal(
    const CopernicusRequest& request, BinaryDownload download = {},
    CredentialValidator validate_credentials = {});
Json::Value CopernicusResultJson(const CopernicusResult& result);

}  // namespace environmental_grib
