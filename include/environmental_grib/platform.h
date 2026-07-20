#pragma once

#include <cstdint>
#include <ctime>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace environmental_grib {

inline std::uint64_t ProcessId() {
#ifdef _WIN32
  return static_cast<std::uint64_t>(::_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

inline std::tm UtcTime(std::time_t value) {
  std::tm result{};
#ifdef _WIN32
  ::gmtime_s(&result, &value);
#else
  ::gmtime_r(&value, &result);
#endif
  return result;
}

inline std::time_t UtcTimeToEpoch(std::tm* value) {
#ifdef _WIN32
  return ::_mkgmtime(value);
#else
  return ::timegm(value);
#endif
}

}  // namespace environmental_grib
