#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <json/json.h>

#include "environmental_grib/model.h"

namespace environmental_grib {

struct GribScanResult {
  std::size_t message_count{};
  std::size_t byte_count{};
};

struct GribNormalizeResult {
  std::size_t message_count{};
  std::size_t raw_byte_count{};
  std::size_t clean_byte_count{};
  std::size_t skipped_byte_count{};
};

struct GribWriteSummary {
  std::size_t message_count{};
  std::filesystem::path output;
};

struct Grib2Field {
  int forecast_hour{};
  std::string short_name;
  std::vector<double> values;
  std::vector<std::uint8_t> mask;
};

struct MergeStreamsResult {
  std::map<std::string, std::size_t> input_message_counts;
  std::size_t output_message_count{};
  std::size_t byte_count{};
  Json::Value inspection;
};

/** Inputs accepted by the user-facing environmental merge operation.
 *
 * Weather and current inputs are independently optional so the same
 * production path also handles weather-only and current-only files.  At
 * least one input must be supplied.
 */
struct EnvironmentalMergeRequest {
  std::optional<std::filesystem::path> weather;
  std::optional<std::filesystem::path> current;
  std::optional<std::filesystem::path> waves;
  std::filesystem::path output;
  bool overwrite{};
};

/** Structured result from validating and merging environmental GRIBs.
 *
 * Validation failures are returned in errors rather than thrown.  This lets
 * both the GUI and headless validation tools report the same production
 * diagnostics without implementing a second merge algorithm.
 */
struct EnvironmentalMergeResult {
  bool success{};
  std::filesystem::path output;
  std::map<std::string, std::size_t> input_message_counts;
  std::size_t output_message_count{};
  std::size_t byte_count{};
  Json::Value input_validation{Json::objectValue};
  Json::Value output_inspection{Json::objectValue};
  std::vector<std::string> fields_discovered;
  std::vector<std::string> time_records_discovered;
  std::vector<std::string> fields_written;
  std::vector<std::string> warnings;
  std::vector<std::string> errors;
};

GribScanResult ScanGribMessages(const std::filesystem::path& path);
GribScanResult ScanGribBytes(std::span<const unsigned char> data);
GribNormalizeResult NormalizeGribStream(const std::filesystem::path& input,
                                        const std::filesystem::path& output);
Json::Value InspectGrib(const std::filesystem::path& path);
GribWriteSummary WriteGrib1Currents(const std::vector<CurrentGrid>& grids,
                                    const std::filesystem::path& output);
GribWriteSummary WriteRegularLatLonGrib2(const RegularGrid& grid,
                                         TimePoint reference,
                                         const std::vector<Grib2Field>& fields,
                                         const std::filesystem::path& output);
MergeStreamsResult MergeGribStreams(
    const std::vector<std::pair<std::string, std::filesystem::path>>& inputs,
    const std::filesystem::path& output, bool overwrite = false);
EnvironmentalMergeResult MergeEnvironmentalGribs(
    const EnvironmentalMergeRequest& request);
Json::Value EnvironmentalMergeResultJson(
    const EnvironmentalMergeResult& result);
/** Merge GRIBs in priority order, retaining the first message for each
 * parameter/level/valid-time tuple. */
MergeStreamsResult CompositeGribStreamsPreferFirst(
    const std::vector<std::pair<std::string, std::filesystem::path>>& inputs,
    const std::filesystem::path& output, bool overwrite = false,
    std::optional<TimePoint> valid_from = std::nullopt,
    std::optional<TimePoint> valid_through = std::nullopt);

}  // namespace environmental_grib
