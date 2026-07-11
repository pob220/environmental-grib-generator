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

class UnsupportedSourceError : public Error {
 public:
  using Error::Error;
};

}  // namespace environmental_grib

