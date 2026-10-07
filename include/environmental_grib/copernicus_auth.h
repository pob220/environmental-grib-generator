#pragma once

#include <string>
#include "environmental_grib/error.h"

namespace environmental_grib {

class CopernicusAuthenticationError : public ValidationError {
 public:
  using ValidationError::ValidationError;
};

// Classifies an untrusted response using fixed messages. Never returns raw
// server descriptions, HTML, unknown error codes or authentication tokens.
std::string CopernicusAuthenticationFailure(long status,
                                            const std::string& response);

}  // namespace environmental_grib
