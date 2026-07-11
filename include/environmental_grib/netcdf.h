#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <map>
#include <vector>

#include <json/json.h>

#include "environmental_grib/model.h"

namespace environmental_grib {

struct NetCDFOptions {
  std::optional<std::string> u_variable;
  std::optional<std::string> v_variable;
  std::optional<std::string> lat_variable;
  std::optional<std::string> lon_variable;
  std::optional<std::string> time_variable;
  std::optional<int> depth_index;
  std::optional<double> depth_value;
  std::optional<std::string> assume_units;
  bool nearest_time{false};
  double coverage_tolerance_deg{0.02};
  bool use_source_grid{false};
  double source_grid_regularity_tolerance{1e-5};
};

class NetCDFCurrentSource {
 public:
  explicit NetCDFCurrentSource(std::filesystem::path path,
                               NetCDFOptions options = {});

  [[nodiscard]] BoundingBox SourceBounds() const;
  [[nodiscard]] BoundingBox ClipBboxToSource(const BoundingBox& bbox) const;
  [[nodiscard]] RegularGrid BuildSourceGrid(const BoundingBox& bbox) const;
  [[nodiscard]] CurrentGrid GetCurrentGrid(const BoundingBox& bbox,
                                           TimePoint time,
                                           const RegularGrid& grid) const;
  [[nodiscard]] Json::Value Inspect() const;

 private:
  std::filesystem::path path_;
  NetCDFOptions options_;
};

Json::Value InspectNetCDF(const std::filesystem::path& path);

struct NetCDFScalarField {
  TimePoint time;
  std::string name;
  std::string source_variable;
  std::string units;
  std::vector<double> values;
  std::vector<std::uint8_t> mask;
};

std::vector<NetCDFScalarField> ReadNetCDFScalarFields(
    const std::filesystem::path& path, const BoundingBox& bbox,
    const std::vector<TimePoint>& times, const RegularGrid& grid,
    const std::map<std::string, std::vector<std::string>>& aliases);

}  // namespace environmental_grib
