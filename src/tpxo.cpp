#include "environmental_grib/tpxo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>

#include <zip.h>

#include "environmental_grib/error.h"
#include "environmental_grib/geo.h"
#include "environmental_grib/grib.h"

namespace environmental_grib {
namespace {

constexpr double kPi = std::numbers::pi_v<double>;
constexpr double kMjdTideEpoch = 48622.0;

struct NpyArray {
  std::string descriptor;
  std::vector<std::size_t> shape;
  std::vector<std::byte> data;
};

std::uint16_t U16(const std::byte* p) {
  return std::to_integer<std::uint8_t>(p[0]) |
         (std::to_integer<std::uint8_t>(p[1]) << 8);
}

std::uint32_t U32(const std::byte* p) {
  return std::to_integer<std::uint8_t>(p[0]) |
         (std::to_integer<std::uint8_t>(p[1]) << 8) |
         (std::to_integer<std::uint8_t>(p[2]) << 16) |
         (std::to_integer<std::uint8_t>(p[3]) << 24);
}

std::vector<std::byte> ReadZipMember(zip_t* archive,
                                     const std::string& member) {
  zip_stat_t stat{};
  zip_stat_init(&stat);
  if (zip_stat(archive, member.c_str(), 0, &stat) != 0) {
    throw ValidationError("TPXO cache is missing " + member);
  }
  if (stat.size > 1024ULL * 1024ULL * 1024ULL) {
    throw ValidationError("TPXO cache member is unreasonably large: " + member);
  }
  zip_file_t* file = zip_fopen(archive, member.c_str(), 0);
  if (!file) throw ValidationError("could not open TPXO cache member " + member);
  std::vector<std::byte> bytes(static_cast<std::size_t>(stat.size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const zip_int64_t count =
        zip_fread(file, bytes.data() + offset, bytes.size() - offset);
    if (count <= 0) {
      zip_fclose(file);
      throw ValidationError("could not read TPXO cache member " + member);
    }
    offset += static_cast<std::size_t>(count);
  }
  zip_fclose(file);
  return bytes;
}

NpyArray ParseNpy(const std::vector<std::byte>& bytes,
                  const std::string& member) {
  static constexpr std::array<std::uint8_t, 6> magic{0x93, 'N', 'U', 'M', 'P', 'Y'};
  if (bytes.size() < 10 ||
      !std::equal(magic.begin(), magic.end(), bytes.begin(),
                  [](std::uint8_t a, std::byte b) {
                    return a == std::to_integer<std::uint8_t>(b);
                  })) {
    throw ValidationError("invalid NPY header in " + member);
  }
  const auto major = std::to_integer<std::uint8_t>(bytes[6]);
  std::size_t prefix = 0;
  std::size_t header_size = 0;
  if (major == 1) {
    prefix = 10;
    header_size = U16(bytes.data() + 8);
  } else if (major == 2 || major == 3) {
    if (bytes.size() < 12) throw ValidationError("truncated NPY header in " + member);
    prefix = 12;
    header_size = U32(bytes.data() + 8);
  } else {
    throw ValidationError("unsupported NPY version in " + member);
  }
  if (prefix + header_size > bytes.size()) {
    throw ValidationError("truncated NPY metadata in " + member);
  }
  const std::string header(reinterpret_cast<const char*>(bytes.data() + prefix),
                           header_size);
  std::smatch match;
  if (!std::regex_search(header, match,
                         std::regex("'descr'\\s*:\\s*'([^']+)'"))) {
    throw ValidationError("NPY descriptor missing in " + member);
  }
  NpyArray result;
  result.descriptor = match[1].str();
  if (header.find("'fortran_order': True") != std::string::npos) {
    throw ValidationError("Fortran-order NPY arrays are unsupported in " + member);
  }
  if (!std::regex_search(header, match,
                         std::regex("'shape'\\s*:\\s*\\(([^)]*)\\)"))) {
    throw ValidationError("NPY shape missing in " + member);
  }
  std::stringstream dimensions(match[1].str());
  std::string token;
  while (std::getline(dimensions, token, ',')) {
    token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());
    if (!token.empty()) result.shape.push_back(std::stoull(token));
  }
  result.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(prefix + header_size),
                     bytes.end());
  return result;
}

NpyArray ReadNpy(zip_t* archive, const std::string& name) {
  return ParseNpy(ReadZipMember(archive, name + ".npy"), name + ".npy");
}

std::vector<std::byte> NpyBytes(const std::string& descriptor,
                                const std::vector<std::size_t>& shape,
                                const void* data, std::size_t byte_count) {
  std::ostringstream shape_text;
  shape_text << '(';
  for (std::size_t i=0;i<shape.size();++i) {
    if (i) shape_text << ", ";
    shape_text << shape[i];
  }
  if (shape.size()==1) shape_text << ',';
  shape_text << ')';
  std::string header="{'descr': '"+descriptor+"', 'fortran_order': False, 'shape': "+shape_text.str()+", }";
  const std::size_t prefix=10;
  const std::size_t padding=(16-((prefix+header.size()+1)%16))%16;
  header.append(padding,' '); header.push_back('\n');
  if (header.size()>65535) throw ValidationError("NPY header is too large");
  std::vector<std::byte> result(prefix+header.size()+byte_count);
  const unsigned char magic[]={0x93,'N','U','M','P','Y',1,0};
  std::memcpy(result.data(),magic,sizeof(magic));
  const auto length=static_cast<std::uint16_t>(header.size());
  result[8]=static_cast<std::byte>(length&0xff); result[9]=static_cast<std::byte>((length>>8)&0xff);
  std::memcpy(result.data()+prefix,header.data(),header.size());
  if (byte_count) std::memcpy(result.data()+prefix+header.size(),data,byte_count);
  return result;
}

std::vector<std::byte> UnicodeNpy(const std::vector<std::string>& values,
                                  const std::vector<std::size_t>& shape) {
  std::size_t width=1;
  for (const auto& value:values) {
    if (!std::all_of(value.begin(),value.end(),[](unsigned char c){return c<128;}))
      throw ValidationError("TPXO cache metadata must be ASCII/JSON escaped");
    width=std::max(width,value.size());
  }
  std::vector<std::uint32_t> utf32(values.size()*width);
  for(std::size_t i=0;i<values.size();++i) for(std::size_t j=0;j<values[i].size();++j)
    utf32[i*width+j]=static_cast<unsigned char>(values[i][j]);
  return NpyBytes("<U"+std::to_string(width),shape,utf32.data(),utf32.size()*sizeof(std::uint32_t));
}

void AddZipMember(zip_t* archive,const std::string& name,
                  std::vector<std::byte> bytes) {
  // libzip owns and frees this allocation after zip_source_buffer(..., 1)
  // succeeds; zip_file_add then transfers source ownership to the archive.
  void* owned=std::malloc(bytes.size());
  if (!owned) throw std::bad_alloc();
  std::memcpy(owned,bytes.data(),bytes.size());
  zip_source_t* source=zip_source_buffer(archive,owned,bytes.size(),1);
  if(!source || zip_file_add(archive,name.c_str(),source,ZIP_FL_OVERWRITE)<0) {
    if(source) zip_source_free(source); else std::free(owned);
    throw ValidationError("could not write TPXO cache member "+name);
  }
  const auto index=zip_name_locate(archive,name.c_str(),0);
  if(index<0 || zip_set_file_compression(archive,index,ZIP_CM_DEFLATE,6)<0)
    throw ValidationError("could not configure TPXO cache compression for "+name);
}

std::size_t Product(const std::vector<std::size_t>& shape) {
  std::size_t result = 1;
  for (const auto value : shape) {
    if (value != 0 && result > std::numeric_limits<std::size_t>::max() / value) {
      throw ValidationError("TPXO cache array shape overflows");
    }
    result *= value;
  }
  return result;
}

std::vector<double> ReadF64(const NpyArray& array, const std::string& name) {
  if (array.descriptor != "<f8" && array.descriptor != "=f8") {
    throw ValidationError(name + " must be a little-endian float64 NPY array");
  }
  const std::size_t count = Product(array.shape);
  if (array.data.size() != count * sizeof(double)) {
    throw ValidationError("invalid byte count for " + name);
  }
  std::vector<double> values(count);
  std::memcpy(values.data(), array.data.data(), array.data.size());
  return values;
}

std::vector<std::complex<double>> ReadC128(const NpyArray& array,
                                           const std::string& name) {
  if (array.descriptor != "<c16" && array.descriptor != "=c16") {
    throw ValidationError(name + " must be a little-endian complex128 NPY array");
  }
  const std::size_t count = Product(array.shape);
  if (array.data.size() != count * sizeof(std::complex<double>)) {
    throw ValidationError("invalid byte count for " + name);
  }
  std::vector<std::complex<double>> values(count);
  std::memcpy(values.data(), array.data.data(), array.data.size());
  return values;
}

std::string Utf8(std::uint32_t codepoint) {
  std::string value;
  if (codepoint <= 0x7f) value.push_back(static_cast<char>(codepoint));
  else if (codepoint <= 0x7ff) {
    value.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    value.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    value.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    value.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    value.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    value.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
  return value;
}

std::vector<std::string> ReadUnicode(const NpyArray& array,
                                     const std::string& name) {
  std::smatch match;
  if (!std::regex_match(array.descriptor, match, std::regex("[<|=]U([0-9]+)"))) {
    throw ValidationError(name + " must be a Unicode NPY array");
  }
  const std::size_t width = std::stoull(match[1].str());
  const std::size_t count = Product(array.shape);
  if (array.data.size() != count * width * 4) {
    throw ValidationError("invalid Unicode byte count for " + name);
  }
  std::vector<std::string> values;
  values.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    std::string value;
    for (std::size_t j = 0; j < width; ++j) {
      const auto codepoint = U32(array.data.data() + (i * width + j) * 4);
      if (codepoint != 0) value += Utf8(codepoint);
    }
    values.push_back(std::move(value));
  }
  return values;
}

double DaysSinceTideEpoch(TimePoint time) {
  using namespace std::chrono;
  const sys_days epoch = year{1992} / January / 1;
  return duration<double, std::ratio<86400>>(time - epoch).count();
}

struct ConstituentParameters { double phase{}; double omega{}; };

const std::map<std::string, ConstituentParameters>& Parameters() {
  static const std::map<std::string, ConstituentParameters> values{
      {"m2", {1.731557546, 1.405189e-4}}, {"s2", {0.0, 1.454441e-4}},
      {"k1", {0.173003674, 7.292117e-5}}, {"o1", {1.558553872, 6.759774e-5}},
      {"n2", {6.050721243, 1.378797e-4}}, {"p1", {6.110181633, 7.252295e-5}},
      {"k2", {3.487600001, 1.458423e-4}}, {"q1", {5.877717569, 6.495854e-5}},
      {"2n2", {4.086699633, 1.352405e-4}}, {"mu2", {3.463115091, 1.355937e-4}},
      {"nu2", {5.427136701, 1.382329e-4}}, {"l2", {0.553986502, 1.431581e-4}},
      {"t2", {0.050398470, 1.452450e-4}}, {"j1", {2.137025284, 7.556036e-5}},
      {"m1", {2.436575000, 7.025945e-5}}, {"oo1", {1.92904613, 7.824458e-5}},
      {"rho1", {5.254133027, 6.531174e-5}}, {"mf", {1.756042456, 0.053234e-4}},
      {"mm", {1.964021610, 0.026392e-4}}, {"m4", {3.463115091, 2.810377e-4}},
      {"ms4", {1.731557546, 2.859630e-4}}, {"mn4", {1.499093481, 2.783984e-4}},
      {"s1", {0.0, 0.0}}};
  return values;
}

std::pair<double, double> Nodal(const std::string& c, double n_deg,
                                double p_deg) {
  const double n = n_deg * kPi / 180.0;
  const double p = p_deg * kPi / 180.0;
  const double sn = std::sin(n), cn = std::cos(n);
  const double s2n = std::sin(2*n), c2n = std::cos(2*n);
  const double s3n = std::sin(3*n);
  double a = 0.0, b = 1.0;
  if (c == "p1" || c == "s1" || c == "s2") return {1.0, 0.0};
  if (c == "mm") { b = 1.0 - 0.130 * cn; }
  else if (c == "mf") {
    return {1.043 + 0.414 * cn,
            (-23.7 * sn + 2.7 * s2n - 0.4 * s3n) * kPi / 180.0};
  } else if (c == "o1") {
    a = 0.189 * sn - 0.0058 * s2n;
    b = 1.0 + 0.189 * cn - 0.0058 * c2n;
    return {std::hypot(a,b), (10.8*sn - 1.3*s2n + 0.2*s3n)*kPi/180.0};
  } else if (c == "q1" || c == "2q1" || c == "rho1") {
    return {std::hypot(1.0 + 0.188*cn, 0.188*sn),
            std::atan(0.189*sn/(1.0 + 0.189*cn))};
  } else if (c == "k1") {
    a = -0.1554*sn + 0.0029*s2n;
    b = 1.0 + 0.1158*cn - 0.0029*c2n;
  } else if (c == "k2") {
    a = -0.3108*sn - 0.0324*s2n;
    b = 1.0 + 0.2852*cn + 0.0324*c2n;
  } else if (c == "m2" || c == "n2" || c == "2n2" || c == "ms4") {
    a = -0.03731*sn + 0.00052*s2n;
    b = 1.0 - 0.03731*cn + 0.00052*c2n;
  } else if (c == "m4" || c == "mn4") {
    const auto [f,u] = Nodal("m2", n_deg, p_deg);
    return {f*f, 2*u};
  } else if (c == "l2") {
    a = -0.25*std::sin(2*p) - 0.11*std::sin(2*p-n) - 0.04*sn;
    b = 1.0 - 0.25*std::cos(2*p) - 0.11*std::cos(2*p-n) - 0.04*cn;
  }
  return {std::hypot(a,b), std::atan2(a,b)};
}

struct Astronomy { double s,h,p,n,pp,tau; };
Astronomy AtMjd(double mjd) {
  const double t = mjd - 51544.4993;
  auto mod = [](double value) { value = std::fmod(value, 360.0); return value < 0 ? value + 360 : value; };
  Astronomy a;
  a.s = mod(218.3164 + 13.17639648*t);
  a.h = mod(280.4661 + 0.98564736*t);
  a.p = mod(83.3535 + 0.11140353*t);
  a.n = mod(125.0445 - 0.05295377*t);
  a.pp = 282.8;
  const double hour = 24.0 * (mjd - std::floor(mjd));
  a.tau = 15.0*hour - a.s + a.h;
  return a;
}

using Harmonics = std::map<std::string, std::complex<double>>;

double MajorPrediction(const Harmonics& values, double tide_days,
                       const Astronomy& astronomy) {
  double result = 0.0;
  for (const auto& [name,z] : values) {
    const auto found = Parameters().find(name);
    if (found == Parameters().end()) {
      throw ValidationError("unsupported TPXO cache constituent: " + name);
    }
    const auto [f,u] = Nodal(name, astronomy.n, astronomy.p);
    const double theta = 86400.0*found->second.omega*tide_days +
                         found->second.phase + u;
    result += z.real()*f*std::cos(theta) - z.imag()*f*std::sin(theta);
  }
  return result;
}

struct MinorDefinition {
  std::string name;
  std::array<double,7> coefficients;
};

const std::vector<MinorDefinition>& MinorDefinitions() {
  static const std::vector<MinorDefinition> values{
    {"2q1",{1,-3,0,2,0,0,-1}}, {"sigma1",{1,-3,2,0,0,0,-1}},
    {"rho1",{1,-2,2,-1,0,0,-1}}, {"m1b",{1,0,0,-1,0,0,1}},
    // pyTMD labels this inferred amplitude m1, but uses the positional m1a
    // astronomical argument returned by minor_arguments().
    {"m1",{1,0,0,1,0,0,1}}, {"chi1",{1,0,2,-1,0,0,1}},
    {"pi1",{1,1,-3,0,0,1,-1}}, {"phi1",{1,1,2,0,0,0,1}},
    {"theta1",{1,2,-2,1,0,0,1}}, {"j1",{1,2,0,-1,0,0,1}},
    {"oo1",{1,3,0,0,0,0,1}}, {"2n2",{2,-2,0,2,0,0,0}},
    {"mu2",{2,-2,2,0,0,0,0}}, {"nu2",{2,-1,2,-1,0,0,0}},
    {"lambda2",{2,1,-2,1,0,0,2}}, {"l2",{2,1,0,-1,0,0,2}},
    {"l2b",{2,1,0,1,0,0,0}}, {"t2",{2,2,-3,0,0,1,0}}
  };
  return values;
}

std::optional<std::complex<double>> Get(const Harmonics& h,
                                        const std::string& key) {
  const auto it = h.find(key); return it == h.end() ? std::nullopt : std::optional(it->second);
}

std::complex<double> Required(const Harmonics& h, const std::string& key) {
  const auto value = Get(h,key);
  if (!value) throw ValidationError("TPXO minor inference requires " + key);
  return *value;
}

double MinorPrediction(const Harmonics& h, const Astronomy& a) {
  static const std::array<std::string,9> needed{"q1","o1","p1","k1","n2","m2","s2","k2","2n2"};
  int available = 0; for (const auto& c : needed) available += h.contains(c);
  if (available < 6) return 0.0;
  std::map<std::string,std::complex<double>> z;
  z["2q1"] = .263*Required(h,"q1") - .0252*Required(h,"o1");
  z["sigma1"] = .297*Required(h,"q1") - .0264*Required(h,"o1");
  z["rho1"] = .164*Required(h,"q1") + .0048*Required(h,"o1");
  z["m1b"] = .0140*Required(h,"o1") + .0101*Required(h,"k1");
  z["m1"] = .0389*Required(h,"o1") + .0282*Required(h,"k1");
  z["chi1"] = .0064*Required(h,"o1") + .0060*Required(h,"k1");
  z["pi1"] = .0030*Required(h,"o1") + .0171*Required(h,"k1");
  z["phi1"] = -.0015*Required(h,"o1") + .0152*Required(h,"k1");
  z["theta1"] = -.0065*Required(h,"o1") + .0155*Required(h,"k1");
  z["j1"] = -.0389*Required(h,"o1") + .0836*Required(h,"k1");
  z["oo1"] = -.0431*Required(h,"o1") + .0613*Required(h,"k1");
  z["2n2"] = .264*Required(h,"n2") - .0253*Required(h,"m2");
  z["mu2"] = .298*Required(h,"n2") - .0264*Required(h,"m2");
  z["nu2"] = .165*Required(h,"n2") + .00487*Required(h,"m2");
  z["lambda2"] = .0040*Required(h,"m2") + .0074*Required(h,"s2");
  z["l2"] = .0131*Required(h,"m2") + .0326*Required(h,"s2");
  z["l2b"] = .0033*Required(h,"m2") + .0082*Required(h,"s2");
  z["t2"] = .0585*Required(h,"s2");

  const double n = a.n*kPi/180.0;
  const double sn=std::sin(n),cn=std::cos(n),s2n=std::sin(2*n),c2n=std::cos(2*n);
  std::array<double,18> f{}; f.fill(1.0);
  std::array<double,18> u{}; u.fill(0.0);
  f[0]=std::hypot(1+.189*cn-.0058*c2n,.189*sn-.0058*s2n); f[1]=f[2]=f[0];
  f[3]=std::hypot(1+.185*cn,.185*sn); f[4]=std::hypot(1+.201*cn,.201*sn);
  f[5]=std::hypot(1+.221*cn,.221*sn); f[9]=std::hypot(1+.198*cn,.198*sn);
  f[10]=std::hypot(1+.640*cn+.134*c2n,.640*sn+.134*s2n);
  f[11]=std::hypot(1-.0373*cn,.0373*sn); f[12]=f[13]=f[15]=f[11];
  f[16]=std::hypot(1+.441*cn,.441*sn);
  u[0]=std::atan2(.189*sn-.0058*s2n,1+.189*cn-.0058*s2n); u[1]=u[2]=u[0];
  u[3]=std::atan2(.185*sn,1+.185*cn); u[4]=std::atan2(-.201*sn,1+.201*cn);
  u[5]=std::atan2(-.221*sn,1+.221*cn); u[9]=std::atan2(-.198*sn,1+.198*cn);
  u[10]=std::atan2(-.640*sn-.134*s2n,1+.640*cn+.134*c2n);
  u[11]=std::atan2(-.0373*sn,1-.0373*cn); u[12]=u[13]=u[15]=u[11];
  u[16]=std::atan2(-.441*sn,1+.441*cn);

  std::set<std::string> major;
  for (const auto& [name, value] : h) { (void)value; major.insert(name); }
  double result=0.0;
  const std::array<double,7> args{a.tau,a.s,a.h,a.p,a.n,a.pp,90.0};
  for (std::size_t i=0;i<MinorDefinitions().size();++i) {
    const auto& d=MinorDefinitions()[i];
    const std::string amplitude_name = d.name == "m1b" ? "m1b" : d.name;
    const std::string exclusion_name = d.name == "m1b" ? "m1b" : d.name;
    if (major.contains(exclusion_name)) continue;
    double g=0.0; for (std::size_t j=0;j<7;++j) g += args[j]*d.coefficients[j];
    const auto& value=z.at(amplitude_name);
    const double theta=g*kPi/180.0+u[i];
    result += value.real()*f[i]*std::cos(theta)-value.imag()*f[i]*std::sin(theta);
  }
  return result;
}

std::vector<CurrentGrid> PredictComponentPair(
    const TpxoCache& cache, const std::vector<TimePoint>& times,
    bool infer_minor) {
  if (cache.metadata.get("corrections", "ATLAS").asString() != "ATLAS") {
    throw ValidationError("native TPXO cache prediction currently requires ATLAS corrections");
  }
  const std::size_t points=cache.grid.size(), nc=cache.constituents.size();
  std::vector<CurrentGrid> result;
  result.reserve(times.size());
  for (const auto time:times) {
    const double tide_days=DaysSinceTideEpoch(time);
    const auto astronomy=AtMjd(tide_days+kMjdTideEpoch);
    CurrentGrid grid{time,cache.grid,std::vector<double>(points),
                     std::vector<double>(points),std::vector<std::uint8_t>(points)};
    bool any_mask = false;
    for (std::size_t p=0;p<points;++p) {
      Harmonics uh,vh;
      bool masked=false;
      for (std::size_t c=0;c<nc;++c) {
        const auto u=cache.u_cm_s[c*points+p],v=cache.v_cm_s[c*points+p];
        if (!std::isfinite(u.real()) || !std::isfinite(u.imag()) ||
            !std::isfinite(v.real()) || !std::isfinite(v.imag())) { masked=true; break; }
        uh[cache.constituents[c]]=u; vh[cache.constituents[c]]=v;
      }
      if (masked) { grid.mask[p] = 1; any_mask = true; continue; }
      const double um=MajorPrediction(uh,tide_days,astronomy)+(infer_minor?MinorPrediction(uh,astronomy):0);
      const double vm=MajorPrediction(vh,tide_days,astronomy)+(infer_minor?MinorPrediction(vh,astronomy):0);
      grid.u_mps[p]=um/100.0; grid.v_mps[p]=vm/100.0;
    }
    if (!any_mask) grid.mask.clear();
    result.push_back(std::move(grid));
  }
  return result;
}

}  // namespace

