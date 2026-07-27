#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace environmental_grib {

inline constexpr std::size_t kDefaultDownloadConcurrency = 4;

inline std::mutex& NetcdfApiMutex() {
  static std::mutex mutex;
  return mutex;
}

// Run independent work concurrently while preserving the input order in the
// returned vector.  All workers are joined before an exception is rethrown, so
// callers can safely clean temporary files after a failed batch.
template <typename Input, typename Function>
auto ParallelMapOrdered(const std::vector<Input>& inputs,
                        std::size_t maximum_concurrency, Function function)
    -> std::vector<std::invoke_result_t<Function, const Input&>> {
  using Result = std::invoke_result_t<Function, const Input&>;
  static_assert(!std::is_void_v<Result>);
  if (inputs.empty()) return {};
  const std::size_t worker_count =
      std::max<std::size_t>(1, std::min(maximum_concurrency, inputs.size()));
  std::vector<std::optional<Result>> slots(inputs.size());
  std::atomic<std::size_t> next{0};
  std::atomic<bool> failed{false};
  std::exception_ptr failure;
  std::mutex failure_mutex;
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  try {
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
      workers.emplace_back([&] {
        while (!failed.load(std::memory_order_acquire)) {
          const std::size_t index = next.fetch_add(1);
          if (index >= inputs.size()) return;
          try {
            slots[index].emplace(function(inputs[index]));
          } catch (...) {
            {
              std::lock_guard lock(failure_mutex);
              if (!failure) failure = std::current_exception();
            }
            failed.store(true, std::memory_order_release);
            return;
          }
        }
      });
    }
  } catch (...) {
    failed.store(true, std::memory_order_release);
    for (auto& worker : workers)
      if (worker.joinable()) worker.join();
    throw;
  }
  for (auto& worker : workers)
    if (worker.joinable()) worker.join();
  if (failure) std::rethrow_exception(failure);
  std::vector<Result> results;
  results.reserve(slots.size());
  for (auto& slot : slots) results.push_back(std::move(*slot));
  return results;
}

}  // namespace environmental_grib
