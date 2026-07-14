#include "environmental_grib/waves.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "environmental_grib/error.h"
#include "environmental_grib/arco.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/netcdf.h"
#include "environmental_grib/weather.h"

namespace environmental_grib {
namespace {

void ConvertUnits(NetCDFScalarField& field) {
  std::string units = field.units;
  units.erase(std::remove_if(units.begin(), units.end(), [](unsigned char c) { return std::isspace(c); }), units.end());
  std::transform(units.begin(), units.end(), units.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  bool supported = units.empty();
  if (field.name == "swh") supported = supported || units == "m" || units == "meter" || units == "metre" || units == "meters" || units == "metres";
  if (field.name == "perpw") supported = supported || units == "s" || units == "sec" || units == "second" || units == "seconds";
  if (field.name == "dirpw") {
    supported = supported || units == "degree" || units == "degrees" || units == "degrees_true" || units == "deg" || units == "1";
    if (units == "radian" || units == "radians" || units == "rad") {
      constexpr double factor = 180.0 / 3.141592653589793238462643383279502884;
      for (double& value : field.values) value *= factor;
      supported = true;
    }
  }
  if (!supported) throw ValidationError("unsupported Copernicus wave units for " + field.name + ": " + field.units);
}

double InferSpacing(const Json::Value& inspection, double fallback) {
  if (!inspection.isMember("longitude_range")) return fallback;
  const auto& dimensions = inspection["dimensions"];
  std::size_t count = 0;
  if (dimensions.isMember("longitude")) count = dimensions["longitude"].asUInt64();
  else if (dimensions.isMember("lon")) count = dimensions["lon"].asUInt64();
  if (count < 2) return fallback;
  return std::abs(inspection["longitude_range"][1].asDouble() -
                  inspection["longitude_range"][0].asDouble()) /
         static_cast<double>(count - 1);
}

}  // namespace

WaveConvertResult ConvertCopernicusWaveNetCDF(
    const std::filesystem::path& input, const BoundingBox& bbox,
    TimePoint start, int hours, int step_hours,
    const std::filesystem::path& output,
    std::optional<double> grid_spacing_deg, bool overwrite) {
  bbox.Validate();
  if (step_hours != 3) throw ValidationError("Copernicus Global Waves currently supports 3-hour wave steps");
  if (hours > 240) throw ValidationError("Copernicus Global Waves forecast requests are limited to 240 hours");
  if (std::filesystem::exists(output) && !overwrite) throw ValidationError("output already exists; enable overwrite");
  const auto times = BuildTimeSequence(start, hours, step_hours);
  const auto source_inspection = InspectNetCDF(input);
  const double spacing = grid_spacing_deg.value_or(InferSpacing(source_inspection, 0.0833333));
  const auto grid = BuildRegularGrid(bbox, spacing);
  const std::map<std::string, std::vector<std::string>> aliases{
      {"swh", {"VHM0", "significant_wave_height", "sea_surface_wave_significant_height"}},
      {"perpw", {"VTPK", "peak_wave_period", "sea_surface_wave_peak_period"}},
      {"dirpw", {"VMDR", "mean_wave_direction", "sea_surface_wave_from_direction"}}};
  auto scalar_fields = ReadNetCDFScalarFields(input, bbox, times, grid, aliases);
  std::vector<Grib2Field> fields;
  Json::Value coverage(Json::objectValue);
  for (auto& field : scalar_fields) {
    ConvertUnits(field);
    const auto forecast = std::chrono::duration_cast<std::chrono::hours>(field.time - start).count();
    std::size_t valid = field.values.size();
    if (!field.mask.empty()) valid -= std::count_if(field.mask.begin(), field.mask.end(), [](auto value) { return value != 0; });
    if (valid == 0) throw ValidationError("Copernicus wave field " + field.name + " has no valid coverage inside requested bbox");
    const std::string key = "f" + std::to_string(forecast) + "_" + field.name;
    coverage[key]["valid_cell_count"] = Json::UInt64(valid);
    coverage[key]["missing_percent"] = 100.0 * static_cast<double>(field.values.size() - valid) / static_cast<double>(field.values.size());
    fields.push_back({static_cast<int>(forecast), field.name,
                      std::move(field.values), std::move(field.mask)});
  }
  WriteRegularLatLonGrib2(grid, start, fields, output);
  auto inspection = InspectGrib(output);
  inspection["wave_coverage"] = coverage;
  const auto scan = ScanGribMessages(output);
  const std::size_t expected = times.size() * 3;
  if (scan.message_count != expected) throw ValidationError("Copernicus wave GRIB message count mismatch");
  return {input, output, scan.message_count, scan.byte_count, inspection};
}

WaveConvertResult GenerateCopernicusGlobalWaves(
    const BoundingBox& bbox, TimePoint start, int hours, int step_hours,
    const std::string& username, const std::string& password,
    const std::filesystem::path& output,
    std::optional<double> grid_spacing_deg, bool overwrite,
    BinaryDownload download, CredentialValidator validate_credentials,
    double timeout_seconds) {
  bbox.Validate();
  if (step_hours != 3) throw ValidationError("Copernicus Global Waves supports 3-hour steps");
  if (hours < 0 || hours > 240) throw ValidationError("Copernicus Global Waves hours must be between 0 and 240");
  if (username.empty() || password.empty()) throw ValidationError("Copernicus username and password are required");
  if (std::filesystem::exists(output) && !overwrite) throw ValidationError("output already exists; enable overwrite");
  if (!download)
    download = BinaryDownload(MakeRetryingHttpGet(
        {}, "Copernicus Global Waves", {}, {5, 1000, 8000}));
  if (!validate_credentials) validate_credentials = ValidateCopernicusCredentials;
  if (!validate_credentials(username, password, timeout_seconds))
    throw ValidationError("invalid Copernicus username or password");
  const auto dataset = DiscoverArcoDataset(
      "GLOBAL_ANALYSISFORECAST_WAV_001_027",
      "cmems_mod_glo_wav_anfc_0.083deg_PT3H-i", username, download,
      timeout_seconds);
  const auto times = BuildTimeSequence(start, hours, step_hours);
  const double spacing = grid_spacing_deg.value_or(0.08333333333333333);
  const auto grid = BuildRegularGrid(bbox, spacing);
  auto arco = ReadArcoFields(dataset, {"VHM0", "VTPK", "VMDR"}, bbox,
                             times, grid, username, download, timeout_seconds);
  const std::map<std::string, std::string> names{{"VHM0", "swh"}, {"VTPK", "perpw"}, {"VMDR", "dirpw"}};
  std::vector<Grib2Field> fields;
  Json::Value coverage(Json::objectValue);
  for (const auto& [source_name, output_name] : names) {
    for (std::size_t t = 0; t < times.size(); ++t) {
      auto& source = arco[source_name][t];
      std::size_t missing = source.mask.empty() ? 0 :
          std::count_if(source.mask.begin(), source.mask.end(), [](auto value) { return value != 0; });
      if (missing == source.values.size()) throw ValidationError("Copernicus wave field has no valid cells: " + source_name);
      const auto forecast = std::chrono::duration_cast<std::chrono::hours>(times[t] - start).count();
      const auto key = "f" + std::to_string(forecast) + "_" + output_name;
      coverage[key]["valid_cell_count"] = Json::UInt64(source.values.size() - missing);
      coverage[key]["missing_percent"] = 100.0 * missing / source.values.size();
      fields.push_back({static_cast<int>(forecast), output_name,
                        std::move(source.values), std::move(source.mask)});
    }
  }
  WriteRegularLatLonGrib2(grid, start, fields, output);
  auto inspection = InspectGrib(output);
  inspection["wave_coverage"] = coverage;
  inspection["copernicus_dataset_version"] = dataset.version_id;
  inspection["copernicus_metadata_root"] = dataset.metadata_root;
  const auto scan = ScanGribMessages(output);
  if (scan.message_count != times.size() * 3) throw ValidationError("Copernicus wave GRIB message count mismatch");
  return {{}, output, scan.message_count, scan.byte_count, inspection};
}

}  // namespace environmental_grib
