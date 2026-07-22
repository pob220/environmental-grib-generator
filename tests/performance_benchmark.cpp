#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>

#include "environmental_grib/environment.h"
#include "environmental_grib/geo.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/model.h"

namespace eg = environmental_grib;

namespace {

std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("cannot open benchmark fixture");
  const auto size = input.tellg();
  std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!input) throw std::runtime_error("cannot read benchmark fixture");
  return bytes;
}

std::vector<unsigned char> MakeFixture(const std::filesystem::path& path,
                                       eg::TimePoint start, bool waves) {
  const auto grid = eg::BuildRegularGrid({-8.5, 50.5, -8.0, 51.0}, 0.5);
  const std::vector<double> values(grid.size(), waves ? 2.0 : 10.0);
  std::vector<eg::Grib2Field> fields;
  if (waves) {
    fields.push_back({0, "swh", values, {}});
    fields.push_back({0, "perpw", values, {}});
    fields.push_back({0, "dirpw", values, {}});
  } else {
    fields.push_back({0, "10u", values, {}});
    fields.push_back({0, "10v", values, {}});
  }
  eg::WriteRegularLatLonGrib2(grid, start, fields, path);
  return ReadBytes(path);
}

class ScopeFiles {
public:
  ~ScopeFiles() {
    std::error_code ignored;
    for (const auto& path : paths) std::filesystem::remove(path, ignored);
  }
  std::vector<std::filesystem::path> paths;
};

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      std::cerr << "usage: environmental_grib_performance_benchmark OUTPUT\n";
      return 2;
    }
    const auto output = std::filesystem::path(argv[1]);
    const auto temporary = std::filesystem::temp_directory_path();
    const auto token = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto weather_path =
        temporary / ("xgrib-bench-weather-" + token + ".grb2");
    const auto wave_path = temporary / ("xgrib-bench-wave-" + token + ".grb2");
    ScopeFiles cleanup{{weather_path, wave_path}};
    const auto start = eg::ParseUtcDateTime("2026-07-01T00:00:00Z");
    const auto weather = MakeFixture(weather_path, start, false);
    const auto waves = MakeFixture(wave_path, start, true);

    eg::EnvironmentRequest request;
    request.bbox = {-8.5, 50.5, -7.5, 51.5};
    request.start = start;
    request.hours = 12;
    request.step_hours = 1;
    request.cycle = "00";
    request.date = "20260701";
    request.weather_provider = "gfs";
    request.include_waves = true;
    request.wave_provider = "gfs_wave";
    request.wave_step_hours = 3;
    request.current_source = "synthetic";
    request.current_grid_spacing_deg = 0.5;
    request.output = output;
    request.overwrite = true;

    std::atomic<int> requests{0};
    std::atomic<int> in_flight{0};
    std::atomic<int> peak_in_flight{0};
    auto download = [&](const std::string& url, double) {
      ++requests;
      const int active = ++in_flight;
      int peak = peak_in_flight.load();
      while (active > peak &&
             !peak_in_flight.compare_exchange_weak(peak, active)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      --in_flight;
      return url.find("wave") == std::string::npos ? weather : waves;
    };

    const auto began = std::chrono::steady_clock::now();
    const auto result = eg::GenerateEnvironment(request, download, start);
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - began);

    Json::Value audit(Json::objectValue);
    audit["elapsed_ms"] = elapsed.count();
    audit["request_count"] = requests.load();
    audit["peak_in_flight"] = peak_in_flight.load();
    audit["message_count"] = Json::UInt64(result.message_count);
    audit["byte_count"] = Json::UInt64(result.byte_count);
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::cout << Json::writeString(builder, audit) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
