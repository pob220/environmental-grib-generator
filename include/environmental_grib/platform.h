#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace environmental_grib {

inline std::string PathToUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
  const auto value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
#else
  return path.string();
#endif
}

inline std::filesystem::path PathFromUtf8(std::string_view value) {
#ifdef _WIN32
  const auto* data = reinterpret_cast<const char8_t*>(value.data());
  return std::filesystem::path(std::u8string(data, data + value.size()));
#else
  return std::filesystem::path(value);
#endif
}

inline void SetEnvironmentIfAbsent(const char* name,
                                   const std::filesystem::path& value) {
  if (std::getenv(name)) return;
#ifdef _WIN32
  ::_putenv_s(name, PathToUtf8(value).c_str());
#else
  ::setenv(name, value.c_str(), 0);
#endif
}

inline FILE* OpenFileForReading(const std::filesystem::path& path) {
#ifdef _WIN32
  return ::_wfopen(path.c_str(), L"rb");
#else
  return std::fopen(path.c_str(), "rb");
#endif
}

constexpr std::uint16_t ByteSwap16(std::uint16_t value) {
  return static_cast<std::uint16_t>((value >> 8U) | (value << 8U));
}

constexpr std::uint32_t ByteSwap32(std::uint32_t value) {
  return ((value & 0x000000ffU) << 24U) |
         ((value & 0x0000ff00U) << 8U) |
         ((value & 0x00ff0000U) >> 8U) |
         ((value & 0xff000000U) >> 24U);
}

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
