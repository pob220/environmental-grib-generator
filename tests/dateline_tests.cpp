#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <vector>
#include <blosc.h>
#include <eccodes.h>
#include <netcdf.h>
#include "environmental_grib/arco.h"
#include "environmental_grib/copernicus.h"
#include "environmental_grib/environment.h"
#include "environmental_grib/error.h"
#include "environmental_grib/gfs_fallback.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/netcdf.h"
#include "environmental_grib/platform.h"
#include "environmental_grib/providers.h"
#include "environmental_grib/sources.h"
#include "environmental_grib/tpxo.h"
#include "environmental_grib/waves.h"
#include "environmental_grib/xtd_package.h"
#include "xtd_test_support.h"
namespace eg = environmental_grib;
namespace {
constexpr double pi = 3.14159265358979323846;
const auto start = eg::ParseUtcDateTime("2026-10-04T12:00:00Z");
const eg::BoundingBox box{170, -1, -170, 1};
void Check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void Nc(int error) { if (error) throw std::runtime_error(nc_strerror(error)); }
template<class F> void Reject(F f, const char* message) {
  bool rejected = false; try { f(); } catch (const eg::ValidationError&) { rejected = true; }
  Check(rejected, message);
}
std::vector<unsigned char> Bytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::vector<unsigned char> JsonBytes(const Json::Value& value) {
  auto text = Json::writeString(Json::StreamWriterBuilder{}, value);
  return {text.begin(), text.end()};
}
void WaveAndCurrentNetCDF(const std::filesystem::path& path, std::vector<double> longitude,
    const std::string& origin = "hours since 2026-10-04 12:00:00", const std::string& calendar = "") {
  int file, x, y, t, lon, lat, time;
  Nc(nc_create(eg::PathToUtf8(path).c_str(), NC_CLOBBER, &file));
  Nc(nc_def_dim(file, "longitude", longitude.size(), &x)); Nc(nc_def_dim(file, "latitude", 3, &y));
  Nc(nc_def_dim(file, "time", 3, &t));
  Nc(nc_def_var(file, "longitude", NC_DOUBLE, 1, &x, &lon));
  Nc(nc_def_var(file, "latitude", NC_DOUBLE, 1, &y, &lat));
  Nc(nc_def_var(file, "time", NC_DOUBLE, 1, &t, &time));
  Nc(nc_put_att_text(file, time, "units", origin.size(), origin.c_str()));
  if (!calendar.empty()) Nc(nc_put_att_text(file,time,"calendar",calendar.size(),calendar.c_str()));
  const int dims[]{t,y,x}; std::map<std::string,int> variables;
  for (const auto& name : {"uo", "vo", "VHM0", "VTPK", "VMDR"}) {
    Nc(nc_def_var(file, name, NC_DOUBLE, 3, dims, &variables[name]));
    std::string units = name == std::string("VMDR") ? "degrees" : name == std::string("VTPK") ? "s" : name == std::string("VHM0") ? "m" : "m/s";
    Nc(nc_put_att_text(file, variables[name], "units", units.size(), units.c_str()));
  }
  Nc(nc_enddef(file)); const double latitudes[]{-1,0,1}, times[]{0,3,6};
  Nc(nc_put_var_double(file,lon,longitude.data())); Nc(nc_put_var_double(file,lat,latitudes)); Nc(nc_put_var_double(file,time,times));
  for (const auto& [name,var] : variables) {
    std::vector<double> values;
    for (int frame=0;frame<3;++frame) for (int row=0;row<3;++row) for (double l : longitude) {
      double value = name == "uo" ? 2+std::cos(l*pi/180) : name == "vo" ? .5 : name == "VHM0" ? 2 : name == "VTPK" ? 8 : (eg::Longitude180(l) < 0 ? 359 : 1);
      values.push_back(value);
    }
    Nc(nc_put_var_double(file,var,values.data()));
  }
  Nc(nc_close(file));
}
void CheckContinuous(const std::filesystem::path& path, std::size_t messages) {
  const auto inspected = eg::InspectGrib(path);
  Check(inspected["message_count"].asUInt64() == messages, "one continuous record per field/time");
  for (const auto& message : inspected["messages"]) {
    const auto& grid = message["grid"];
    Check(std::abs(grid["west"].asDouble()-170) < 1e-6 && grid["east"].asDouble() >= 189.75-1e-6,
          "both date-line halves retained");
    Check(message["values"]["missing_count"].asUInt64() == 0, "no artificial seam masks");
  }
}
void RewriteGrib(const std::filesystem::path& input, const std::filesystem::path& output,
                 const std::function<void(codes_handle*)>& edit) {
  const auto close=[](FILE* f) { if (f) std::fclose(f); };
  std::unique_ptr<FILE,decltype(close)> file(eg::OpenFileForReading(input),close);
  Check(bool(file),"open fixture for metadata mutation");
  std::ofstream out(output,std::ios::binary);
  for (;;) {
    int error=0;
    std::unique_ptr<codes_handle,decltype(&codes_handle_delete)> handle(
        codes_handle_new_from_file(nullptr,file.get(),PRODUCT_GRIB,&error),&codes_handle_delete);
    if (!handle) { Check(!error,"read fixture metadata"); break; }
    edit(handle.get()); const void* data=nullptr; std::size_t length=0;
    Check(!codes_get_message(handle.get(),&data,&length),"encode fixture mutation");
    out.write(static_cast<const char*>(data),length);
  }
  Check(bool(out),"write fixture mutation");
}
// A spatially chunked global ARCO store, including direction values 359/1.
struct ArcoFixture {
  eg::ArcoDataset dataset;
  bool padded;
  ArcoFixture(bool padded_chunks = true) : padded(padded_chunks) {
    dataset.dataset_id = "fixture"; dataset.service_url = "https://test.invalid/store";
    auto& asset = dataset.item["assets"]["timeChunked"]; asset["href"] = dataset.service_url;
    for (const auto& variable : {"uo","vo","VHM0","VTPK","VMDR"}) {
      auto& metadata = dataset.item["properties"]["cube:variables"][variable];
      for (const auto& dimension : {"time","latitude","longitude"}) metadata["dimensions"].append(dimension);
      metadata["missingValue"] = -9999.0;
      asset["viewVariables"][variable]["dtype"] = "<f4";
      for (const auto& name : {"time","latitude","longitude"}) {
        auto& dim = asset["viewDims"][name]; auto& coords = dim["coords"];
        coords["type"] = "minMaxStep";
        if (name == std::string("longitude")) { coords["min"]=-180; coords["step"]=45; coords["len"]=8; dim["chunkLen"][variable]=3; }
        else if (name == std::string("latitude")) { coords["min"]=-1; coords["step"]=1; coords["len"]=3; dim["chunkLen"][variable]=3; }
        else { coords["min"]=Json::Int64(start.time_since_epoch().count()*1000); coords["step"]=10800000; coords["len"]=3; dim["chunkLen"][variable]=1; }
      }
    }
  }
  std::vector<unsigned char> Download(const std::string& url, double) const {
    if (url.find("clients-config") != std::string::npos) return JsonBytes(Json::Value(Json::objectValue));
    if (url.find("product.stac") != std::string::npos) {
      Json::Value product;
      for (const auto& id : {"cmems_mod_glo_phy_anfc_0.083deg_PT1H-m", "cmems_mod_glo_wav_anfc_0.083deg_PT3H-i"}) {
        Json::Value link; link["rel"]="item"; link["href"]=std::string(id)+".dataset.stac.json"; product["links"].append(link);
      }
      return JsonBytes(product);
    }
    if (url.find("dataset.stac") != std::string::npos) return JsonBytes(dataset.item);
    std::string variable;
    for (const auto& name : {"uo","vo","VHM0","VTPK","VMDR"}) if (url.find(std::string("/")+name+"/") != std::string::npos) variable=name;
    Check(!variable.empty(), "unexpected ARCO URL");
    auto suffix=url.substr(url.rfind('/')+1); suffix=suffix.substr(0,suffix.find('?'));
    const int chunk=std::stoi(suffix.substr(suffix.rfind('.')+1));
    std::vector<float> data;
    const int columns = padded ? 3 : std::min(3,8-chunk*3);
    for (int y=0;y<3;++y) for (int x=0;x<columns;++x) {
      const int column=chunk*3+x; const double l=-180+column*45;
      data.push_back(column >= 8 ? -9999 : variable=="uo" ? 2+std::cos(l*pi/180) : variable=="vo" ? .5 : variable=="VHM0" ? 2 : variable=="VTPK" ? 8 : column==0 ? 359 : 1);
    }
    std::vector<unsigned char> encoded(data.size()*sizeof(float)+BLOSC_MAX_OVERHEAD);
    const int size=blosc_compress_ctx(5,1,sizeof(float),data.size()*sizeof(float),data.data(),encoded.data(),encoded.size(),"blosclz",0,1);
    Check(size>0,"ARCO compression"); encoded.resize(size); return encoded;
  }
};
}
int main(int argc,char** argv) {
  try {
    const auto root=argc>1 ? std::filesystem::path(argv[1])/"dateline" : std::filesystem::temp_directory_path()/("xgrib-dateline-"+std::to_string(eg::ProcessId()));
    std::filesystem::create_directories(root);
    box.Validate(); Check(box.Width()==20,"wrapped width");
    Check(eg::BoundingBox{-180,-90,180,90}.Contains(box),"global contains crossing box");
    Check(!eg::BoundingBox{-20,-90,20,90}.Contains(box),"regional bounds exclude crossing box");
    Check(eg::BoundingBox{160,-5,-160,5}.Contains(box),"wrapped coverage contains wrapped request");
    Reject([] { eg::BoundingBox{180,-1,-180,1}.Validate(); },"zero-width seam rejected");
    Reject([] { eg::BoundingBox{10,-1,10,1}.Validate(); },"equal endpoints rejected");
    Check(eg::BoundingBox{10,-1,-10,1}.Width()==340,"wide crossing is not inverted");
    const auto grid=eg::BuildRegularGrid(box,.25);
    Check(grid.nx()==81 && grid.longitudes.back()==190,"continuous target grid");
    const auto irregular=eg::BuildRegularGrid({179,-1,-178.7,1},.5);
    for (std::size_t x=1;x<irregular.nx();++x) Check(std::abs(irregular.longitudes[x]-irregular.longitudes[x-1]-irregular.longitude_spacing_deg)<1e-10,"off-grid endpoints still encode a regular grid");
    // Longitude representation never changes UTC, including local dates on
    // opposite sides of the line and real UTC calendar rollovers.
    const auto east_local = eg::ParseUtcDateTime("2026-10-05T00:00:00+12:00");
    const auto west_local = eg::ParseUtcDateTime("2026-10-04T00:00:00-12:00");
    Check(east_local == west_local && east_local == start,
          "different local dates on either side of the line identify the same UTC instant");
    Reject([] { eg::ParseUtcDateTime("2026-02-29T00:00:00Z"); },"invalid leap date rejected");
    const std::vector<std::pair<std::string,std::vector<std::string>>> rollovers{
      {"2026-10-06T23:00:00Z",{"20261006T2300","20261007T0200","20261007T0500"}},
      {"2026-12-31T23:00:00Z",{"20261231T2300","20270101T0200","20270101T0500"}},
      {"2024-02-28T23:00:00Z",{"20240228T2300","20240229T0200","20240229T0500"}},
      {"2024-02-29T23:00:00Z",{"20240229T2300","20240301T0200","20240301T0500"}}};
    for (std::size_t n=0;n<rollovers.size();++n) {
      const auto reference=eg::ParseUtcDateTime(rollovers[n].first);
      std::vector<eg::Grib2Field> frames;
      std::vector<eg::CurrentGrid> current_frames;
      for (int lead : {0,3,6}) {
        frames.push_back({lead,"10u",std::vector<double>(grid.size(),3+lead),{}});
        frames.push_back({lead,"10v",std::vector<double>(grid.size(),1),{}});
        frames.push_back({lead,"swh",std::vector<double>(grid.size(),2),{}});
        current_frames.push_back({reference+std::chrono::hours(lead),grid,
          std::vector<double>(grid.size(),.5),std::vector<double>(grid.size(),.1),{}});
      }
      const auto w=root/("rollover-weather-"+std::to_string(n)+".grb2"),
                 c=root/("rollover-current-"+std::to_string(n)+".grb");
      eg::WriteRegularLatLonGrib2(grid,reference,frames,w);
      eg::WriteGrib1Currents(current_frames,c);
      eg::EnvironmentalMergeRequest req;req.weather=w;req.current=c;
      req.output=root/("rollover-"+std::to_string(n)+".grb");req.overwrite=true;
      const auto merged=eg::MergeEnvironmentalGribs(req);
      Check(merged.success,"crossing calendar-rollover merge");CheckContinuous(req.output,15);
      const auto& times=merged.output_inspection["valid_times"];
      Check(times.size()==3,"no phantom day introduced at the longitude seam");
      for (std::size_t i=0;i<3;++i) Check(times[Json::ArrayIndex(i)].asString()==rollovers[n].second[i],
          "GRIB1 currents and GRIB2 weather/waves retain the same exact UTC rollover");
    }
    for (const auto& origin : {"hours since 2026-10-05 00:00:00 +12:00",
                             "hours since 2026-10-04 00:00:00 -12:00",
                             "hours since 2026-10-04 12:00:00 UTC",
                             "hours since 2026-10-05 00:00 +12",
                             "hours since 2026-10-04 00:00:00 -1200",
                             "hours since 2026-10-04 09:00 -3:00"}) {
      const auto path=root/"offset-time.nc";WaveAndCurrentNetCDF(path,{-180,-135,-90,-45,0,45,90,135},origin,"gregorian");
      eg::NetCDFCurrentSource source(path,{});
      Check(source.GetCurrentGrid(box,start,grid).mask.empty(),"CF signed offset normalizes the local date to UTC");
      Check(source.GetCurrentGrid(box,start+std::chrono::hours(6),grid).mask.empty(),"CF forecast cadence retained after offset normalization");
    }
    const auto noncivil=root/"noncivil.nc";
    WaveAndCurrentNetCDF(noncivil,{-180,-135,-90,-45,0,45,90,135},"hours since 2026-10-04 12:00:00","360_day");
    Reject([&] { eg::NetCDFCurrentSource source(noncivil,{});(void)source.GetCurrentGrid(box,start,grid); },
           "noncivil calendar cannot silently become a Gregorian forecast");
    std::vector<std::filesystem::path> currents, weather, waves;
    for (auto longitude : {std::vector<double>{-180,-135,-90,-45,0,45,90,135}, std::vector<double>{315,270,225,180,135,90,45,0}, std::vector<double>{170,180,-170}, std::vector<double>{170,180,190}, std::vector<double>{-180,-135,-90,-45,0,45,90,135,180}}) {
      const auto file=root/("source-"+std::to_string(currents.size())+".nc"); WaveAndCurrentNetCDF(file,longitude);
      eg::NetCDFCurrentSource source(file,{});
      Check(source.SourceBounds().Contains(box),"source bounds preserve date-line coverage");
      const auto field=source.GetCurrentGrid(box,start,grid);
      Check(field.mask.empty(),"NetCDF seam interpolation has complete coverage");
      Check(std::abs(field.u_mps[grid.nx()/2]-1.0)<1e-8,"NetCDF samples 180 correctly");
      const auto output=root/("netcdf-"+std::to_string(currents.size())+".grb");
      eg::WriteGrib1Currents({field},output); CheckContinuous(output,2); currents.push_back(output);
      if (longitude.size()==3) {
        const auto source_grid=source.BuildSourceGrid(box);
        Check(std::is_sorted(source_grid.longitudes.begin(),source_grid.longitudes.end()),"source-grid columns ordered with data");
      } else Reject([&] { (void)source.BuildSourceGrid(box); },"a native subset with fewer than two columns is rejected");
      const auto wave=root/("netcdf-waves-"+std::to_string(currents.size())+".grb2");
      eg::ConvertCopernicusWaveNetCDF(file,box,start,0,3,wave,.25,true);
      CheckContinuous(wave,3);
    }
    for (bool padded : {false,true}) {
      ArcoFixture boundary(padded);
      const auto data=eg::ReadArcoFields(boundary.dataset,{"uo"},box,{start},grid,"fixture",
          [&](const auto& url,double timeout) { return boundary.Download(url,timeout); });
      Check(data.at("uo")[0].mask.empty(),"padded/clipped ARCO boundary chunk retains both halves");
      for (std::size_t y=0;y<grid.ny();++y) for (std::size_t x=0;x<grid.nx();++x)
        Check(std::abs(data.at("uo")[0].values[y*grid.nx()+x]-data.at("uo")[0].values[x])<1e-7,
              "ARCO boundary row strides preserve nonconstant values");
    }
    ArcoFixture arco;
    const auto fetch=[&](const std::string& url,double timeout) { return arco.Download(url,timeout); };
    const auto fields=eg::ReadArcoFields(arco.dataset,{"uo","VMDR"},box,{start},grid,"fixture",fetch);
    Check(fields.at("uo")[0].mask.empty(),"ARCO cyclic interpolation across spatial chunks");
    const auto middle=grid.nx()/2;
    Check(std::abs(fields.at("uo")[0].values[middle]-1)<1e-8,"ARCO seam value");
    const double angle=fields.at("VMDR")[0].values[middle-1];
    Check(angle<2 || angle>358,"wave direction follows the short arc");
    eg::CopernicusRequest cp; cp.bbox=box; cp.start=start; cp.hours=0; cp.grid_spacing_deg=.25;
    cp.username="fixture"; cp.password="fixture"; cp.provider="copernicus_global"; cp.output=root/"copernicus.grb"; cp.overwrite=true;
    eg::GenerateCopernicusGlobal(cp,fetch,[](const auto&,const auto&,double) { return true; });
    CheckContinuous(cp.output,2); currents.push_back(cp.output);
    const auto cop_wave=root/"copernicus-waves.grb2";
    eg::GenerateCopernicusGlobalWaves(box,start,0,3,"fixture","fixture",cop_wave,.25,true,fetch,[](const auto&,const auto&,double) { return true; });
    CheckContinuous(cop_wave,3); waves.push_back(cop_wave);
    const auto wave_inspection = eg::InspectGrib(cop_wave);
    for (const auto& message : wave_inspection["messages"])
      Check(message["level_type"].asString()=="surface" && message["level"].asInt()==0,
            "generated waves have a surface level with older and newer ecCodes");
    for (int version : {1,2}) {
      eg::test::XtdFixtureOptions options; options.lon_u0=options.lon_v0=-180; options.west=-180; options.east=180;
      const auto package=root/("v"+std::to_string(version)+".xtd");
      if (version==1) eg::test::WriteXtdFixture(package,options);
      else { eg::test::XtdV2FixtureOptions v2; v2.tide=options; v2.include_height=true; eg::test::WriteXtdV2Fixture(package,v2); }
      eg::EnvironmentRequest request; request.bbox=box; request.start=start; request.hours=0;
      request.weather_provider="none"; request.current_source="offline-tidal"; request.offline_tidal_file=package;
      request.current_grid_spacing_deg=.25; request.output=root/("xtd-"+std::to_string(version)+".grb"); request.overwrite=true;
      eg::GenerateEnvironment(request); CheckContinuous(request.output,2); currents.push_back(request.output);
      if (version==2) {
        request.offline_current_mode="tide-expected-seasonal"; request.output=root/"xtd-seasonal.grb";
        eg::GenerateEnvironment(request); CheckContinuous(request.output,2); currents.push_back(request.output);
        eg::XtdPackageReader reader(package);
        const auto heights=reader.PredictHeight(grid,{start},false);
        Check(heights.size()==1 && std::all_of(heights[0].height_m.begin(),heights[0].height_m.end(),[](double value) { return std::isfinite(value); }),"XTD water-level grid accepts continuous longitudes");
      }
    }
    eg::TpxoCache cache; cache.bbox=box; cache.grid=grid; cache.constituents={"m2"};
    cache.metadata["format"]="tidal-current-grib-generator-tpxo-cache"; cache.metadata["format_version"]=1;
    cache.metadata["velocity_units"]="cm/s"; cache.metadata["corrections"]="ATLAS"; cache.metadata["grid_spacing_deg"]=.25;
    for (const auto& [name,value] : std::map<std::string,double>{{"west",170},{"south",-1},{"east",-170},{"north",1}}) cache.metadata["bbox"][name]=value;
    cache.u_cm_s.assign(grid.size(),{100,20}); cache.v_cm_s.assign(grid.size(),{50,10});
    const auto cache_path=root/"current.tpxocache"; eg::WriteTpxoCache(cache_path,cache,true);
    const auto cached=root/"cached.grb"; eg::GenerateFromTpxoCache(cache_path,start,0,3,cached,false,true); CheckContinuous(cached,2); currents.push_back(cached);
    const auto native_grid=eg::BuildRegularGrid({-180,-1,180,1},.25);
    const auto global=root/"global-weather.grb2";
    eg::WriteRegularLatLonGrib2(native_grid,start,{{0,"10u",std::vector<double>(native_grid.size(),3),{}},{0,"10v",std::vector<double>(native_grid.size(),1),{}}},global);
    const auto imported=root/"imported.grb2"; eg::CropAndStitchGrib({global},box,imported); CheckContinuous(imported,2); weather.push_back(imported);
    // Two fragments must become one field; byte concatenation is insufficient.
    for (const auto& [name,area] : std::map<std::string,eg::BoundingBox>{{"west",{170,-1,180,1}},{"east",{-180,-1,-170,1}}}) {
      const auto half=eg::BuildRegularGrid(area,.25); eg::WriteRegularLatLonGrib2(half,start,{{0,"10u",std::vector<double>(half.size(),3),{}},{0,"10v",std::vector<double>(half.size(),1),{}}},root/(name+".grb2"));
    }
    const auto joined=root/"joined.grb2"; eg::CropAndStitchGrib({root/"west.grb2",root/"east.grb2"},box,joined); CheckContinuous(joined,2); weather.push_back(joined);
    Reject([&] { eg::CropAndStitchGrib({root/"west.grb2"},box,root/"incomplete.grb2"); },"a missing half cannot masquerade as complete coverage");
    Check(!std::filesystem::exists(root/"incomplete.grb2"),"failed assembly publishes no output");
    const auto half=eg::BuildRegularGrid({-180,-1,-170,1},.25);
    eg::WriteRegularLatLonGrib2(half,start,{{0,"10u",std::vector<double>(half.size(),4),{}},{0,"10v",std::vector<double>(half.size(),1),{}}},root/"conflict.grb2");
    Reject([&] { eg::CropAndStitchGrib({root/"west.grb2",root/"conflict.grb2"},box,root/"conflicting.grb2"); },"inconsistent shared seam values are rejected");
    const auto gap_grid=eg::BuildRegularGrid({-179.5,-1,-170,1},.25);
    eg::WriteRegularLatLonGrib2(gap_grid,start,{{0,"10u",std::vector<double>(gap_grid.size(),3),{}},{0,"10v",std::vector<double>(gap_grid.size(),1),{}}},root/"gap.grb2");
    Reject([&] { eg::CropAndStitchGrib({root/"west.grb2",root/"gap.grb2"},box,root/"gapped.grb2"); },"an absent longitude column is rejected");
    std::vector<std::uint8_t> mask(half.size(),0); mask[half.nx()+1]=1;
    eg::WriteRegularLatLonGrib2(half,start,{{0,"10u",std::vector<double>(half.size(),3),mask},{0,"10v",std::vector<double>(half.size(),1),{}}},root/"masked.grb2");
    eg::CropAndStitchGrib({root/"west.grb2",root/"masked.grb2"},box,root/"masked-joined.grb2");
    Check(eg::InspectGrib(root/"masked-joined.grb2")["messages"][0]["values"]["missing_count"].asUInt64()==1,"source masks survive stitching without expansion");
    RewriteGrib(root/"east.grb2",root/"wrong-cycle.grb2",[](codes_handle* h) {
      Check(!codes_set_long(h,"dataTime",1800),"set mismatched fixture cycle");
    });
    Reject([&] { eg::CropAndStitchGrib({root/"west.grb2",root/"wrong-cycle.grb2"},box,root/"cycle-mixed.grb2"); },"opposite halves from different cycles cannot be joined");
    // Nonconstant values expose column/row permutation mistakes in all scan modes.
    std::vector<double> gradient;
    for (double latitude : grid.latitudes) for (double longitude : grid.longitudes) gradient.push_back(longitude+latitude);
    eg::WriteRegularLatLonGrib2(grid,start,{{0,"10u",gradient,{}}},root/"gradient.grb2");
    for (int flags=0;flags<16;++flags) {
      RewriteGrib(root/"gradient.grb2",root/"scanned.grb2",[&](codes_handle* h) {
        const bool neg=flags&1, north=flags&2, adjacent=flags&4, alternating=flags&8;
        Check(!codes_set_long(h,"iScansNegatively",neg) && !codes_set_long(h,"jScansPositively",north) &&
              !codes_set_long(h,"jPointsAreConsecutive",adjacent) && !codes_set_long(h,"alternativeRowScanning",alternating),"set scan fixture flags");
        Check(!codes_set_double(h,"longitudeOfFirstGridPointInDegrees",neg?190:170) &&
              !codes_set_double(h,"longitudeOfLastGridPointInDegrees",neg?170:190) &&
              !codes_set_double(h,"latitudeOfFirstGridPointInDegrees",north?-1:1) &&
              !codes_set_double(h,"latitudeOfLastGridPointInDegrees",north?1:-1),"set scan fixture endpoints");
        std::vector<double> encoded(grid.size());
        for (std::size_t j=0;j<grid.ny();++j) for (std::size_t i=0;i<grid.nx();++i) {
          const auto x=neg?grid.nx()-1-i:i, y=north?j:grid.ny()-1-j;
          const auto index=adjacent ? i*grid.ny()+(alternating&&i%2?grid.ny()-1-j:j)
                                   : j*grid.nx()+(alternating&&j%2?grid.nx()-1-i:i);
          encoded[index]=gradient[y*grid.nx()+x];
        }
        Check(!codes_set_double_array(h,"values",encoded.data(),encoded.size()),"encode scan fixture gradient");
      });
      eg::CropAndStitchGrib({root/"scanned.grb2"},box,root/"scan-cropped.grb2");
      RewriteGrib(root/"scan-cropped.grb2",root/"scan-verified.grb2",[&](codes_handle* h) {
        std::vector<double> values(grid.size()); auto length=values.size();
        Check(!codes_get_double_array(h,"values",values.data(),&length),"decode scanned result");
        for (std::size_t i=0;i<values.size();++i) Check(std::abs(values[i]-gradient[i])<1e-6,"scan flags preserve spatial values");
      });
    }
    const eg::BoundingBox wide{10,-1,-10,1};
    eg::CropAndStitchGrib({global},wide,root/"wide.grb2");
    Check(eg::InspectGrib(root/"wide.grb2")["messages"][0]["grid"]["ni"].asInt()==1361,"a wide crossing retains its 340-degree extent");
    eg::GFSRequest gfs; gfs.bbox=box; gfs.hours=0; gfs.step_hours=3; gfs.cycle="12"; gfs.date="20261004"; gfs.preset="minimal"; gfs.overwrite=true; gfs.output=root/"gfs.grb2";
    Check(eg::BuildGfsFilterUrl({"20261004","12"},0,box,{}).find("rightlon=190")!=std::string::npos,"NOAA receives an eastward interval");
    eg::GenerateGfs(gfs,[&](const auto&,double) { return Bytes(global); }); CheckContinuous(gfs.output,2); weather.push_back(gfs.output);
    const auto global_bytes = Bytes(global);
    std::string index;
    std::size_t offset = 0;
    // ECMWF indexes contain one compact JSON object per line.
    Json::StreamWriterBuilder compact; compact["indentation"]=""; index.clear(); offset=0;
    for (const auto& name : {"10u","10v"}) {
      std::uint64_t length=0; for (int i=8;i<16;++i) length=(length<<8)|global_bytes[offset+i];
      Json::Value entry; entry["param"]=name; entry["type"]="fc"; entry["levtype"]="sfc"; entry["step"]=0;
      entry["_offset"]=Json::UInt64(offset); entry["_length"]=Json::UInt64(length);
      index+=Json::writeString(compact,entry)+"\n"; offset+=length;
    }
    for (bool aifs : {false,true}) {
      gfs.step_hours=aifs?6:3; gfs.output=root/(aifs?"aifs.grb2":"ifs.grb2");
      eg::GenerateEcmwfOpenData(gfs,aifs,[&](const auto&,double) { return std::vector<unsigned char>(index.begin(),index.end()); },
        [&](const auto&,std::uint64_t first,std::uint64_t last,double) { return std::vector<unsigned char>(global_bytes.begin()+first,global_bytes.begin()+last+1); });
      CheckContinuous(gfs.output,2); weather.push_back(gfs.output);
    }
    gfs.step_hours=3; gfs.output=root/"hosted-gfs.grb2";
    eg::GenerateGfs(gfs,[](const auto&,double)->std::vector<unsigned char> { throw eg::HttpDownloadError("offline",true,429); }, {}, {}, {},
      [&](const auto&,const auto&,const auto&,auto) { return Bytes(imported); });
    CheckContinuous(gfs.output,2); weather.push_back(gfs.output);
    const auto wave_global=root/"global-waves.grb2";
    eg::WriteRegularLatLonGrib2(native_grid,start,{{0,"swh",std::vector<double>(native_grid.size(),2),{}},{0,"perpw",std::vector<double>(native_grid.size(),8),{}},{0,"dirpw",std::vector<double>(native_grid.size(),1),{}}},wave_global);
    gfs.waves=true; gfs.output=root/"gfs-waves.grb2"; eg::GenerateGfs(gfs,[&](const auto&,double) { return Bytes(wave_global); }); CheckContinuous(gfs.output,3); waves.push_back(gfs.output);
    // Exercise orchestration, checkpoints and native cadence at a handover.
    eg::WriteRegularLatLonGrib2(grid,start,{{0,"10u",std::vector<double>(grid.size(),3),{}},{0,"10v",std::vector<double>(grid.size(),1),{}},
        {3,"10u",std::vector<double>(grid.size(),3),{}},{3,"10v",std::vector<double>(grid.size(),1),{}}},root/"preferred.grb2");
    eg::EnvironmentRequest extension;
    extension.bbox=box; extension.start=start; extension.hours=6; extension.step_hours=3;
    extension.date="20261004"; extension.cycle="12"; extension.weather_preset="minimal";
    extension.weather_provider="existing-file"; extension.weather_file=root/"preferred.grb2";
    extension.extend_forecast=true; extension.fallback_weather_provider="gfs";
    extension.current_source="offline-tidal"; extension.offline_tidal_file=root/"v2.xtd";
    extension.current_grid_spacing_deg=.5; extension.output=root/"extended.grb"; extension.overwrite=true;
    const auto extension_result=eg::GenerateEnvironment(extension,[&](const std::string& url,double) {
      const auto marker=url.find(".pgrb2.0p25.f"); Check(marker!=std::string::npos,"extension uses selected GFS fallback");
      const int hour=std::stoi(url.substr(marker+13,3));
      const auto coarse=eg::BuildRegularGrid(box,.5);
      const auto frame=root/("fallback-frame-"+std::to_string(hour)+".grb2");
      eg::WriteRegularLatLonGrib2(coarse,start,{{hour,"10u",std::vector<double>(coarse.size(),4),{}},{hour,"10v",std::vector<double>(coarse.size(),1),{}}},frame);
      return Bytes(frame);
    },start+std::chrono::hours(6));
    Check(extension_result.message_count==12,"extension retains preferred frames and both halves of the fallback: "+Json::writeString(Json::StreamWriterBuilder{},extension_result.diagnostics));
    CheckContinuous(extension.output,12);
    // All combinations of the generated component streams, with optional roles.
    weather.emplace_back(); waves.emplace_back(); currents.emplace_back(); std::size_t combinations=0;
    for (const auto& w : weather) for (const auto& a : waves) for (const auto& c : currents) {
      if (w.empty() && a.empty() && c.empty()) continue;
      eg::EnvironmentalMergeRequest request; if (!w.empty()) request.weather=w; if (!a.empty()) request.waves=a; if (!c.empty()) request.current=c;
      request.output=root/"combined.grb"; request.overwrite=true;
      const auto result=eg::MergeEnvironmentalGribs(request); Check(result.success,"viable component combination merges");
      CheckContinuous(request.output,(w.empty()?0:2)+(a.empty()?0:3)+(c.empty()?0:2)); ++combinations;
    }
    eg::EnvironmentalMergeRequest final; final.weather=weather[0]; final.waves=waves[0]; final.current=currents[0]; final.output=root/"reader.grb"; final.overwrite=true;
    Check(eg::MergeEnvironmentalGribs(final).success,"reader fixture");
    eg::ProviderRegistry providers;
    for (const auto& name : {"marine_ie_irish_sea","copernicus_nws","copernicus_ibi","copernicus_mediterranean","noaa_rtofs_global"}) Check(!providers.Get(name).SupportsBbox(box),"regional provider rejects uncovered crossing");
    std::cout << "Date-line geometry, source interpolation, GRIB stitching and " << combinations << " component combinations passed\n";
  } catch (const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