TpxoCache LoadTpxoCache(const std::filesystem::path& path) {
  int error=0;
  zip_t* archive=zip_open(path.c_str(),ZIP_RDONLY,&error);
  if (!archive) throw ValidationError("could not read TPXO cache file " + path.string());
  try {
    const auto metadata_values=ReadUnicode(ReadNpy(archive,"metadata_json"),"metadata_json");
    if (metadata_values.size()!=1) throw ValidationError("TPXO cache metadata must be scalar");
    Json::CharReaderBuilder builder; Json::Value metadata; std::string errors;
    std::istringstream input(metadata_values[0]);
    if (!Json::parseFromStream(builder,input,&metadata,&errors)) throw ValidationError("invalid TPXO cache metadata JSON");
    if (metadata["format"].asString()!="tidal-current-grib-generator-tpxo-cache" || metadata["format_version"].asInt()!=1)
      throw ValidationError("unsupported TPXO cache format");
    const auto longitudes=ReadF64(ReadNpy(archive,"longitudes"),"longitudes");
    const auto latitudes=ReadF64(ReadNpy(archive,"latitudes"),"latitudes");
    const auto constituents=ReadUnicode(ReadNpy(archive,"constituents"),"constituents");
    const auto ua=ReadNpy(archive,"u_complex"),va=ReadNpy(archive,"v_complex");
    auto u=ReadC128(ua,"u_complex"),v=ReadC128(va,"v_complex");
    if (ua.shape!=va.shape || ua.shape.size()!=2 || ua.shape[0]!=constituents.size() || ua.shape[1]!=latitudes.size()*longitudes.size())
      throw ValidationError("TPXO cache harmonic array dimensions do not match its grid");
    const auto& b=metadata["bbox"];
    TpxoCache cache{metadata,{b["west"].asDouble(),b["south"].asDouble(),b["east"].asDouble(),b["north"].asDouble()},
                    {latitudes,longitudes,metadata["grid_spacing_deg"].asDouble(),
                     metadata["grid_spacing_deg"].asDouble(),
                     metadata["grid_spacing_deg"].asDouble()},
                    constituents,std::move(u),std::move(v)};
    cache.bbox.Validate();
    if (cache.grid.nx() == 0 || cache.grid.ny() == 0)
      throw ValidationError("TPXO cache grid is empty");
    zip_close(archive);
    return cache;
  } catch (...) { zip_close(archive); throw; }
}

