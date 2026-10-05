#pragma once

#include <stdexcept>

namespace environmental_grib {

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class ValidationError : public Error {
 public:
  using Error::Error;
};

class HttpDownloadError : public ValidationError {
 public:
   HttpDownloadError(const std::string& message, bool transient,
                     long status = 0, bool forecast_missing = false)
       : ValidationError(message),
         transient_(transient),
         status_(status),
         forecast_missing_(forecast_missing) {}

   [[nodiscard]] bool transient() const noexcept { return transient_; }
   [[nodiscard]] long status() const noexcept { return status_; }
   [[nodiscard]] bool forecast_missing() const noexcept {
     return forecast_missing_;
   }

 private:
  bool transient_{};
  long status_{};
  bool forecast_missing_{};
};

class UnsupportedSourceError : public Error {
 public:
  using Error::Error;
};

}  // namespace environmental_grib
