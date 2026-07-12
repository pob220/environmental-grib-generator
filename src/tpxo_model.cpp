#include "environmental_grib/tpxo.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <netcdf.h>

#include "environmental_grib/error.h"
#include "environmental_grib/geo.h"
#include "environmental_grib/grib.h"

namespace environmental_grib {
namespace {

void NcCheck(int status, const std::string& context) {
  if (status != NC_NOERR) throw ValidationError(context + ": " + nc_strerror(status));
}

class NcFile {
 public:
  explicit NcFile(const std::filesystem::path& path) : path_(path) {
    NcCheck(nc_open(path.c_str(), NC_NOWRITE, &id_), "could not open " + path.string());
  }
  ~NcFile() { if (id_ >= 0) nc_close(id_); }
  NcFile(const NcFile&) = delete;
  NcFile& operator=(const NcFile&) = delete;
  int Var(const std::string& name) const {
    int id=-1; NcCheck(nc_inq_varid(id_,name.c_str(),&id),"missing variable " + name + " in " + path_.string()); return id;
  }
  std::vector<std::size_t> Shape(int variable) const {
    int ndims=0; NcCheck(nc_inq_varndims(id_,variable,&ndims),"could not inspect NetCDF variable");
    std::vector<int> dimensions(ndims); NcCheck(nc_inq_vardimid(id_,variable,dimensions.data()),"could not inspect NetCDF dimensions");
    std::vector<std::size_t> shape(ndims);
    for (int i=0;i<ndims;++i) NcCheck(nc_inq_dimlen(id_,dimensions[i],&shape[i]),"could not inspect NetCDF dimension length");
    return shape;
  }
  std::vector<double> Vector(const std::string& name) const {
    const int variable=Var(name); const auto shape=Shape(variable);
    if (shape.size()!=1) throw ValidationError(name + " must be one-dimensional");
    std::vector<double> values(shape[0]); NcCheck(nc_get_var_double(id_,variable,values.data()),"could not read " + name);
    return values;
  }
  std::vector<double> Slab(const std::string& name,
                           const std::vector<std::size_t>& start,
                           const std::vector<std::size_t>& count) const {
    const int variable=Var(name); const auto shape=Shape(variable);
    if (shape.size()!=2 || start.size()!=2 || count.size()!=2) throw ValidationError(name + " must be two-dimensional");
    std::vector<double> values(count[0]*count[1]);
    NcCheck(nc_get_vara_double(id_,variable,start.data(),count.data(),values.data()),"could not read " + name);
    double scale=1.0,offset=0.0,fill=std::numeric_limits<double>::quiet_NaN();
    nc_get_att_double(id_,variable,"scale_factor",&scale); nc_get_att_double(id_,variable,"add_offset",&offset);
    if (nc_get_att_double(id_,variable,"_FillValue",&fill)!=NC_NOERR) nc_get_att_double(id_,variable,"missing_value",&fill);
    for (auto& value:values) {
      if (std::isfinite(fill) && value==fill) value=std::numeric_limits<double>::quiet_NaN();
      else value=value*scale+offset;
    }
    return values;
  }
  std::string Text(const std::string& name) const {
    const int variable=Var(name); const auto shape=Shape(variable);
    std::size_t count=1; for(auto n:shape) count*=n;
    std::string value(count,'\0'); NcCheck(nc_get_var_text(id_,variable,value.data()),"could not read " + name);
    while(!value.empty() && (value.back()=='\0' || value.back()==' ')) value.pop_back();
    return value;
  }
 private:
  std::filesystem::path path_; int id_{-1};
};

struct AxisWindow { std::size_t begin{},count{}; std::vector<double> values; };

AxisWindow Window(const std::vector<double>& axis, double minimum,
                  double maximum, const std::string& name) {
  if (axis.size()<2 || !std::is_sorted(axis.begin(),axis.end()))
    throw ValidationError("TPXO " + name + " axis must be increasing");
  auto lower=std::lower_bound(axis.begin(),axis.end(),minimum);
  auto upper=std::upper_bound(axis.begin(),axis.end(),maximum);
  std::size_t first=lower==axis.begin()?0:static_cast<std::size_t>(lower-axis.begin()-1);
  std::size_t last=upper==axis.end()?axis.size()-1:static_cast<std::size_t>(upper-axis.begin());
  if (last<=first) throw ValidationError("requested area is outside TPXO " + name + " coverage");
  const auto first_it=axis.begin()+static_cast<std::ptrdiff_t>(first);
  const auto last_it=axis.begin()+static_cast<std::ptrdiff_t>(last+1);
  return {first,last-first+1,std::vector<double>(first_it,last_it)};
}

struct RegionalField {
  std::vector<double> x,y;
  std::vector<std::complex<double>> values;  // y,x
};

std::size_t Index2(const std::vector<std::size_t>& shape,
                   std::size_t a,std::size_t b) { return a*shape[1]+b; }

RegionalField ReadComponent(const std::filesystem::path& grid_path,
                            const std::filesystem::path& model_path,
                            const std::string& component,
                            const BoundingBox& bbox) {
  const std::string& suffix=component;
  NcFile grid(grid_path),model(model_path);
  auto grid_x=grid.Vector("lon_"+suffix), grid_y=grid.Vector("lat_"+suffix);
  auto xall=model.Vector("lon_"+suffix), yall=model.Vector("lat_"+suffix);
  if (grid_x.size()!=xall.size() || grid_y.size()!=yall.size())
    throw ValidationError("TPXO model and bathymetry coordinate sizes differ");
  for (std::size_t i=0;i<xall.size();++i)
    if (std::abs(grid_x[i]-xall[i])>1e-10)
      throw ValidationError("TPXO model and bathymetry longitudes differ");
  for (std::size_t i=0;i<yall.size();++i)
    if (std::abs(grid_y[i]-yall[i])>1e-10)
      throw ValidationError("TPXO model and bathymetry latitudes differ");
  const bool zero_to_360=xall.back()>180.0;
  std::vector<std::pair<AxisWindow,double>> xwindows;
  if(zero_to_360 && bbox.west<0.0 && bbox.east>=0.0) {
    xwindows.emplace_back(Window(xall,bbox.west+360.0,xall.back(),"longitude"),-360.0);
    xwindows.emplace_back(Window(xall,xall.front(),bbox.east,"longitude"),0.0);
  } else {
    double west=bbox.west,east=bbox.east;
    if(zero_to_360 && west<0.0) { west+=360.0; east+=360.0; }
    xwindows.emplace_back(Window(xall,west,east,"longitude"),zero_to_360&&west>180.0?-360.0:0.0);
  }
  const auto yw=Window(yall,bbox.south,bbox.north,"latitude");
  const auto bath_shape=grid.Shape(grid.Var("h"+suffix));
  bool xy=bath_shape.size()==2 && bath_shape[0]==xall.size() && bath_shape[1]==yall.size();
  bool yx=bath_shape.size()==2 && bath_shape[0]==yall.size() && bath_shape[1]==xall.size();
  if(!xy&&!yx) throw ValidationError("TPXO bathymetry dimensions do not match coordinate axes");
  const auto model_shape=model.Shape(model.Var(suffix+"Re"));
  if(model_shape!=bath_shape) throw ValidationError("TPXO transport and bathymetry dimensions differ");
  std::vector<RegionalField> pieces;
  for(const auto& [xw,shift]:xwindows) {
    const std::vector<std::size_t> start=xy?std::vector<std::size_t>{xw.begin,yw.begin}:std::vector<std::size_t>{yw.begin,xw.begin};
    const std::vector<std::size_t> count=xy?std::vector<std::size_t>{xw.count,yw.count}:std::vector<std::size_t>{yw.count,xw.count};
    const auto depth=grid.Slab("h"+suffix,start,count);
    const auto real=model.Slab(suffix+"Re",start,count),imag=model.Slab(suffix+"Im",start,count);
    RegionalField piece{xw.values,yw.values,std::vector<std::complex<double>>(xw.count*yw.count)};
    for(auto& x:piece.x) x+=shift;
    for(std::size_t y=0;y<yw.count;++y) for(std::size_t x=0;x<xw.count;++x) {
      const std::size_t source=xy?Index2(count,x,y):Index2(count,y,x);
      const std::size_t target=y*xw.count+x;
      if(!std::isfinite(depth[source]) || depth[source]==0 || !std::isfinite(real[source]) || !std::isfinite(imag[source]))
        piece.values[target]={NAN,NAN};
      else piece.values[target]={real[source]/depth[source],imag[source]/depth[source]};
    }
    pieces.push_back(std::move(piece));
  }
  if(pieces.size()==1) return std::move(pieces.front());
  RegionalField result; result.y=yw.values;
  for(const auto& piece:pieces) result.x.insert(result.x.end(),piece.x.begin(),piece.x.end());
  result.values.reserve(result.x.size()*result.y.size());
  for(std::size_t y=0;y<result.y.size();++y)
    for(const auto& piece:pieces)
      result.values.insert(
          result.values.end(),
          piece.values.begin()+static_cast<std::ptrdiff_t>(y*piece.x.size()),
          piece.values.begin()+static_cast<std::ptrdiff_t>((y+1)*piece.x.size()));
  return result;
}

std::complex<double> Bilinear(const RegionalField& source,double lon,double lat) {
  auto bracket=[](const std::vector<double>& axis,double value)
      -> std::optional<std::pair<std::size_t,std::size_t>> {
    if(value<axis.front()||value>axis.back()) return std::nullopt;
    if(value==axis.front()) return std::pair<std::size_t,std::size_t>{0,0};
    if(value==axis.back()) return std::pair<std::size_t,std::size_t>{axis.size()-1,axis.size()-1};
    const auto upper=std::upper_bound(axis.begin(),axis.end(),value);
    const auto high=static_cast<std::size_t>(upper-axis.begin());
    return std::pair<std::size_t,std::size_t>{high-1,high};
  };
  const auto xb=bracket(source.x,lon),yb=bracket(source.y,lat);
  if(!xb||!yb) return {NAN,NAN};
  const auto [x0,x1]=*xb; const auto [y0,y1]=*yb;
  const auto q00=source.values[y0*source.x.size()+x0],q10=source.values[y0*source.x.size()+x1];
  const auto q01=source.values[y1*source.x.size()+x0],q11=source.values[y1*source.x.size()+x1];
  if(!std::isfinite(q00.real())||!std::isfinite(q10.real())||!std::isfinite(q01.real())||!std::isfinite(q11.real())) return {NAN,NAN};
  const double fx=x0==x1?0.0:(lon-source.x[x0])/(source.x[x1]-source.x[x0]);
  const double fy=y0==y1?0.0:(lat-source.y[y0])/(source.y[y1]-source.y[y0]);
  return q00*(1-fx)*(1-fy)+q10*fx*(1-fy)+q01*(1-fx)*fy+q11*fx*fy;
}

std::vector<std::filesystem::path> ModelFiles(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> files;
  for(const auto& entry:std::filesystem::directory_iterator(directory)) {
    const auto name=entry.path().filename().string();
    if(entry.is_regular_file() && name.starts_with("u_") && name.ends_with("_tpxo10_atlas_30_v2.nc")) files.push_back(entry.path());
  }
  std::sort(files.begin(),files.end());
  if(files.empty()) throw ValidationError("no TPXO10 atlas constituent files found in "+directory.string());
  return files;
}

}  // namespace

std::filesystem::path ResolveTpxo10AtlasDirectory(
    const std::filesystem::path& model_directory) {
  const auto direct_grid = model_directory / "grid_tpxo10atlas_v2.nc";
  const auto nested = model_directory / "TPXO10_atlas_v2";
  const auto nested_grid = nested / "grid_tpxo10atlas_v2.nc";
  std::filesystem::path directory;
  if (std::filesystem::is_regular_file(direct_grid)) {
    directory = model_directory;
  } else if (std::filesystem::is_regular_file(nested_grid)) {
    directory = nested;
  } else {
    throw ValidationError(
        "TPXO10 grid file not found; select either the model parent or "
        "TPXO10_atlas_v2 directory (checked " + direct_grid.string() +
        " and " + nested_grid.string() + ")");
  }
  ModelFiles(directory);
  return directory;
}

TpxoCache LoadTpxo10AtlasModel(const std::filesystem::path& model_directory,
                               const BoundingBox& bbox,
                               const RegularGrid& output_grid) {
  bbox.Validate();
  const auto directory=ResolveTpxo10AtlasDirectory(model_directory);
  const auto grid_file=directory/"grid_tpxo10atlas_v2.nc";
  const auto files=ModelFiles(directory);
  TpxoCache cache; cache.bbox=bbox; cache.grid=output_grid;
  cache.metadata["format"]="tidal-current-grib-generator-tpxo-cache";
  cache.metadata["format_version"]=1; cache.metadata["model_name"]="TPXO10-atlas-v2-nc";
  cache.metadata["corrections"]="ATLAS"; cache.metadata["grid_spacing_deg"]=output_grid.spacing_deg;
  cache.metadata["bbox"]["west"]=bbox.west; cache.metadata["bbox"]["south"]=bbox.south;
  cache.metadata["bbox"]["east"]=bbox.east; cache.metadata["bbox"]["north"]=bbox.north;
  cache.metadata["model_files"]=Json::arrayValue;
  {
    const auto stat_path=[&](const std::filesystem::path& path) {
      Json::Value record(Json::objectValue);
      record["name"]=path.filename().string();
      record["relative_path"]=
          std::filesystem::relative(path,directory.parent_path()).string();
      record["size"]=Json::UInt64(std::filesystem::file_size(path));
      const auto ticks=std::filesystem::last_write_time(path).time_since_epoch().count();
      record["mtime"]=Json::Int64(ticks);
      cache.metadata["model_files"].append(record);
    };
    stat_path(grid_file);
    for(const auto& file:files) stat_path(file);
  }
  const std::size_t points=output_grid.size();
  for(const auto& file:files) {
    NcFile model(file); const std::string constituent=model.Text("con");
    const auto u=ReadComponent(grid_file,file,"u",bbox),v=ReadComponent(grid_file,file,"v",bbox);
    cache.constituents.push_back(constituent);
    for(double lat:output_grid.latitudes) for(double lon:output_grid.longitudes) cache.u_cm_s.push_back(Bilinear(u,lon,lat));
    for(double lat:output_grid.latitudes) for(double lon:output_grid.longitudes) cache.v_cm_s.push_back(Bilinear(v,lon,lat));
    if(cache.u_cm_s.size()!=cache.constituents.size()*points || cache.v_cm_s.size()!=cache.constituents.size()*points)
      throw ValidationError("internal TPXO interpolation size mismatch");
  }
  return cache;
}

TpxoGenerationResult GenerateFromTpxo10AtlasModel(const TpxoModelRequest& request) {
  if(std::filesystem::exists(request.output)&&!request.overwrite) throw ValidationError("output already exists; enable overwrite to replace it");
  const auto grid=BuildRegularGrid(request.bbox,request.grid_spacing_deg);
  const auto cache=LoadTpxo10AtlasModel(request.model_directory,request.bbox,grid);
  const auto fields=PredictTpxoCache(cache,BuildTimeSequence(request.start,request.hours,request.step_hours),request.infer_minor);
  WriteGrib1Currents(fields,request.output); const auto scanned=ScanGribMessages(request.output);
  return {request.output,scanned.message_count,std::filesystem::file_size(request.output),InspectGrib(request.output)};
}

Json::Value PrepareTpxo10Cache(const std::filesystem::path& model_directory,
                               const BoundingBox& bbox,double grid_spacing_deg,
                               const std::filesystem::path& output,
                               bool overwrite) {
  const auto grid=BuildRegularGrid(bbox,grid_spacing_deg);
  auto cache=LoadTpxo10AtlasModel(model_directory,bbox,grid);
  cache.metadata["notice"]="Derived from local licensed TPXO model files. Do not redistribute unless your TPXO licence permits it.";
  cache.metadata["created_utc"]=FormatUtcDateTime(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
  cache.metadata["constituents"]=Json::arrayValue;
  for(const auto& constituent:cache.constituents) cache.metadata["constituents"].append(constituent);
  cache.metadata["minor_constituents"]=Json::arrayValue;
  cache.metadata["pyTMD_version"]=Json::nullValue;
  WriteTpxoCache(output,cache,overwrite);
  auto result=InspectTpxoCache(output); result["cache_file"]=output.string();
  return result;
}

}  // namespace environmental_grib
