#include "environmental_grib/copernicus_auth.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <json/json.h>

namespace environmental_grib {

std::string CopernicusAuthenticationFailure(long status,
                                            const std::string& response) {
  Json::Value reply;
  if (response.size() <= 1024 * 1024) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string ignored;
    if (!reader->parse(response.data(), response.data() + response.size(),
                       &reply, &ignored) || !reply.isObject())
      reply = Json::Value();
  }
  const auto field = [&reply](const char* key) {
    return reply.isObject() && reply[key].isString() ? reply[key].asString()
                                                    : std::string();
  };
  const auto code = field("error");
  auto description = field("error_description");
  std::transform(description.begin(), description.end(), description.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::string message;
  std::string safe_code;
  if (status == 429 || status >= 500) {
    message = "The Copernicus Marine sign-in service is temporarily unavailable or busy. Try again later.";
  } else if (code == "invalid_grant") {
    safe_code = code;
    if (description.find("fully set up") != std::string::npos ||
        description.find("required action") != std::string::npos ||
        description.find("email not verified") != std::string::npos) {
      message = "Your Copernicus Marine account needs attention. Sign in to the Marine website and complete any account setup, email verification or required actions.";
    } else if (description.find("disabled") != std::string::npos ||
               description.find("locked") != std::string::npos) {
      message = "Your Copernicus Marine account is disabled or temporarily locked. Sign in to the Marine website or contact Copernicus Marine support.";
    } else if (description == "invalid user credentials") {
      message = "Copernicus Marine rejected the username/email or password. Check your Marine account credentials and try a fresh sign-in on the Marine website.";
    } else {
      message = "Copernicus Marine did not accept this account login. Check your Marine username/email and password, and sign in to the Marine website to check for required account actions.";
    }
  } else if (code == "invalid_request" || code == "invalid_client" ||
             code == "unauthorized_client" || code == "unsupported_grant_type" ||
             code == "invalid_scope") {
    safe_code = code;
    message = "Copernicus Marine rejected xGRIB's sign-in request. This does not establish that your password is wrong. Please report this error with your xGRIB version.";
  } else if (status == 401 || status == 403) {
    message = "Copernicus Marine refused this sign-in. Check your Marine account credentials and account status on the Marine website.";
  } else {
    message = "Copernicus Marine sign-in failed. The service did not provide a recognised authentication reason. Check your Marine account on the Marine website; if it works there, report this error with your xGRIB version.";
  }
  return message + " (HTTP " + std::to_string(status) +
         (safe_code.empty() ? "" : "; " + safe_code) + ")";
}

}  // namespace environmental_grib
