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
  HttpDownloadError(const std::string& message, bool transient)
      : ValidationError(message), transient_(transient) {}

  [[nodiscard]] bool transient() const noexcept { return transient_; }

 private:
  bool transient_{};
};

class UnsupportedSourceError : public Error {
 public:
  using Error::Error;
};

}  // namespace environmental_grib
