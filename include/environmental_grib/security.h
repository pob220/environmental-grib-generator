#pragma once

#include <map>
#include <string>
#include <vector>

namespace environmental_grib {

inline constexpr const char* kRedacted = "<redacted>";

bool IsSensitiveKey(const std::string& key);
std::string RedactText(const std::string& text,
                       const std::vector<std::string>& sensitive_values = {});
std::map<std::string, std::string> RedactMapping(
    const std::map<std::string, std::string>& values);

}  // namespace environmental_grib

