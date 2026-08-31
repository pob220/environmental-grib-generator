#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "environmental_grib/environment.h"
#include "environmental_grib/geo.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/model.h"
#include "environmental_grib/parallel.h"
#include "environmental_grib/platform.h"
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
    bool map_failure_caught = false;
    try {
      (void)eg::ParallelMapOrdered(inputs, 4, [](const int& input) {
        if (input == 3) throw std::runtime_error("expected worker failure");
        return input;
      });
    } catch (const std::runtime_error& error) {
      map_failure_caught =
          std::string(error.what()) == "expected worker failure";
    }
    Check(map_failure_caught,
          "ordered map joins workers and propagates worker failure");

    const auto token = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root =
        std::filesystem::temp_directory_path() /
        ("environmental-grib-concurrency-tests-" +
         std::to_string(eg::ProcessId()) + "-" + token);
    std::error_code create_error;
    std::filesystem::create_directories(root, create_error);
    if (create_error)
      throw std::runtime_error("cannot create isolated concurrency test "
                               "directory: " +
                               create_error.message());
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
                   std::atomic<int>& peak,
                   std::atomic<bool>& component_overlap) {
      std::atomic<int> active{0};
      std::mutex component_mutex;
      std::condition_variable component_cv;
      bool weather_entered = false;
      bool current_entered = false;
      request.parallel_components = parallel;
      request.output = output;
      return eg::GenerateEnvironment(
          request,
          [&](const std::string& url, double) {
            const bool is_current = url.rfind("ftp://", 0) == 0;
            if (parallel) {
              std::unique_lock<std::mutex> lock(component_mutex);
              (is_current ? current_entered : weather_entered) = true;
              if (weather_entered && current_entered)
                component_overlap.store(true);
              component_cv.notify_all();
              if (!component_cv.wait_for(
                      lock, std::chrono::seconds(5), [&] {
                        return weather_entered && current_entered;
                      })) {
                throw std::runtime_error(
                    "weather and current components did not overlap");
              }
            }
            const int count = ++active;
            UpdatePeak(peak, count);
            std::this_thread::sleep_for(is_current
                                            ? std::chrono::milliseconds(120)
                                            : std::chrono::milliseconds(50));
            --active;
            return is_current ? current_bytes : weather_bytes;
          },
          start);
    };

    std::atomic<int> serial_peak{0}, parallel_peak{0};
    std::atomic<bool> serial_overlap{false}, parallel_overlap{false};
    const auto serial =
        run(false, serial_path, serial_peak, serial_overlap);
    const auto concurrent =
        run(true, parallel_path, parallel_peak, parallel_overlap);
    Check(serial_peak.load() == 4,
          "serial component mode still parallelizes forecast hours");
    Check(!serial_overlap.load(), "serial component mode does not overlap");
    Check(parallel_overlap.load(),
          "weather and remote current components overlap");
    Check(parallel_peak.load() <= 5,
          "overlapping component downloads stay within global bound");
    Check(serial.message_count == concurrent.message_count &&
              serial.byte_count == concurrent.byte_count,
          "component concurrency preserves output counts");
    Check(ReadBytes(serial_path) == ReadBytes(parallel_path),
          "component concurrency preserves GRIB bytes exactly");

    const auto workspace_prefix =
        ".environmental-grib-" + std::to_string(eg::ProcessId());
    const auto count_workspaces = [&] {
      std::size_t count = 0;
      std::error_code iterator_error;
      std::filesystem::directory_iterator entry(root, iterator_error), end;
      while (!iterator_error && entry != end) {
        std::error_code status_error;
        if (entry->is_directory(status_error) && !status_error &&
            entry->path().filename().string().rfind(workspace_prefix, 0) ==
                0)
          ++count;
        entry.increment(iterator_error);
      }
      Check(!iterator_error, "isolated workspace directory is readable");
      return count;
    };
    const auto workspaces_before = count_workspaces();
    eg::EnvironmentRequest retained_request;
    retained_request.bbox = request.bbox;
    retained_request.start = start;
    retained_request.hours = 0;
    retained_request.step_hours = 1;
    retained_request.weather_provider = "existing-file";
    retained_request.weather_file = weather_path;
    retained_request.current_source = "none";
    retained_request.overwrite = true;
    retained_request.keep_intermediate = true;
    retained_request.parallel_components = false;
    retained_request.output =
        root / ("xgrib-retained-workspace-a-" + token + ".grb");
    auto retained_a = std::async(std::launch::async, [&] {
      return eg::GenerateEnvironment(retained_request, {}, start);
    });
    auto retained_request_b = retained_request;
    retained_request_b.output =
        root / ("xgrib-retained-workspace-b-" + token + ".grb");
    auto retained_b = std::async(std::launch::async, [&] {
      return eg::GenerateEnvironment(retained_request_b, {}, start);
    });
    (void)retained_a.get();
    (void)retained_b.get();
    Check(count_workspaces() == workspaces_before + 2,
          "concurrent generator jobs retain distinct workspaces");

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    Check(!cleanup_error, "isolated concurrency test directory is removed");
  } catch (const std::exception& error) {
    std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
    ++failures;
  }
  std::cout << "environmental_grib_concurrency_tests failures=" << failures
            << '\n';
  return failures == 0 ? 0 : 1;
}
