#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <json/json.h>

namespace environmental_grib {

using BinaryDownload =
    std::function<std::vector<unsigned char>(const std::string&, double)>;

struct DirectCurrentResult {
  std::string provider;
  std::filesystem::path output;
  std::size_t raw_byte_count{};
  std::size_t clean_byte_count{};
  std::size_t skipped_byte_count{};
  Json::Value inspection;
};

DirectCurrentResult DownloadMarineIe(
    const std::filesystem::path& output, bool overwrite = false,
    BinaryDownload download = {}, double timeout_seconds = 120.0);
Json::Value DirectCurrentResultJson(const DirectCurrentResult& result);

}  // namespace environmental_grib

