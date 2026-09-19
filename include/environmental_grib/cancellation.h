#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <curl/curl.h>

namespace environmental_grib {
// Optional per-job context. Desktop callers retain the existing defaults.
struct ExecutionContext {
  std::shared_ptr<std::atomic<bool>> cancelled;
  std::string ca_bundle;
};
inline thread_local ExecutionContext current_execution;
class ExecutionScope {
 public:
  explicit ExecutionScope(ExecutionContext context)
      : previous_(current_execution) { current_execution = std::move(context); }
  ~ExecutionScope() { current_execution = previous_; }
 private:
  ExecutionContext previous_;
};
inline void CheckCancellation() {
  if (current_execution.cancelled && current_execution.cancelled->load())
    throw std::runtime_error("Generation cancelled");
}
inline void ConfigureJobCurl(CURL* curl) {
  CheckCancellation();
  if (!current_execution.ca_bundle.empty())
    curl_easy_setopt(curl, CURLOPT_CAINFO, current_execution.ca_bundle.c_str());
  if (current_execution.cancelled) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,
                     current_execution.cancelled.get());
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
        +[](void* flag, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
          return static_cast<std::atomic<bool>*>(flag)->load() ? 1 : 0;
        });
  }
}
}  // namespace environmental_grib
