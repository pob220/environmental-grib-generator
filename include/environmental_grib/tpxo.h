#pragma once

#include <complex>
#include <filesystem>
#include <string>
#include <vector>

#include <json/json.h>

#include "environmental_grib/model.h"

namespace environmental_grib {

struct TpxoCache {
  Json::Value metadata;
  BoundingBox bbox;
  RegularGrid grid;
  std::vector<std::string> constituents;
  // Constituent-major matrices: [constituent][flattened latitude/longitude].
  std::vector<std::complex<double>> u_cm_s;
  std::vector<std::complex<double>> v_cm_s;
};

struct TpxoGenerationResult {
  std::filesystem::path output;
  std::size_t message_count{};
  std::size_t byte_count{};
  Json::Value inspection;
};

struct TpxoModelRequest {
  BoundingBox bbox;
  TimePoint start;
  int hours{};
  int step_hours{1};
  double grid_spacing_deg{0.05};
  std::filesystem::path model_directory;
  std::filesystem::path output;
  bool infer_minor{true};
  bool overwrite{false};
};

TpxoCache LoadTpxoCache(const std::filesystem::path& path);
void WriteTpxoCache(const std::filesystem::path& path,
                    const TpxoCache& cache, bool overwrite = false);
Json::Value InspectTpxoCache(const std::filesystem::path& path);
std::vector<CurrentGrid> PredictTpxoCache(
    const TpxoCache& cache, const std::vector<TimePoint>& times,
    bool infer_minor = true);
TpxoGenerationResult GenerateFromTpxoCache(
    const std::filesystem::path& input_cache, TimePoint start, int hours,
    int step_hours, const std::filesystem::path& output,
    bool infer_minor = true, bool overwrite = false);
TpxoCache LoadTpxo10AtlasModel(const std::filesystem::path& model_directory,
                               const BoundingBox& bbox,
                               const RegularGrid& output_grid);
std::filesystem::path ResolveTpxo10AtlasDirectory(
    const std::filesystem::path& model_directory);
TpxoGenerationResult GenerateFromTpxo10AtlasModel(
    const TpxoModelRequest& request);
Json::Value PrepareTpxo10Cache(const std::filesystem::path& model_directory,
                               const BoundingBox& bbox,
                               double grid_spacing_deg,
                               const std::filesystem::path& output,
                               bool overwrite = false);

}  // namespace environmental_grib
