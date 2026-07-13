#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
#include <netcdf.h>
#include <bzlib.h>
#include <blosc.h>

#include "environmental_grib/error.h"
#include "environmental_grib/environment.h"
#include "environmental_grib/copernicus.h"
#include "environmental_grib/geo.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/job.h"
#include "environmental_grib/model.h"
#include "environmental_grib/netcdf.h"
#include "environmental_grib/providers.h"
#include "environmental_grib/remote_currents.h"
#include "environmental_grib/rtofs.h"
#include "environmental_grib/security.h"
#include "environmental_grib/sources.h"
#include "environmental_grib/tpxo.h"
#include "environmental_grib/weather.h"
#include "environmental_grib/waves.h"
#include "environmental_grib/ukv.h"

namespace eg = environmental_grib;

namespace {
int failures = 0;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

template <typename Function>
void ExpectValidation(Function function, const std::string& message) {
  try {
    function();
    Check(false, message);
  } catch (const eg::ValidationError&) {
  }
}

bool Near(double first, double second, double tolerance = 1e-10) {
  return std::abs(first - second) <= tolerance;
}

void Nc(int status, const std::string& action) {
  if (status != NC_NOERR) throw std::runtime_error(action + ": " + nc_strerror(status));
}

void WriteNetCDFFixture(const std::filesystem::path& path, const std::string& units) {
  int file = -1;
  Nc(nc_create(path.c_str(), NC_CLOBBER, &file), "create fixture");
  int time_dim = -1, lat_dim = -1, lon_dim = -1;
  Nc(nc_def_dim(file, "time", 2, &time_dim), "time dimension");
  Nc(nc_def_dim(file, "latitude", 3, &lat_dim), "latitude dimension");
  Nc(nc_def_dim(file, "longitude", 3, &lon_dim), "longitude dimension");
  int time_var = -1, lat_var = -1, lon_var = -1, u_var = -1, v_var = -1;
  Nc(nc_def_var(file, "time", NC_DOUBLE, 1, &time_dim, &time_var), "time variable");
  Nc(nc_def_var(file, "latitude", NC_DOUBLE, 1, &lat_dim, &lat_var), "latitude variable");
  Nc(nc_def_var(file, "longitude", NC_DOUBLE, 1, &lon_dim, &lon_var), "longitude variable");
  const int dims[] = {time_dim, lat_dim, lon_dim};
  Nc(nc_def_var(file, "eastward_sea_water_velocity", NC_DOUBLE, 3, dims, &u_var), "u variable");
  Nc(nc_def_var(file, "northward_sea_water_velocity", NC_DOUBLE, 3, dims, &v_var), "v variable");
  const std::string time_units = "hours since 2026-07-01 00:00:00";
  Nc(nc_put_att_text(file, time_var, "units", time_units.size(), time_units.c_str()), "time units");
  Nc(nc_put_att_text(file, u_var, "units", units.size(), units.c_str()), "u units");
  Nc(nc_put_att_text(file, v_var, "units", units.size(), units.c_str()), "v units");
  Nc(nc_enddef(file), "end definitions");
  const double times[] = {0.0, 1.0};
  const double latitudes[] = {51.5, 52.0, 52.5};
  const double longitudes[] = {-7.0, -6.5, -6.0};
  std::vector<double> u_values(18, 1.0), v_values(18, 2.0);
  Nc(nc_put_var_double(file, time_var, times), "time values");
  Nc(nc_put_var_double(file, lat_var, latitudes), "latitude values");
  Nc(nc_put_var_double(file, lon_var, longitudes), "longitude values");
  Nc(nc_put_var_double(file, u_var, u_values.data()), "u values");
  Nc(nc_put_var_double(file, v_var, v_values.data()), "v values");
  Nc(nc_close(file), "close fixture");
}

void WriteWaveFixture(const std::filesystem::path& path) {
  int file = -1;
  Nc(nc_create(path.c_str(), NC_CLOBBER, &file), "create wave fixture");
  int time_dim = -1, lat_dim = -1, lon_dim = -1;
  Nc(nc_def_dim(file, "time", 2, &time_dim), "wave time dimension");
  Nc(nc_def_dim(file, "latitude", 3, &lat_dim), "wave latitude dimension");
  Nc(nc_def_dim(file, "longitude", 3, &lon_dim), "wave longitude dimension");
  int time_var = -1, lat_var = -1, lon_var = -1;
  Nc(nc_def_var(file, "time", NC_DOUBLE, 1, &time_dim, &time_var), "wave time variable");
  Nc(nc_def_var(file, "latitude", NC_DOUBLE, 1, &lat_dim, &lat_var), "wave latitude variable");
  Nc(nc_def_var(file, "longitude", NC_DOUBLE, 1, &lon_dim, &lon_var), "wave longitude variable");
  const int dims[] = {time_dim, lat_dim, lon_dim};
  int height = -1, period = -1, direction = -1;
  Nc(nc_def_var(file, "VHM0", NC_DOUBLE, 3, dims, &height), "wave height");
  Nc(nc_def_var(file, "VTPK", NC_DOUBLE, 3, dims, &period), "wave period");
  Nc(nc_def_var(file, "VMDR", NC_DOUBLE, 3, dims, &direction), "wave direction");
  const std::string time_units = "hours since 2026-07-01 00:00:00";
  Nc(nc_put_att_text(file, time_var, "units", time_units.size(), time_units.c_str()), "wave time units");
  const std::string metres = "m", seconds = "s", degrees = "degrees";
  Nc(nc_put_att_text(file, height, "units", metres.size(), metres.c_str()), "height units");
  Nc(nc_put_att_text(file, period, "units", seconds.size(), seconds.c_str()), "period units");
  Nc(nc_put_att_text(file, direction, "units", degrees.size(), degrees.c_str()), "direction units");
  Nc(nc_enddef(file), "end wave definitions");
  const double times[] = {0.0, 3.0}, lats[] = {51.5, 52.0, 52.5}, lons[] = {-7.0, -6.5, -6.0};
  std::vector<double> heights(18, 2.0), periods(18, 8.0), directions(18, 270.0);
  Nc(nc_put_var_double(file, time_var, times), "wave times");
  Nc(nc_put_var_double(file, lat_var, lats), "wave lats");
  Nc(nc_put_var_double(file, lon_var, lons), "wave lons");
  Nc(nc_put_var_double(file, height, heights.data()), "wave heights");
  Nc(nc_put_var_double(file, period, periods.data()), "wave periods");
  Nc(nc_put_var_double(file, direction, directions.data()), "wave directions");
  Nc(nc_close(file), "close wave fixture");
}

void WriteRtofsFixture(const std::filesystem::path& path) {
  int file = -1;
  Nc(nc_create(path.c_str(), NC_CLOBBER, &file), "create RTOFS fixture");
  int mt = -1, depth = -1, y = -1, x = -1;
  Nc(nc_def_dim(file, "MT", 1, &mt), "RTOFS MT");
  Nc(nc_def_dim(file, "Depth", 1, &depth), "RTOFS depth");
  Nc(nc_def_dim(file, "Y", 3, &y), "RTOFS Y");
  Nc(nc_def_dim(file, "X", 3, &x), "RTOFS X");
  const int coordinate_dims[] = {y, x};
  const int field_dims[] = {mt, depth, y, x};
  int latitude = -1, longitude = -1, u = -1, v = -1;
  Nc(nc_def_var(file, "Latitude", NC_DOUBLE, 2, coordinate_dims, &latitude), "RTOFS latitude");
  Nc(nc_def_var(file, "Longitude", NC_DOUBLE, 2, coordinate_dims, &longitude), "RTOFS longitude");
  Nc(nc_def_var(file, "u", NC_DOUBLE, 4, field_dims, &u), "RTOFS u");
  Nc(nc_def_var(file, "v", NC_DOUBLE, 4, field_dims, &v), "RTOFS v");
  const std::string units = "m/s";
  Nc(nc_put_att_text(file, u, "units", units.size(), units.c_str()), "RTOFS u units");
  Nc(nc_put_att_text(file, v, "units", units.size(), units.c_str()), "RTOFS v units");
  Nc(nc_enddef(file), "end RTOFS definitions");
  const double latitudes[] = {30.0, 30.0, 30.0, 30.5, 30.55, 30.5, 31.0, 31.0, 31.0};
  const double longitudes[] = {-80.0, -79.5, -79.0, -80.0, -79.48, -79.0, -80.0, -79.5, -79.0};
  std::vector<double> us(9), vs(9);
  for (std::size_t i = 0; i < us.size(); ++i) {
    us[i] = longitudes[i] * 0.01 + latitudes[i] * 0.02;
    vs[i] = longitudes[i] * -0.03 + latitudes[i] * 0.01;
  }
  Nc(nc_put_var_double(file, latitude, latitudes), "RTOFS latitudes");
  Nc(nc_put_var_double(file, longitude, longitudes), "RTOFS longitudes");
  Nc(nc_put_var_double(file, u, us.data()), "RTOFS u values");
  Nc(nc_put_var_double(file, v, vs.data()), "RTOFS v values");
  Nc(nc_close(file), "close RTOFS fixture");
}

void WriteUkvFixture(const std::filesystem::path& path,
                     const std::string& variable_name,
                     const std::string& standard_name,
                     const std::string& units, double value) {
  int file = -1;
  Nc(nc_create(path.c_str(), NC_CLOBBER, &file), "create UKV fixture");
  int y_dim = -1, x_dim = -1;
  Nc(nc_def_dim(file, "projection_y_coordinate", 5, &y_dim), "UKV y dimension");
  Nc(nc_def_dim(file, "projection_x_coordinate", 5, &x_dim), "UKV x dimension");
  int y_var = -1, x_var = -1, mapping = -1, data = -1;
  Nc(nc_def_var(file, "projection_y_coordinate", NC_DOUBLE, 1, &y_dim, &y_var), "UKV y");
  Nc(nc_def_var(file, "projection_x_coordinate", NC_DOUBLE, 1, &x_dim, &x_var), "UKV x");
  const int dims[] = {y_dim, x_dim};
  Nc(nc_def_var(file, "lambert_azimuthal_equal_area", NC_INT, 0, nullptr, &mapping), "UKV mapping");
  Nc(nc_def_var(file, variable_name.c_str(), NC_DOUBLE, 2, dims, &data), "UKV field");
  const std::string x_standard = "projection_x_coordinate", y_standard = "projection_y_coordinate";
  Nc(nc_put_att_text(file, x_var, "standard_name", x_standard.size(), x_standard.c_str()), "UKV x standard name");
  Nc(nc_put_att_text(file, y_var, "standard_name", y_standard.size(), y_standard.c_str()), "UKV y standard name");
  const std::string mapping_name = "lambert_azimuthal_equal_area";
  Nc(nc_put_att_text(file, mapping, "grid_mapping_name", mapping_name.size(), mapping_name.c_str()), "UKV mapping name");
  const double lat0 = 54.9, lon0 = -2.5, zero = 0.0, semi_major = 6378137.0, inverse_flattening = 298.257223563;
  Nc(nc_put_att_double(file, mapping, "latitude_of_projection_origin", NC_DOUBLE, 1, &lat0), "UKV lat0");
  Nc(nc_put_att_double(file, mapping, "longitude_of_projection_origin", NC_DOUBLE, 1, &lon0), "UKV lon0");
  Nc(nc_put_att_double(file, mapping, "false_easting", NC_DOUBLE, 1, &zero), "UKV false easting");
  Nc(nc_put_att_double(file, mapping, "false_northing", NC_DOUBLE, 1, &zero), "UKV false northing");
  Nc(nc_put_att_double(file, mapping, "semi_major_axis", NC_DOUBLE, 1, &semi_major), "UKV semi major");
  Nc(nc_put_att_double(file, mapping, "inverse_flattening", NC_DOUBLE, 1, &inverse_flattening), "UKV inverse flattening");
  Nc(nc_put_att_text(file, data, "grid_mapping", mapping_name.size(), mapping_name.c_str()), "UKV field mapping");
  Nc(nc_put_att_text(file, data, "standard_name", standard_name.size(), standard_name.c_str()), "UKV standard name");
  Nc(nc_put_att_text(file, data, "units", units.size(), units.c_str()), "UKV units");
  Nc(nc_enddef(file), "end UKV definitions");
  const double axis[] = {-500000, -250000, 0, 250000, 500000};
  std::vector<double> values(25, value);
  Nc(nc_put_var_double(file, x_var, axis), "UKV x values");
  Nc(nc_put_var_double(file, y_var, axis), "UKV y values");
  Nc(nc_put_var_double(file, data, values.data()), "UKV field values");
  Nc(nc_close(file), "close UKV fixture");
}

std::vector<unsigned char> Bzip(const std::vector<unsigned char>& input) {
  std::vector<unsigned char> output(input.size() + input.size() / 100 + 601);
  unsigned int size = static_cast<unsigned int>(output.size());
  const int status = BZ2_bzBuffToBuffCompress(
      reinterpret_cast<char*>(output.data()), &size,
      const_cast<char*>(reinterpret_cast<const char*>(input.data())),
      static_cast<unsigned int>(input.size()), 9, 0, 30);
  if (status != BZ_OK) throw std::runtime_error("test bzip compression failed");
  output.resize(size);
  return output;
}

std::vector<unsigned char> BloscInt16(const std::vector<std::int16_t>& input) {
  std::vector<unsigned char> output(input.size() * sizeof(std::int16_t) + BLOSC_MAX_OVERHEAD);
  const int bytes = blosc_compress(5, 1, sizeof(std::int16_t),
                                   input.size() * sizeof(std::int16_t), input.data(),
                                   output.data(), output.size());
  if (bytes <= 0) throw std::runtime_error("test Blosc compression failed");
  output.resize(static_cast<std::size_t>(bytes));
  return output;
}

std::vector<unsigned char> BloscFloat(const std::vector<float>& input) {
  std::vector<unsigned char> output(input.size() * sizeof(float) + BLOSC_MAX_OVERHEAD);
  const int bytes = blosc_compress(5, 1, sizeof(float), input.size() * sizeof(float),
                                   input.data(), output.data(), output.size());
  if (bytes <= 0) throw std::runtime_error("test float Blosc compression failed");
  output.resize(static_cast<std::size_t>(bytes));
  return output;
}

std::filesystem::path Temp(const std::string& name) {
  return std::filesystem::temp_directory_path() /
         ("environmental-grib-tests-" + std::to_string(::getpid()) + "-" + name);
}
}  // namespace

