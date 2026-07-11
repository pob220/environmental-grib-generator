#pragma once

#include <map>
#include <string>
#include <vector>

#include <json/json.h>

#include "environmental_grib/geo.h"
#include "environmental_grib/netcdf.h"
#include "environmental_grib/remote_currents.h"

namespace environmental_grib {

struct ArcoDataset {
  std::string dataset_id;
  std::string version_id;
  std::string service_url;
  Json::Value item;
};

ArcoDataset DiscoverArcoDataset(const std::string& product_id,
                                const std::string& dataset_id,
                                const std::string& username,
                                BinaryDownload download,
                                double timeout_seconds = 120.0);

std::map<std::string, std::vector<NetCDFScalarField>> ReadArcoFields(
    const ArcoDataset& dataset, const std::vector<std::string>& variables,
    const BoundingBox& bbox, const std::vector<TimePoint>& times,
    const RegularGrid& target_grid, const std::string& username,
    BinaryDownload download, double timeout_seconds = 120.0);

}  // namespace environmental_grib