void WriteTpxoCache(const std::filesystem::path& path,const TpxoCache& cache,
                    bool overwrite) {
  if(std::filesystem::exists(path)&&!overwrite) throw ValidationError("TPXO cache already exists; enable overwrite to replace it");
  if(cache.u_cm_s.size()!=cache.constituents.size()*cache.grid.size() || cache.v_cm_s.size()!=cache.u_cm_s.size())
    throw ValidationError("TPXO cache harmonic dimensions are inconsistent");
  std::filesystem::create_directories(path.parent_path().empty()?std::filesystem::current_path():path.parent_path());
  const auto temporary=path.string()+".tmp";
  std::error_code ignored; std::filesystem::remove(temporary,ignored);
  int error=0; zip_t* archive=zip_open(temporary.c_str(),ZIP_CREATE|ZIP_TRUNCATE,&error);
  if(!archive) throw ValidationError("could not create TPXO cache "+temporary);
  try {
    Json::StreamWriterBuilder builder; builder["indentation"]="";
    const std::string metadata=Json::writeString(builder,cache.metadata);
    AddZipMember(archive,"metadata_json.npy",UnicodeNpy({metadata},{}));
    AddZipMember(archive,"longitudes.npy",NpyBytes("<f8",{cache.grid.longitudes.size()},cache.grid.longitudes.data(),cache.grid.longitudes.size()*sizeof(double)));
    AddZipMember(archive,"latitudes.npy",NpyBytes("<f8",{cache.grid.latitudes.size()},cache.grid.latitudes.data(),cache.grid.latitudes.size()*sizeof(double)));
    std::vector<double> lon_points,lat_points; lon_points.reserve(cache.grid.size()); lat_points.reserve(cache.grid.size());
    for(double lat:cache.grid.latitudes) for(double lon:cache.grid.longitudes) {lon_points.push_back(lon);lat_points.push_back(lat);}
    AddZipMember(archive,"lon_points.npy",NpyBytes("<f8",{lon_points.size()},lon_points.data(),lon_points.size()*sizeof(double)));
    AddZipMember(archive,"lat_points.npy",NpyBytes("<f8",{lat_points.size()},lat_points.data(),lat_points.size()*sizeof(double)));
    AddZipMember(archive,"constituents.npy",UnicodeNpy(cache.constituents,{cache.constituents.size()}));
    const std::vector<std::size_t> harmonic_shape{cache.constituents.size(),cache.grid.size()};
    AddZipMember(archive,"u_complex.npy",NpyBytes("<c16",harmonic_shape,cache.u_cm_s.data(),cache.u_cm_s.size()*sizeof(std::complex<double>)));
    AddZipMember(archive,"v_complex.npy",NpyBytes("<c16",harmonic_shape,cache.v_cm_s.data(),cache.v_cm_s.size()*sizeof(std::complex<double>)));
    if(zip_close(archive)!=0) { archive=nullptr; throw ValidationError("could not finalize TPXO cache "+temporary); }
    archive=nullptr; std::filesystem::rename(temporary,path);
  } catch (...) {
    if (archive) zip_discard(archive);
    std::filesystem::remove(temporary,ignored);
    throw;
  }
}

