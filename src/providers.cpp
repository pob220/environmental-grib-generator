#include "environmental_grib/providers.h"

#include <algorithm>

#include "environmental_grib/error.h"
#include "environmental_grib/rtofs.h"

namespace environmental_grib {

bool Provider::SupportsBbox(const BoundingBox& bbox) const {
  if (id == "noaa_rtofs_global") {
    try {
      (void)RtofsRegionForBbox(bbox);
      return implemented;
    } catch (const ValidationError&) {
      return false;
    }
  }
  return !coverage.has_value() ? implemented : coverage->Contains(bbox);
}

ProviderRegistry::ProviderRegistry() {
  providers_ = {
      {.id = "marine_ie_irish_sea",
       .label = "Marine Institute Ireland Irish Sea currents",
       .coverage = BoundingBox{-6.994, 51.506, -4.006, 55.494},
       .variables = {"49", "50"},
       .implemented = true,
       .resolution = "ready-made Irish Sea current GRIB",
       .description = "Ready-made OpenCPN-compatible Irish Sea current GRIB "
                      "from Marine Institute Ireland.",
       .default_step_hours = 1,
       .provider_type = "direct_current_grib",
       .nominal_duration_hours = 72,
       .max_duration_hours = 72,
       .source_url =
           "ftp://ftp.marine.ie/OSS/modelling/GRIB_Files/irish_sea_ms.grb"},
      {.id = "copernicus_nws",
       .label = "Copernicus Marine North-West Shelf high-resolution currents",
       .coverage = BoundingBox{-20.0, 40.0, 13.0, 65.0},
       .dataset_id = "cmems_mod_nws_phy-cur_anfc_1.5km-2D_PT1H-i",
       .variables = {"eastward_sea_water_velocity",
                     "northward_sea_water_velocity"},
       .implemented = true,
       .resolution = "approx 1.5 km",
       .description = "Modelled North-West European shelf currents including "
                      "tides/residuals.",
       .product_id = "NWSHELF_ANALYSISFORECAST_PHY_004_013"},
      {.id = "copernicus_global",
       .label = "Copernicus Marine Global currents",
       .coverage = BoundingBox{-180.0, -80.0, 180.0, 90.0},
       .dataset_id = "cmems_mod_glo_phy_anfc_0.083deg_PT1H-m",
       .variables = {"uo", "vo"},
       .implemented = true,
       .resolution = "about 0.083 degrees / 1/12 degree",
       .description = "Global Copernicus Ocean Physics Analysis and Forecast "
                      "surface currents via native ARCO chunk decoding.",
       .product_id = "GLOBAL_ANALYSISFORECAST_PHY_001_024",
       .default_step_hours = 1,
       .minimum_depth = 0.0,
       .maximum_depth = 0.5,
       .source_grid_regularity_tolerance = 5e-5},
      {.id = "copernicus_ibi",
       .label = "Copernicus Marine IBI high-resolution currents",
       .coverage = BoundingBox{-19.0828411, 26.16535726, 5.084567,
                               56.08294177},
       .dataset_id = "cmems_mod_ibi_phy_anfc_0.027deg-2D_PT1H-m",
       .variables = {"uo", "vo"},
       .implemented = true,
       .resolution = "about 0.028 degrees / 1/36 degree",
       .description =
           "Hourly Iberia-Biscay-Ireland surface currents including tides, "
           "surge, atmospheric forcing and river discharge.",
       .product_id = "IBI_ANALYSISFORECAST_PHY_005_001",
       .default_step_hours = 1,
       .provider_type = "regional_model"},
      {.id = "copernicus_mediterranean",
       .label = "Copernicus Marine Mediterranean currents",
       .coverage = BoundingBox{-17.2916661, 30.1875, 36.291668, 45.979168},
       .dataset_id = "cmems_mod_med_phy-cur_anfc_4.2km-2D_PT1H-m",
       .variables = {"uo", "vo"},
       .implemented = true,
       .resolution = "about 0.042 degrees / 4.2 km",
       .description =
           "Hourly Mediterranean surface currents including tidal and "
           "non-tidal model processes.",
       .product_id = "MEDSEA_ANALYSISFORECAST_PHY_006_013",
       .default_step_hours = 1,
       .provider_type = "regional_model"},
      {.id = "noaa_rtofs_global",
       .label = "NOAA RTOFS Global ocean currents",
       .dataset_id = "rtofs",
       .variables = {"u", "v"},
       .implemented = true,
       .resolution = "about 1/12 degree native HYCOM grid",
       .description =
           "NOAA Real-Time Ocean Forecast System global model via public "
           "regional NetCDF files for US East, US West, and Alaska.",
       .product_id = "NOAA/NCEP RTOFS",
       .default_step_hours = 6,
       .minimum_depth = 0.0,
       .maximum_depth = 0.0,
       .provider_type = "model",
       .nominal_duration_hours = 192,
       .max_duration_hours = 192,
       .source_url =
           "https://nomads.ncep.noaa.gov/pub/data/nccf/com/rtofs/prod/"},
      {.id = "noaa_ofs_s111",
       .label = "NOAA OFS / S-111 coastal currents",
       .dataset_id = "noaa_ofs_s111",
       .variables = {"surfaceCurrentSpeed", "surfaceCurrentDirection"},
       .implemented = false,
       .resolution = "regional NOAA OFS/S-111 product native grids",
       .description =
           "Experimental discovery only; not yet a complete generator.",
       .provider_type = "experimental_discovery",
       .source_url = "https://registry.opendata.aws/noaa-nos-ofs/"},
      {.id = "local_netcdf",
       .label = "Local NetCDF file",
       .implemented = true,
       .resolution = "source file native grid or requested output grid",
       .description = "User-selected local NetCDF current file."},
      {.id = "tpxo",
       .label = "TPXO10 astronomical tidal currents",
       .coverage = BoundingBox{-180.0, -90.0, 180.0, 90.0},
       .implemented = true,
       .resolution = "licensed TPXO10 Atlas v2 model grid",
       .description = "Native regional interpolation and harmonic prediction "
                      "from user-supplied TPXO10 Atlas v2 NetCDF files.",
       .provider_type = "local_licensed_model"},
      {.id = "tpxo-cache",
       .label = "Prepared TPXO astronomical tidal-current cache",
       .implemented = true,
       .resolution = "prepared cache grid",
       .description = "Native prediction from a portable cache derived from "
                      "user-supplied licensed TPXO model files.",
       .provider_type = "local_harmonic_cache"},
      {.id = "offline-tidal",
       .label = "Offline current (.xtd)",
       .coverage = BoundingBox{-180.0, -90.0, 180.0, 90.0},
       .variables = {"49", "50"},
       .implemented = true,
       .resolution = "package-declared global grids",
       .description =
           "Offline tidal streams and optional expected seasonal global-current "
           "overlay from a curated multi-source xGRIB XTD package.",
       .product_id = "xgrib-global-currents.xtd",
       .default_step_hours = 1,
       .provider_type = "local_xtd_current_package"},
      {.id = "synthetic",
       .label = "Synthetic test source",
       .coverage = BoundingBox{-180.0, -90.0, 180.0, 90.0},
       .implemented = true,
       .resolution = "requested grid",
       .description = "Offline deterministic test source."},
  };
}

const Provider& ProviderRegistry::Get(const std::string& id) const {
  const auto found =
      std::find_if(providers_.begin(), providers_.end(),
                   [&](const Provider& value) { return value.id == id; });
  if (found == providers_.end())
    throw ValidationError("unknown provider: " + id);
  return *found;
}

const Provider* SelectBestProviderForBbox(const BoundingBox& bbox,
                                          std::optional<int> duration_hours,
                                          const ProviderRegistry& registry) {
  const Provider& marine = registry.Get("marine_ie_irish_sea");
  if (marine.implemented && marine.SupportsBbox(bbox) &&
      (!duration_hours || !marine.max_duration_hours ||
       *duration_hours <= *marine.max_duration_hours)) {
    return &marine;
  }
  const Provider& nws = registry.Get("copernicus_nws");
  if (nws.implemented && nws.SupportsBbox(bbox)) return &nws;
  const Provider& global = registry.Get("copernicus_global");
  return global.implemented && global.SupportsBbox(bbox) ? &global : nullptr;
}

const Provider& SelectCopernicusProvider(const std::string& provider_id,
                                         const BoundingBox& bbox,
                                         const ProviderRegistry& registry) {
  if (provider_id == "auto") {
    const Provider& nws = registry.Get("copernicus_nws");
    if (nws.implemented && nws.SupportsBbox(bbox)) return nws;
    const Provider& global = registry.Get("copernicus_global");
    if (global.implemented && global.SupportsBbox(bbox)) return global;
    throw ValidationError(
        "no implemented Copernicus provider supports the requested bbox");
  }
  const Provider& provider = registry.Get(provider_id);
  if (provider.id.rfind("copernicus_", 0) != 0)
    throw ValidationError(provider_id + " is not a Copernicus provider");
  if (!provider.implemented)
    throw ValidationError(provider.label + " is not implemented");
  if (!provider.SupportsBbox(bbox))
    throw ValidationError(provider.label +
                          " does not support the requested bbox");
  return provider;
}

}  // namespace environmental_grib
