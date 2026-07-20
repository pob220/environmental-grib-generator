#include "environmental_grib/remote_currents.h"

#include <fstream>

#include "environmental_grib/error.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/platform.h"
#include "environmental_grib/weather.h"

namespace environmental_grib {
namespace {
constexpr const char* kMarineIeUrl =
    "ftp://ftpossapp2:FtpOssapp2@ftp.marine.ie/OSS/modelling/GRIB_Files/"
    "irish_sea_ms.grb";

std::filesystem::path TemporarySibling(const std::filesystem::path& output,
                                       const std::string& suffix) {
  auto path = output;
  path += "." + std::to_string(ProcessId()) + suffix;
  return path;
}
}  // namespace

DirectCurrentResult DownloadMarineIe(const std::filesystem::path& output,
                                     bool overwrite, BinaryDownload download,
                                     double timeout_seconds) {
  if (std::filesystem::exists(output) &&
      std::filesystem::is_directory(output)) {
    throw ValidationError("output must be a file path, not a directory");
  }
  if (std::filesystem::exists(output) && !overwrite) {
    throw ValidationError("output already exists: " + output.string() +
                          "; enable overwrite to replace it");
  }
  if (!download) download = CurlHttpGet;
  const auto raw_bytes = download(kMarineIeUrl, timeout_seconds);
  if (raw_bytes.empty())
    throw ValidationError("Marine.ie download returned empty response");
  std::filesystem::create_directories(
      output.parent_path().empty() ? "." : output.parent_path());
  const auto raw_path = TemporarySibling(output, ".download");
  const auto clean_path = TemporarySibling(output, ".tmp");
  try {
    std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
    raw.write(reinterpret_cast<const char*>(raw_bytes.data()),
              static_cast<std::streamsize>(raw_bytes.size()));
    if (!raw) throw ValidationError("writing Marine.ie download failed");
    raw.close();
    const auto normalized = NormalizeGribStream(raw_path, clean_path);
    auto inspection = InspectGrib(clean_path);
    const auto u_count =
        inspection["current_component_counts"]["u_49"].asUInt64();
    const auto v_count =
        inspection["current_component_counts"]["v_50"].asUInt64();
    if (u_count == 0 || v_count == 0 || u_count != v_count) {
      throw ValidationError(
          "Marine.ie GRIB does not contain balanced u/v current components");
    }
    std::filesystem::rename(clean_path, output);
    std::error_code ignored;
    std::filesystem::remove(raw_path, ignored);
    return {"marine_ie_irish_sea",         output,
            normalized.raw_byte_count,     normalized.clean_byte_count,
            normalized.skipped_byte_count, inspection};
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(raw_path, ignored);
    std::filesystem::remove(clean_path, ignored);
    throw;
  }
}

Json::Value DirectCurrentResultJson(const DirectCurrentResult& result) {
  Json::Value value(Json::objectValue);
  value["provider"] = result.provider;
  value["output"] = result.output.string();
  value["raw_byte_count"] = Json::UInt64(result.raw_byte_count);
  value["clean_byte_count"] = Json::UInt64(result.clean_byte_count);
  value["skipped_byte_count"] = Json::UInt64(result.skipped_byte_count);
  value["inspection"] = result.inspection;
  return value;
}

}  // namespace environmental_grib