Json::Value InspectTpxoCache(const std::filesystem::path& path) {
  const auto cache=LoadTpxoCache(path);
  Json::Value value=cache.metadata; value["input_cache"]=path.string();
  value["grid_size"]["nx"]=Json::UInt64(cache.grid.nx());
  value["grid_size"]["ny"]=Json::UInt64(cache.grid.ny());
  value["point_count"]=Json::UInt64(cache.grid.size()); value["valid"]=true;
  return value;
}

std::vector<CurrentGrid> PredictTpxoCache(const TpxoCache& cache,
                                          const std::vector<TimePoint>& times,
                                          bool infer_minor) {
  return PredictComponentPair(cache,times,infer_minor);
}

TpxoGenerationResult GenerateFromTpxoCache(
    const std::filesystem::path& input_cache, TimePoint start, int hours,
    int step_hours, const std::filesystem::path& output, bool infer_minor,
    bool overwrite) {
  if (std::filesystem::exists(output) && !overwrite) throw ValidationError("output already exists; enable overwrite to replace it");
  const auto cache=LoadTpxoCache(input_cache);
  const auto fields=PredictTpxoCache(cache,BuildTimeSequence(start,hours,step_hours),infer_minor);
  WriteGrib1Currents(fields,output);
  const auto messages=ScanGribMessages(output);
  return {output,messages.message_count,std::filesystem::file_size(output),InspectGrib(output)};
}

}  // namespace environmental_grib
