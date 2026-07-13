#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <json/json.h>

#include "environmental_grib/model.h"
#include "environmental_grib/random_access.h"
#include "environmental_grib/xtd.h"

namespace environmental_grib {

enum class OfflineCurrentMode {
  kAstronomicalTideOnly,
  kTideAndExpectedSeasonalCirculation,
};

std::string OfflineCurrentModeId(OfflineCurrentMode mode);
OfflineCurrentMode ParseOfflineCurrentMode(const std::string& value);

struct XtdPackageStatus {
  std::filesystem::path path;
  std::uint32_t format_version{};
  Json::Value metadata;
  bool authenticated{};
  bool tide_available{};
  bool climatology_available{};
  bool uncertainty_available{};
  std::string residual_representation;
  std::string package_id;
  std::string package_hash;
  std::string parent_package_hash;
  double validation_ms{};
};

struct XtdPackageStatistics {
  XtdStatistics tide;
  XtdStatistics residual;
  XtdStatistics uncertainty;
  std::uint64_t outer_bytes_read{};
};

class XtdPackageReader {
public:
  explicit XtdPackageReader(const std::filesystem::path& path,
                            XtdReaderOptions options = {});
  ~XtdPackageReader();
  XtdPackageReader(XtdPackageReader&&) noexcept;
  XtdPackageReader& operator=(XtdPackageReader&&) noexcept;
  XtdPackageReader(const XtdPackageReader&) = delete;
  XtdPackageReader& operator=(const XtdPackageReader&) = delete;

  [[nodiscard]] const XtdPackageStatus& status() const noexcept;
  [[nodiscard]] XtdPackageStatistics statistics() const noexcept;
  void ClearTileCaches() noexcept;

  std::vector<CurrentGrid> Predict(const RegularGrid& output_grid,
                                   const std::vector<TimePoint>& times,
                                   OfflineCurrentMode mode,
                                   bool infer_minor_tides = true);
  Json::Value VerifyAllComponents();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

Json::Value InspectXtdPackage(const std::filesystem::path& path);
Json::Value VerifyXtdPackage(const std::filesystem::path& path,
                             XtdReaderOptions options = {});

}  // namespace environmental_grib