int main() {
  Json::Value job_json(Json::objectValue);
  job_json["schemaVersion"] = 1;
  job_json["operation"] = "generateEnvironment";
  auto& job_request = job_json["request"];
  job_request["bbox"]["west"] = -8.5;
  job_request["bbox"]["south"] = 50.5;
  job_request["bbox"]["east"] = -2.5;
  job_request["bbox"]["north"] = 56.5;
  job_request["start"] = "2026-07-12T00:00:00Z";
  job_request["hours"] = 24;
  job_request["stepHours"] = 3;
  job_request["weatherProvider"] = "gfs";
  job_request["currentSource"] = "none";
  job_request["autoPrepareTpxoCache"] = true;
  job_request["output"] = "/tmp/environmental-grib-job-test.grb";
  job_request["overwrite"] = true;
  job_json["credentials"]["copernicusPasswordEnvironment"] = "TEST_SECRET";
  const auto parsed_job = eg::ParseGeneratorJob(job_json);
  Check(parsed_job.request.bbox.west == -8.5 &&
            parsed_job.request.hours == 24 &&
            parsed_job.request.weather_provider == "gfs" &&
            parsed_job.request.overwrite &&
            parsed_job.request.auto_prepare_tpxo_cache,
        "job protocol request mapping");
  Check(parsed_job.copernicus_password_environment == "TEST_SECRET",
        "job protocol secret environment mapping");
  const auto capabilities = eg::GeneratorCapabilitiesJson();
  Check(capabilities["schemaVersion"].asInt() == 1 &&
            capabilities["operations"][0].asString() ==
                "generateEnvironment",
        "job protocol capabilities");
  job_json["schemaVersion"] = 2;
  bool rejected_job_schema = false;
  try {
    eg::ParseGeneratorJob(job_json);
  } catch (const eg::ValidationError&) {
    rejected_job_schema = true;
  }
  Check(rejected_job_schema, "job protocol rejects unsupported schema");

  const auto tpxo_root = Temp("tpxo-layout");
  const auto tpxo_atlas = tpxo_root / "TPXO10_atlas_v2";
  std::filesystem::create_directories(tpxo_atlas);
  std::ofstream(tpxo_atlas / "grid_tpxo10atlas_v2.nc").put('\0');
  std::ofstream(tpxo_atlas / "u_m2_tpxo10_atlas_30_v2.nc").put('\0');
  Check(eg::ResolveTpxo10AtlasDirectory(tpxo_root) == tpxo_atlas,
        "TPXO parent directory resolves atlas");
  Check(eg::ResolveTpxo10AtlasDirectory(tpxo_atlas) == tpxo_atlas,
        "TPXO direct atlas directory resolves without duplication");

  eg::EnvironmentRequest invalid_tpxo;
  invalid_tpxo.bbox = {-8.5, 50.5, -2.5, 56.5};
  invalid_tpxo.start = eg::ParseUtcDateTime("2026-07-12T00:00:00Z");
  invalid_tpxo.hours = 6;
  invalid_tpxo.step_hours = 3;
  invalid_tpxo.weather_provider = "gfs";
  invalid_tpxo.current_source = "tpxo-cache";
  invalid_tpxo.input_cache = Temp("missing-cache.tpxocache");
  invalid_tpxo.auto_prepare_tpxo_cache = true;
  invalid_tpxo.tpxo_model_directory = Temp("missing-model");
  invalid_tpxo.output = Temp("invalid-tpxo-output.grb");
  invalid_tpxo.overwrite = true;
  int preflight_http_calls = 0;
  ExpectValidation(
      [&] {
        eg::GenerateEnvironment(
            invalid_tpxo,
            [&](const std::string&, double) {
              ++preflight_http_calls;
              return std::vector<unsigned char>{};
            });
      },
      "invalid TPXO model rejected before generation");
  Check(preflight_http_calls == 0,
        "invalid TPXO model rejected before weather downloads");
  std::filesystem::remove_all(tpxo_root);

  const eg::BoundingBox bbox{-1.0, 50.0, 0.0, 51.0};
  bbox.Validate();
  ExpectValidation([] { eg::BoundingBox{-4.0, 51.5, -7.0, 55.5}.Validate(); },
                   "inverted bbox rejected");
  const auto grid = eg::BuildRegularGrid(bbox, 0.5);
  Check(grid.nx() == 3 && grid.ny() == 3, "inclusive 3x3 grid");
  Check(grid.longitudes == std::vector<double>({-1.0, -0.5, 0.0}), "grid longitudes");
  const auto start = eg::ParseUtcDateTime("2026-07-01T00:00:00Z");
  Check(eg::FormatUtcDateTime(start) == "2026-07-01T00:00:00Z", "UTC round trip");
  Check(eg::BuildTimeSequence(start, 6, 3).size() == 3, "time sequence count");
  ExpectValidation([&] { eg::BuildTimeSequence(start, 5, 2); }, "non-divisible time range rejected");

  const auto [speed, direction] = eg::ComponentsToSpeedDirection(0.514444, 0.0);
  Check(Near(speed, 1.0) && Near(direction, 90.0), "component conversion eastward");
  Check(Near(eg::DirectionErrorDegrees(350.0, 10.0), -20.0), "direction error wraps");
  const auto [u, v] = eg::SpeedDirectionToComponents(2.0, 90.0, "knots");
  Check(Near(u, 1.028888) && std::abs(v) < 1e-12, "speed conversion knots");

  const auto constant = eg::MakeConstantCurrent(bbox, start, grid, 1.0, 2.0);
  Check(constant.u_mps.size() == 9 && constant.u_mps.front() == 1.0 && constant.v_mps.back() == 2.0,
        "constant source");
  const auto synthetic_a = eg::MakeSyntheticRotaryCurrent(bbox, start, grid);
  const auto synthetic_b = eg::MakeSyntheticRotaryCurrent(bbox, start, grid);
  Check(synthetic_a.u_mps == synthetic_b.u_mps && synthetic_a.v_mps == synthetic_b.v_mps,
        "synthetic source deterministic");

  eg::ProviderRegistry registry;
  Check(eg::SelectBestProviderForBbox({-6.5, 52.0, -4.5, 55.0}, 48, registry)->id == "marine_ie_irish_sea",
        "Marine.ie selected for short Irish Sea request");
  Check(eg::SelectBestProviderForBbox({-6.5, 52.0, -4.5, 55.0}, 96, registry)->id == "copernicus_nws",
        "NWS selected beyond Marine.ie duration");
  Check(!registry.Get("noaa_rtofs_global").SupportsBbox({-6.5, 52.0, -4.5, 55.0}) &&
            registry.Get("noaa_rtofs_global").SupportsBbox({-80.0, 30.0, -79.0, 31.0}),
        "RTOFS advertises only downloadable public regions");
  Check(eg::RedactText("https://x.test?a=1&password=secret") ==
            "https://x.test?a=1&password=<redacted>", "query secret redaction");

  const auto minimal = Temp("minimal.grb");
  const std::vector<unsigned char> message = {'G','R','I','B',0,0,12,1,'7','7','7','7'};
  std::ofstream(minimal, std::ios::binary).write(reinterpret_cast<const char*>(message.data()), message.size());
  Check(eg::ScanGribMessages(minimal).message_count == 1, "strict GRIB scan");
  const auto wrapped = Temp("wrapped.grb");
  const auto clean = Temp("clean.grb");
  {
    std::ofstream out(wrapped, std::ios::binary);
    out << "wrapper";
    out.write(reinterpret_cast<const char*>(message.data()), message.size());
    out << "\r\r\n";
    out.write(reinterpret_cast<const char*>(message.data()), message.size());
    out << "tail";
  }
  const auto normalized = eg::NormalizeGribStream(wrapped, clean);
  Check(normalized.message_count == 2 && normalized.clean_byte_count == 24,
        "normalize wrapped GRIB");

  const auto current_path = Temp("current.grb");
  eg::WriteGrib1Currents({constant}, current_path);
  const auto inspection = eg::InspectGrib(current_path);
  Check(inspection["message_count"].asUInt64() == 2, "ecCodes current writer messages");
  Check(inspection["current_component_counts"]["u_49"].asUInt64() == 1 &&
            inspection["current_component_counts"]["v_50"].asUInt64() == 1,
        "ecCodes current parameters");
  const auto grib2_path = Temp("fields.grb2");
  std::vector<eg::Grib2Field> grib2_fields;
  for (const auto& name : {"10u", "10v", "prmsl", "2t", "swh", "perpw", "dirpw"}) {
    grib2_fields.push_back({0, name, std::vector<double>(grid.size(), 1.5), {}});
  }
  eg::WriteRegularLatLonGrib2(grid, start, grib2_fields, grib2_path);
  const auto grib2_inspection = eg::InspectGrib(grib2_path);
  Check(grib2_inspection["message_count"].asUInt64() == 7 &&
            grib2_inspection["short_name_counts"]["10u"].asUInt64() == 1 &&
            grib2_inspection["short_name_counts"]["swh"].asUInt64() == 1,
        "generic GRIB2 weather/wave writer");

  const auto weather_fields = eg::GfsVariablesForPreset("routing");
  const eg::GFSCycle known_cycle{"20260701", "00"};
  const auto weather_url = eg::BuildGfsFilterUrl(
      known_cycle, 6, {-8.5, 50.5, -2.5, 56.5}, weather_fields);
  Check(weather_url.find("file=gfs.t00z.pgrb2.0p25.f006") != std::string::npos &&
            weather_url.find("dir=%2Fgfs.20260701%2F00%2Fatmos") != std::string::npos &&
            weather_url.find("var_UGRD=on") != std::string::npos,
        "GFS filter URL parity");
  eg::GFSRequest cycle_request{{-1, 50, 0, 51}, Temp("unused.grb"), 6};
  cycle_request.max_auto_cycles = 3;
  const auto cycles = eg::GfsCycleCandidates(cycle_request, eg::ParseUtcDateTime("2026-07-02T13:30:00Z"));
  Check(cycles.size() == 3 && cycles[0].date == "20260702" && cycles[0].cycle == "12" &&
            cycles[2].cycle == "00", "GFS automatic cycle order");
  std::ifstream source_bytes(current_path, std::ios::binary | std::ios::ate);
  std::vector<unsigned char> downloaded(static_cast<std::size_t>(source_bytes.tellg()));
  source_bytes.seekg(0);
  source_bytes.read(reinterpret_cast<char*>(downloaded.data()), static_cast<std::streamsize>(downloaded.size()));
  const auto weather_path = Temp("gfs.grb");
  eg::GFSRequest weather_request{{-8.5, 50.5, -2.5, 56.5}, weather_path, 6};
  weather_request.cycle = "00"; weather_request.date = "20260701";
  const auto generated_weather = eg::GenerateGfs(
      weather_request, [&](const std::string&, double) { return downloaded; });
  Check(generated_weather.message_count == 6 && generated_weather.urls.size() == 3,
        "GFS atomic segment assembly with injectable HTTP");
  Check(eg::DwdIconEuForecastHourSequence(6, 3) == std::vector<int>({0, 3, 6}),
        "ICON-EU forecast cadence");
  Check(eg::BuildDwdIconEuUrl({"20260701", "06"}, 3, "u_10m").find(
            "2026070106_003_U_10M.grib2.bz2") != std::string::npos,
        "ICON-EU URL parity");
  const auto icon_path = Temp("icon.grb");
  eg::GFSRequest icon_request{{-8.5, 50.5, -2.5, 56.5}, icon_path, 0};
  icon_request.cycle = "06"; icon_request.date = "20260701";
  const auto compressed_current = Bzip(downloaded);
  const auto icon = eg::GenerateDwdIconEu(
      icon_request, [&](const std::string&, double) { return compressed_current; });
  Check(icon.message_count == 8 && icon.urls.size() == 4,
        "ICON-EU decompression and atomic field assembly");
  const std::string hrrr_inventory =
      "1:0:d=20260701:UGRD:10 m above ground:anl:\n"
      "2:100:d=20260701:VGRD:10 m above ground:anl:\n"
      "3:200:d=20260701:TMP:2 m above ground:anl:\n"
      "4:300:d=20260701:PRES:surface:anl:\n"
      "5:400:d=20260701:OTHER:surface:anl:\n";
  const auto parsed_inventory = eg::ParseHrrrInventory(hrrr_inventory);
  Check(parsed_inventory.size() == 5 && parsed_inventory[1].offset == 100,
        "HRRR inventory parser");
  const auto hrrr_path = Temp("hrrr.grb");
  eg::GFSRequest hrrr_request{{-100.0, 30.0, -90.0, 40.0}, hrrr_path, 0, 1};
  hrrr_request.cycle = "06"; hrrr_request.date = "20260701";
  const auto hrrr = eg::GenerateHrrr(
      hrrr_request,
      [&](const std::string&, double) {
        return std::vector<unsigned char>(hrrr_inventory.begin(), hrrr_inventory.end());
      },
      [&](const std::string&, std::size_t, std::size_t, double) { return downloaded; });
  Check(hrrr.message_count == 8 && hrrr.urls.size() == 1,
        "HRRR indexed field assembly");
  Check(eg::BuildEcmwfDataUrl({"20260710", "00"}, 6, false) ==
            "https://data.ecmwf.int/forecasts/20260710/00z/ifs/0p25/oper/20260710000000-6h-oper-fc.grib2",
        "ECMWF Open Data URL parity");
  Check(eg::UkvForecastHours(60, 1).back() == 60 &&
            eg::UkvForecastHours(60, 1).size() == 57,
        "UKV hourly then three-hour cadence");
  Check(eg::UkvSourceKey("20260703T0000Z", 6, "wind_speed_at_10m") ==
            "uk-deterministic-2km/20260703T0000Z/20260703T0600Z-PT0006H00M-wind_speed_at_10m.nc",
        "UKV source key parity");
  std::string ecmwf_index;
  for (const auto& field : {"10u", "10v", "msl", "2t"}) {
    ecmwf_index += std::string("{\"_offset\":0,\"_length\":100,\"param\":\"") +
                   field + "\",\"type\":\"fc\",\"step\":\"0\",\"levtype\":\"sfc\"}\n";
  }
  const auto ecmwf_path = Temp("ecmwf.grb");
  eg::GFSRequest ecmwf_request{{-8.5, 50.5, -2.5, 56.5}, ecmwf_path, 0, 3};
  ecmwf_request.cycle = "00"; ecmwf_request.date = "20260710";
  const auto ecmwf = eg::GenerateEcmwfOpenData(
      ecmwf_request, false,
      [&](const std::string&, double) {
        return std::vector<unsigned char>(ecmwf_index.begin(), ecmwf_index.end());
      },
      [&](const std::string&, std::size_t, std::size_t, double) { return downloaded; });
  Check(ecmwf.message_count == 8 && ecmwf.urls.size() == 1,
        "ECMWF index and range assembly");
  auto wrapped_current = downloaded;
  wrapped_current.insert(wrapped_current.begin(), {'w', 'r', 'a', 'p'});
  wrapped_current.insert(wrapped_current.end(), {'t', 'a', 'i', 'l'});
  const auto marine_path = Temp("marine.grb");
  std::string marine_url;
  const auto marine = eg::DownloadMarineIe(
      marine_path, true,
      [&](const std::string& url, double) { marine_url = url; return wrapped_current; });
  Check(marine.inspection["current_component_counts"]["u_49"].asUInt64() == 1 &&
            marine.skipped_byte_count == 8 &&
            marine_url.find("ftpossapp2:FtpOssapp2@ftp.marine.ie") != std::string::npos,
        "Marine.ie normalisation and current validation");
  Check(eg::RtofsForecastHours(18, 6) == std::vector<int>({6, 12, 18}),
        "RTOFS native forecast cadence");
  Check(eg::RtofsRegionForBbox({-80.0, 30.0, -79.0, 31.0}) == "US_east",
        "RTOFS regional coverage selection");
  const auto rtofs_source = Temp("rtofs.nc");
  const auto rtofs_output = Temp("rtofs.grb");
  WriteRtofsFixture(rtofs_source);
  eg::RtofsRequest rtofs_request;
  rtofs_request.bbox = {-79.9, 30.1, -79.1, 30.9};
  rtofs_request.output = rtofs_output;
  rtofs_request.hours = 6;
  rtofs_request.step_hours = 6;
  rtofs_request.cycle = "00";
  rtofs_request.date = "20260701";
  rtofs_request.download_directory = Temp("rtofs-downloads");
  rtofs_request.grid_spacing_deg = 0.4;
  rtofs_request.overwrite = true;
  const auto rtofs = eg::GenerateRtofs(
      rtofs_request,
      [](const std::string&, double) {
        return std::string("rtofs_glo_3dz_f006_6hrly_hvr_US_east.nc");
      },
      [&](const std::string&, double) {
        std::ifstream input(rtofs_source, std::ios::binary | std::ios::ate);
        std::vector<unsigned char> bytes(static_cast<std::size_t>(input.tellg()));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return bytes;
      });
  Check(rtofs.message_count == 2 &&
            eg::InspectGrib(rtofs_output)["current_component_counts"]["u_49"].asUInt64() == 1,
        "RTOFS curvilinear Delaunay conversion");
  const auto copernicus_output = Temp("copernicus.grb");
  eg::CopernicusRequest copernicus_request;
  copernicus_request.bbox = {-6.9, 52.1, -6.1, 52.9};
  copernicus_request.start = start;
  copernicus_request.hours = 0;
  copernicus_request.grid_spacing_deg = 0.4;
  copernicus_request.username = "test-user";
  copernicus_request.password = "secret";
  copernicus_request.output = copernicus_output;
  copernicus_request.overwrite = true;
  const std::string copernicus_product =
      R"({"links":[{"rel":"item","href":"cmems_mod_nws_phy-cur_anfc_1.5km-2D_PT1H-i_202607/dataset.stac.json"}]})";
  const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(start.time_since_epoch()).count();
  const std::string copernicus_item =
      std::string(R"({"id":"cmems_mod_nws_phy-cur_anfc_1.5km-2D_PT1H-i_202607","assets":{"timeChunked":{"href":"https://test.invalid/nws.zarr","viewDims":{"latitude":{"coords":{"min":52.0,"max":53.0,"step":0.5,"len":3}},"longitude":{"coords":{"min":-7.0,"max":-6.0,"step":0.5,"len":3}},"time":{"coords":{"min":)") +
      std::to_string(epoch_ms) +
      R"(,"max":)" + std::to_string(epoch_ms) +
      R"(,"step":3600000,"len":1}}}}},"properties":{"cube:variables":{"uo":{"scale":0.001,"offset":0.0,"missingValue":-32768},"vo":{"scale":0.001,"offset":0.0,"missingValue":-32768}}}})";
  const auto u_chunk = BloscInt16({100, 200, 300, 200, 300, 400, 300, 400, 500});
  const auto v_chunk = BloscInt16({-100, -200, -300, -200, -300, -400, -300, -400, -500});
  const auto copernicus = eg::GenerateCopernicusNws(
      copernicus_request,
      [&](const std::string& url, double) {
        if (url.find("product.stac.json") != std::string::npos)
          return std::vector<unsigned char>(copernicus_product.begin(), copernicus_product.end());
        if (url.find("dataset.stac.json") != std::string::npos)
          return std::vector<unsigned char>(copernicus_item.begin(), copernicus_item.end());
        if (url.find("/uo/0.0.0") != std::string::npos) return u_chunk;
        if (url.find("/vo/0.0.0") != std::string::npos) return v_chunk;
        throw std::runtime_error("unexpected mocked Copernicus URL: " + url);
      },
      [](const std::string& username, const std::string& password, double) {
        return username == "test-user" && password == "secret";
      });
  Check(copernicus.message_count == 2 &&
            copernicus.dataset_version.find("202607") != std::string::npos &&
            eg::InspectGrib(copernicus_output)["current_component_counts"]["v_50"].asUInt64() == 1,
        "Copernicus dynamic STAC and Blosc Zarr conversion");
  const auto global_current_output = Temp("copernicus-global.grb");
  eg::CopernicusRequest global_request = copernicus_request;
  global_request.provider = "copernicus_global";
  global_request.output = global_current_output;
  const std::string global_product =
      R"({"links":[{"rel":"item","href":"cmems_mod_glo_phy_anfc_0.083deg_PT1H-m_202607/dataset.stac.json"}]})";
  const std::string global_item =
      std::string(R"({"id":"cmems_mod_glo_phy_anfc_0.083deg_PT1H-m_202607","assets":{"timeChunked":{"href":"https://test.invalid/global.zarr","viewDims":{"elevation":{"chunkLen":{"uo":1,"vo":1},"coords":{"type":"explicit","values":[-0.5],"len":1}},"latitude":{"chunkLen":{"uo":3,"vo":3},"coords":{"type":"minMaxStep","min":52.0,"max":53.0,"step":0.5,"len":3}},"longitude":{"chunkLen":{"uo":3,"vo":3},"coords":{"type":"minMaxStep","min":-7.0,"max":-6.0,"step":0.5,"len":3}},"time":{"chunkLen":{"uo":1,"vo":1},"coords":{"type":"minMaxStep","min":)") +
      std::to_string(epoch_ms) + R"(,"max":)" + std::to_string(epoch_ms) +
      R"(,"step":3600000,"len":1}}},"viewVariables":{"uo":{"dtype":"<f4"},"vo":{"dtype":"<f4"}}}},"properties":{"cube:variables":{"uo":{"dimensions":["time","elevation","latitude","longitude"],"scale":null,"offset":null,"missingValue":9.96921e36},"vo":{"dimensions":["time","elevation","latitude","longitude"],"scale":null,"offset":null,"missingValue":9.96921e36}}}})";
  const auto global_u = BloscFloat({.1f,.2f,.3f,.2f,.3f,.4f,.3f,.4f,.5f});
  const auto global_v = BloscFloat({-.1f,-.2f,-.3f,-.2f,-.3f,-.4f,-.3f,-.4f,-.5f});
  const auto global_current = eg::GenerateCopernicusGlobal(
      global_request,
      [&](const std::string& url, double) {
        if (url.find("product.stac.json") != std::string::npos)
          return std::vector<unsigned char>(global_product.begin(), global_product.end());
        if (url.find("dataset.stac.json") != std::string::npos)
          return std::vector<unsigned char>(global_item.begin(), global_item.end());
        if (url.find("/uo/0.0.0.0") != std::string::npos) return global_u;
        if (url.find("/vo/0.0.0.0") != std::string::npos) return global_v;
        throw std::runtime_error("unexpected mocked Global URL: " + url);
      },
      [](const std::string&, const std::string&, double) { return true; });
  Check(global_current.message_count == 2 &&
            eg::InspectGrib(global_current_output)["current_component_counts"]["u_49"].asUInt64() == 1,
        "Copernicus Global float32 four-dimensional ARCO conversion");
  const auto remote_wave_output = Temp("copernicus-remote-waves.grb2");
  const std::string wave_product =
      R"({"links":[{"rel":"item","href":"cmems_mod_glo_wav_anfc_0.083deg_PT3H-i_202607/dataset.stac.json"}]})";
  const std::string wave_item =
      std::string(R"({"id":"cmems_mod_glo_wav_anfc_0.083deg_PT3H-i_202607","assets":{"timeChunked":{"href":"https://test.invalid/waves.zarr","viewDims":{"latitude":{"chunkLen":{"VHM0":3,"VTPK":3,"VMDR":3},"coords":{"type":"minMaxStep","min":52.0,"max":53.0,"step":0.5,"len":3}},"longitude":{"chunkLen":{"VHM0":3,"VTPK":3,"VMDR":3},"coords":{"type":"minMaxStep","min":-7.0,"max":-6.0,"step":0.5,"len":3}},"time":{"chunkLen":{"VHM0":1,"VTPK":1,"VMDR":1},"coords":{"type":"minMaxStep","min":)") +
      std::to_string(epoch_ms) + R"(,"max":)" + std::to_string(epoch_ms) +
      R"(,"step":10800000,"len":1}}},"viewVariables":{"VHM0":{"dtype":"<i2"},"VTPK":{"dtype":"<i2"},"VMDR":{"dtype":"<i2"}}}},"properties":{"cube:variables":{"VHM0":{"dimensions":["time","latitude","longitude"],"scale":0.01,"offset":0,"missingValue":-32767},"VTPK":{"dimensions":["time","latitude","longitude"],"scale":0.01,"offset":0,"missingValue":-32767},"VMDR":{"dimensions":["time","latitude","longitude"],"scale":0.01,"offset":180,"missingValue":-32767}}}})";
  const auto height_chunk = BloscInt16(std::vector<std::int16_t>(9, 200));
  const auto period_chunk = BloscInt16(std::vector<std::int16_t>(9, 800));
  const auto direction_chunk = BloscInt16(std::vector<std::int16_t>(9, 9000));
  const auto remote_waves = eg::GenerateCopernicusGlobalWaves(
      eg::BoundingBox{-7.0, 51.5, -6.0, 52.5}, start, 0, 3, "test-user",
      "secret", remote_wave_output,
      0.5, true,
      [&](const std::string& url, double) {
        if (url.find("product.stac.json") != std::string::npos)
          return std::vector<unsigned char>(wave_product.begin(), wave_product.end());
        if (url.find("dataset.stac.json") != std::string::npos)
          return std::vector<unsigned char>(wave_item.begin(), wave_item.end());
        if (url.find("/VHM0/0.0.0") != std::string::npos) return height_chunk;
        if (url.find("/VTPK/0.0.0") != std::string::npos) return period_chunk;
        if (url.find("/VMDR/0.0.0") != std::string::npos) return direction_chunk;
        throw std::runtime_error("unexpected mocked wave URL: " + url);
      },
      [](const std::string&, const std::string&, double) { return true; });
  Check(remote_waves.message_count == 3 &&
            eg::InspectGrib(remote_wave_output)["short_name_counts"]["dirpw"].asUInt64() == 1,
        "Copernicus Global packed wave ARCO conversion");
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
  const auto ukv_pressure = Temp("ukv-pressure.nc");
  const auto ukv_temperature = Temp("ukv-temperature.nc");
  const auto ukv_speed = Temp("ukv-speed.nc");
  const auto ukv_direction = Temp("ukv-direction.nc");
  const auto ukv_output = Temp("ukv.grb2");
  WriteUkvFixture(ukv_pressure, "pressure_at_mean_sea_level", "air_pressure_at_mean_sea_level", "Pa", 101500.0);
  WriteUkvFixture(ukv_temperature, "temperature_at_screen_level", "air_temperature", "K", 285.0);
  WriteUkvFixture(ukv_speed, "wind_speed_at_10m", "wind_speed", "m s-1", 10.0);
  WriteUkvFixture(ukv_direction, "wind_direction_at_10m", "wind_from_direction", "degree", 270.0);
  auto bytes_from = [](const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
  };
  eg::UkvRequest ukv_request;
  ukv_request.bbox = {-5.8, 53.0, -5.2, 53.5};
  ukv_request.output = ukv_output;
  ukv_request.hours = 0;
  ukv_request.cycle = "00";
  ukv_request.date = "20260703";
  ukv_request.grid_spacing_deg = 0.1;
  ukv_request.overwrite = true;
  const auto ukv = eg::GenerateUkv(
      ukv_request,
      [&](const std::string& url, double) {
        if (url.find("pressure_at_mean_sea_level") != std::string::npos) return bytes_from(ukv_pressure);
        if (url.find("temperature_at_screen_level") != std::string::npos) return bytes_from(ukv_temperature);
        if (url.find("wind_speed_at_10m") != std::string::npos) return bytes_from(ukv_speed);
        if (url.find("wind_direction_at_10m") != std::string::npos) return bytes_from(ukv_direction);
        throw std::runtime_error("unexpected UKV URL");
      });
  Check(ukv.message_count == 4 &&
            eg::InspectGrib(ukv_output)["short_name_counts"]["10u"].asUInt64() == 1,
        "UKV projected NetCDF regrid and meteorological wind conversion");
#endif
  const auto environment_path = Temp("environment.grb");
  eg::EnvironmentRequest environment;
  environment.bbox = {-8.5, 50.5, -2.5, 56.5};
  environment.start = start;
  environment.hours = 6;
  environment.step_hours = 3;
  environment.cycle = "00";
  environment.date = "20260701";
  environment.weather_provider = "gfs";
  environment.current_source = "existing-file";
  environment.current_file = current_path;
  environment.output = environment_path;
  environment.overwrite = true;
  const auto environment_result = eg::GenerateEnvironment(
      environment, [&](const std::string&, double) { return downloaded; });
  Check(environment_result.message_count == 8 &&
            environment_result.current_source == "existing-file" &&
            environment_result.selected_cycle == "20260701T0000Z",
        "combined environment orchestration and deterministic merge");
  const auto merged_path = Temp("merged.grb");
  const auto merged = eg::MergeGribStreams({{"a", current_path}, {"b", current_path}}, merged_path, true);
  Check(merged.output_message_count == 4, "atomic stream merge");

  const auto netcdf_path = Temp("currents.nc");
  WriteNetCDFFixture(netcdf_path, "cm/s");
  eg::NetCDFCurrentSource netcdf(netcdf_path);
  const eg::BoundingBox netcdf_bbox{-7.0, 51.5, -6.0, 52.5};
  const auto netcdf_grid = eg::BuildRegularGrid(netcdf_bbox, 0.5);
  const auto netcdf_current = netcdf.GetCurrentGrid(netcdf_bbox, start, netcdf_grid);
  Check(netcdf_current.u_mps.size() == 9 && Near(netcdf_current.u_mps.front(), 0.01) &&
            Near(netcdf_current.v_mps.back(), 0.02), "NetCDF detection and cm/s conversion");
  const auto source_grid = netcdf.BuildSourceGrid(netcdf_bbox);
  Check(source_grid.nx() == 3 && source_grid.ny() == 3 && Near(source_grid.spacing_deg, 0.5),
        "NetCDF native source grid");
  const auto netcdf_inspection = netcdf.Inspect();
  Check(netcdf_inspection["detected_u_variable"].asString() == "eastward_sea_water_velocity",
        "NetCDF inspection detects u variable");
  const auto wave_netcdf = Temp("waves.nc");
  const auto wave_grib = Temp("waves.grb2");
  WriteWaveFixture(wave_netcdf);
  const auto converted_waves = eg::ConvertCopernicusWaveNetCDF(
      wave_netcdf, netcdf_bbox, start, 3, 3, wave_grib, 0.5, true);
  Check(converted_waves.message_count == 6 &&
            converted_waves.inspection["short_name_counts"]["swh"].asUInt64() == 2 &&
            converted_waves.inspection["short_name_counts"]["dirpw"].asUInt64() == 2,
        "Copernicus wave NetCDF conversion");

  const auto wave_only_path = Temp("wave-only-environment.grb");
  eg::EnvironmentRequest wave_only;
  wave_only.bbox = netcdf_bbox;
  wave_only.start = start;
  wave_only.hours = 0;
  wave_only.step_hours = 1;
  wave_only.wave_step_hours = 1;
  wave_only.cycle = "00";
  wave_only.date = "20260701";
  wave_only.weather_provider = "none";
  wave_only.include_waves = true;
  wave_only.wave_provider = "gfs_wave";
  wave_only.current_source = "none";
  wave_only.output = wave_only_path;
  wave_only.overwrite = true;
  const auto wave_only_result = eg::GenerateEnvironment(
      wave_only, [&](const std::string&, double) { return bytes_from(wave_grib); });
  Check(wave_only_result.message_count == 6 && wave_only_result.inputs.size() == 1 &&
            wave_only_result.wave_provider == "gfs_wave",
        "wave generation remains independent of weather selection");

  for (const auto& path : {minimal, wrapped, clean, current_path, grib2_path, merged_path, netcdf_path, wave_netcdf, wave_grib, wave_only_path, weather_path, icon_path, hrrr_path, ecmwf_path, marine_path, rtofs_source, rtofs_output, copernicus_output, global_current_output, remote_wave_output, environment_path}) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
#ifdef ENVIRONMENTAL_GRIB_HAVE_PROJ
  for (const auto& path : {ukv_pressure, ukv_temperature, ukv_speed, ukv_direction, ukv_output}) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
#endif
  std::cout << "environmental_grib_tests failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
