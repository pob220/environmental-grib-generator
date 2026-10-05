#pragma once

#include "environmental_grib/weather.h"
#include "environmental_grib/error.h"

namespace environmental_grib {

inline constexpr const char* kGfsFallbackOrigin =
    "https://grib.agentracert.com";

// Only coverage failures may advance an automatically selected forecast cycle.
// Service outages and invalid results stop the job instead of submitting every
// older cycle to the same unavailable service.
class HostedGfsServiceError : public Error {
public:
  using Error::Error;
};
class HostedGfsCoverageUnavailable : public ValidationError {
public:
  using ValidationError::ValidationError;
};

struct HostedHttpReply {
  long status{};
  std::vector<unsigned char> body;
  int retry_after_seconds{};
};
using HostedHttp = std::function<HostedHttpReply(
    const std::string&, const std::string&, const std::string&,
    const std::string&, double)>;

bool IsGfsFailoverError(const HttpDownloadError& error);
HostedGfsDownload BuiltinHostedGfsDownload();
std::vector<unsigned char> DownloadHostedGfs(const GFSRequest& request,
                                             const GFSCycle& cycle,
                                             const std::vector<int>& hours,
                                             ProgressCallback progress = {},
                                             HostedHttp http = {},
                                             RetrySleeper sleeper = {});
std::map<int, std::vector<unsigned char>> ValidateHostedGfsBytes(
    const std::vector<unsigned char>& bytes, const GFSRequest& request,
    const GFSCycle& cycle, const std::vector<int>& hours);

}  // namespace environmental_grib
