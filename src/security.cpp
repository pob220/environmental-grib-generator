#include "environmental_grib/security.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace environmental_grib {

bool IsSensitiveKey(const std::string& key) {
  std::string lower = key;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  static constexpr const char* keys[] = {"password", "token", "secret", "credential",
                                         "api_key", "apikey", "username", "user", "email"};
  return std::any_of(std::begin(keys), std::end(keys), [&](const char* value) {
    return lower.find(value) != std::string::npos;
  });
}

std::string RedactText(const std::string& text,
                       const std::vector<std::string>& sensitive_values) {
  static const std::regex query(
      R"(([?&](?:x-cop-user|username|user|email|token|access_token|api_key|apikey|password)=)([^&#\s]+))",
      std::regex::icase);
  std::string result = std::regex_replace(text, query, "$1<redacted>");
  for (const auto& value : sensitive_values) {
    if (value.empty()) continue;
    std::size_t pos = 0;
    while ((pos = result.find(value, pos)) != std::string::npos) {
      result.replace(pos, value.size(), kRedacted);
      pos += std::char_traits<char>::length(kRedacted);
    }
  }
  return result;
}

std::map<std::string, std::string> RedactMapping(
    const std::map<std::string, std::string>& values) {
  std::map<std::string, std::string> result;
  for (const auto& [key, value] : values) {
    result[key] = IsSensitiveKey(key) && !value.empty() ? kRedacted : RedactText(value);
  }
  return result;
}

}  // namespace environmental_grib
