#include <cmath>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <json/json.h>
#include <sodium.h>

#include "environmental_grib/geo.h"
#include "environmental_grib/tpxo.h"
#include "environmental_grib/xtd_package.h"
#include "xtd_test_support.h"

namespace eg = environmental_grib;

namespace {
std::string Sha256(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("could not hash authored XTD file");
  crypto_hash_sha256_state state;
  crypto_hash_sha256_init(&state);
  std::array<unsigned char, 1024 * 1024> buffer{};
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    const auto count = input.gcount();
    if (count > 0)
      crypto_hash_sha256_update(&state, buffer.data(),
                                static_cast<unsigned long long>(count));
  }
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  crypto_hash_sha256_final(&state, digest.data());
  static constexpr char hex[] = "0123456789abcdef";
  std::string result(digest.size() * 2, '0');
  for (std::size_t index = 0; index < digest.size(); ++index) {
    result[index * 2] = hex[digest[index] >> 4];
    result[index * 2 + 1] = hex[digest[index] & 15];
  }
  return result;
}

void WriteProvenance(const std::filesystem::path& xtdt,
                     const eg::TpxoHeightCache& cache, double spacing) {
  Json::Value provenance(Json::objectValue);
  provenance["schema"] = "xtdt-provenance";
  provenance["schema_version"] = 1;
  provenance["xtdt_file"] = xtdt.filename().string();
  provenance["xtdt_sha256"] = Sha256(xtdt);
  provenance["xtdt_bytes"] = Json::UInt64(std::filesystem::file_size(xtdt));
  provenance["package_id"] = eg::InspectXtdPackage(xtdt)["package_id"];
  provenance["created_utc"] = eg::FormatUtcDateTime(
      std::chrono::floor<std::chrono::seconds>(
          std::chrono::system_clock::now()));
  provenance["product"] = "derived astronomical water-level harmonics";
  provenance["authoring_source"]["model"] = "TPXO10-atlas-v2-nc";
  provenance["authoring_source"]["role"] = "offline derivation only";
  provenance["distribution"]["contains_raw_tpxo"] = false;
  provenance["distribution"]["runtime_requires_tpxo"] = false;
  provenance["distribution"]["permission_boundary"] =
      "Raw TPXO data is neither distributed nor required. The source model "
      "was used only to derive the XTD coefficient grid.";
  provenance["grid"]["west"] = cache.bbox.west;
  provenance["grid"]["south"] = cache.bbox.south;
  provenance["grid"]["east"] = cache.bbox.east;
  provenance["grid"]["north"] = cache.bbox.north;
  provenance["grid"]["spacing_deg"] = spacing;
  provenance["grid"]["nx"] = Json::UInt64(cache.grid.longitudes.size());
  provenance["grid"]["ny"] = Json::UInt64(cache.grid.latitudes.size());
  provenance["interpolation"]["primary"] = "bilinear";
  provenance["interpolation"]["coastal_fallback"] =
      "nearest wet model node";
  provenance["interpolation"]["maximum_fallback_distance_deg"] = 0.25;
  provenance["interpolation"]["reason"] =
      "Represent narrow estuaries and channels absent from the ocean-model "
      "land mask; accuracy remains subject to station validation.";
  provenance["quantization_scale_m"] = 0.0005;
  provenance["vertical_datum"]["id"] = cache.vertical_datum_id;
  provenance["vertical_datum"]["name"] = cache.vertical_datum_name;
  provenance["vertical_datum"]["warning"] =
      "Model mean sea level is not Chart Datum. Do not use these absolute "
      "heights for adjusted soundings without a separately validated datum "
      "transformation.";
  for (const auto& constituent : cache.constituents)
    provenance["constituents"].append(constituent);
  const auto path = std::filesystem::path(xtdt.string() + ".prv");
  std::ofstream output(path);
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "  ";
  output << Json::writeString(writer, provenance);
  if (!output) throw std::runtime_error("could not write XTD provenance sidecar");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 8) {
    std::cerr << "usage: environmental_grib_tpxo_height_author "
                 "MODEL_DIR WEST SOUTH EAST NORTH SPACING_DEG OUTPUT.xtdt\n";
    return 2;
  }
  try {
    const eg::BoundingBox bbox{std::stod(argv[2]), std::stod(argv[3]),
                               std::stod(argv[4]), std::stod(argv[5])};
    const double spacing = std::stod(argv[6]);
    const auto grid = eg::BuildRegularGrid(bbox, spacing);
    const auto cache = eg::LoadTpxo10AtlasHeightModel(argv[1], bbox, grid);
    const auto points = grid.size();

    eg::test::XtdV2FixtureOptions options;
    options.randomize_crypto = true;
    options.include_tide = false;
    options.include_residual = false;
    options.include_uncertainty = false;
    options.include_height = true;
    options.outer_metadata["package_profile"] = "xtidal-tide-v1";
    options.outer_metadata["recommended_extension"] = ".xtdt";
    options.outer_metadata["capabilities"]["water_level_harmonics"] = true;
    options.outer_metadata["capabilities"]["tidal_current_harmonics"] = false;
    options.outer_metadata["capabilities"]["water_level_quality"] = false;
    options.outer_metadata["capabilities"]["tidal_current_quality"] = false;
    options.tide.nx = static_cast<std::uint32_t>(grid.longitudes.size());
    options.tide.ny = static_cast<std::uint32_t>(grid.latitudes.size());
    options.tide.west = bbox.west;
    options.tide.south = bbox.south;
    options.tide.east = bbox.east;
    options.tide.north = bbox.north;
    options.tide.lon_u0 = options.tide.lon_v0 = grid.longitudes.front();
    options.tide.lat_u0 = options.tide.lat_v0 = grid.latitudes.front();
    options.tide.lon_step = options.tide.lat_step = spacing;
    options.tile_width = 64;
    options.tile_height = 64;
    // Half-millimetre quantisation covers extreme global tidal amplitudes
    // while remaining far below source-model and gauge uncertainty.
    options.quantization_scale = 0.0005F;
    options.height_reference_level_m = 0.0;
    options.height_datum_id = cache.vertical_datum_id;
    options.height_datum_name = cache.vertical_datum_name;
    options.height_constituents = cache.constituents;
    options.valid = [&cache, points](std::uint32_t x, std::uint32_t y) {
      const auto point = static_cast<std::size_t>(y) *
                             cache.grid.longitudes.size() +
                         x;
      for (std::size_t constituent = 0;
           constituent < cache.constituents.size(); ++constituent) {
        const auto value = cache.height_m[constituent * points + point];
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
          return false;
      }
      return true;
    };
    options.height_value =
        [&cache, points](std::size_t field, std::uint32_t x,
                         std::uint32_t y) {
          const auto point = static_cast<std::size_t>(y) *
                                 cache.grid.longitudes.size() +
                             x;
          const auto value = cache.height_m[(field / 2) * points + point];
          if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
            return 0.0;
          return field % 2 == 0 ? value.real() : value.imag();
    };
    eg::test::WriteXtdV2Fixture(argv[7], options);
    WriteProvenance(argv[7], cache, spacing);
    const auto inspection = eg::InspectXtdPackage(argv[7]);
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    std::cout << Json::writeString(writer, inspection);
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
