#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "environmental_grib/environment.h"
#include "environmental_grib/geo.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/model.h"
#include "environmental_grib/parallel.h"
#include "environmental_grib/sources.h"

namespace eg = environmental_grib;

namespace {

int failures = 0;

void Check(bool condition, const std::string& description) {
  if (!condition) {
    std::cerr << "FAIL: " << description << '\n';
    ++failures;
  }
}

std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("cannot open concurrency fixture");
  const auto size = input.tellg();
  std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!input) throw std::runtime_error("cannot read concurrency fixture");
  return bytes;
}

void UpdatePeak(std::atomic<int>& peak, int active) {
  int value = peak.load();
  while (active > value && !peak.compare_exchange_weak(value, active)) {
  }
}

}  // namespace

int main() {
  try {
    std::vector<int> inputs(16);
    for (std::size_t i = 0; i < inputs.size(); ++i)
      inputs[i] = static_cast<int>(i);
    std::atomic<int> map_active{0}, map_peak{0};
    const auto mapped =
        eg::ParallelMapOrdered(inputs, 4, [&](const int& input) {
          const int active = ++map_active;
          UpdatePeak(map_peak, active);
          std::this_thread::sleep_for(
              std::chrono::milliseconds(2 * (4 - input % 4)));
          --map_active;
          return input * input;
        });
    Check(map_peak.load() == 4, "ordered map uses the requested bound");
    for (std::size_t i = 0; i < mapped.size(); ++i)
      Check(mapped[i] == inputs[i] * inputs[i],
            "ordered map preserves input order");

    const auto root = std::filesystem::temp_directory_path();
    const auto token = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto weather_path =
        root / ("xgrib-concurrency-weather-" + token + ".grb2");
    const auto current_path =
        root / ("xgrib-concurrency-current-" + token + ".grb");
    const auto serial_path =
        root / ("xgrib-concurrency-serial-" + token + ".grb");
    const auto parallel_path =
        root / ("xgrib-concurrency-parallel-" + token + ".grb");
    const auto start = eg::ParseUtcDateTime("2026-07-01T00:00:00Z");
    const auto grid = eg::BuildRegularGrid({-8.5, 50.5, -7.5, 51.5}, 0.5);
    eg::WriteRegularLatLonGrib2(
        grid, start,
        {{0, "10u", std::vector<double>(grid.size(), 4.0), {}},
         {0, "10v", std::vector<double>(grid.size(), -2.0), {}}},
        weather_path);
    const auto current =
        eg::MakeSyntheticRotaryCurrent({-8.5, 50.5, -7.5, 51.5}, start, grid);
    eg::WriteGrib1Currents({current}, current_path);
    const auto weather_bytes = ReadBytes(weather_path);
    const auto current_bytes = ReadBytes(current_path);

    eg::EnvironmentRequest request;
    request.bbox = {-8.5, 50.5, -7.5, 51.5};
    request.start = start;
    request.hours = 6;
    request.step_hours = 1;
    request.cycle = "00";
    request.date = "20260701";
    request.weather_provider = "gfs";
    request.current_source = "marine_ie_irish_sea";
    request.overwrite = true;

    auto run = [&](bool parallel, const std::filesystem::path& output,
                   std::atomic<int>& peak) {
      std::atomic<int> active{0};
      request.parallel_components = parallel;
      request.output = output;
      return eg::GenerateEnvironment(
          request,
          [&](const std::string& url, double) {
            const int count = ++active;
            UpdatePeak(peak, count);
            std::this_thread::sleep_for(url.rfind("ftp://", 0) == 0
                                            ? std::chrono::milliseconds(120)
                                            : std::chrono::milliseconds(50));
            --active;
            return url.rfind("ftp://", 0) == 0 ? current_bytes : weather_bytes;
          },
          start);
    };

    std::atomic<int> serial_peak{0}, parallel_peak{0};
    const auto serial = run(false, serial_path, serial_peak);
    const auto concurrent = run(true, parallel_path, parallel_peak);
    Check(serial_peak.load() == 4,
          "serial component mode still parallelizes forecast hours");
    Check(parallel_peak.load() == 5,
          "weather and remote current downloads overlap within global bound");
    Check(serial.message_count == concurrent.message_count &&
              serial.byte_count == concurrent.byte_count,
          "component concurrency preserves output counts");
    Check(ReadBytes(serial_path) == ReadBytes(parallel_path),
          "component concurrency preserves GRIB bytes exactly");

    for (const auto& path :
         {weather_path, current_path, serial_path, parallel_path}) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } catch (const std::exception& error) {
    std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
    ++failures;
  }
  std::cout << "environmental_grib_concurrency_tests failures=" << failures
            << '\n';
  return failures == 0 ? 0 : 1;
}
