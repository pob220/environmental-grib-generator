#include "environmental_grib/grib.h"

#include <eccodes.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <unistd.h>

#include "environmental_grib/error.h"
#include "environmental_grib/geo.h"

namespace environmental_grib {
namespace {

using HandlePtr = std::unique_ptr<codes_handle, decltype(&codes_handle_delete)>;

std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) throw ValidationError("unable to read file: " + path.string());
  const auto end = stream.tellg();
  if (end < 0) throw ValidationError("unable to determine file size: " + path.string());
  std::vector<unsigned char> bytes(static_cast<std::size_t>(end));
  stream.seekg(0);
  if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), end)) {
    throw ValidationError("unable to read file: " + path.string());
  }
  return bytes;
}

std::uint64_t ReadBigEndian(const unsigned char* value, std::size_t size) {
  std::uint64_t result = 0;
  for (std::size_t i = 0; i < size; ++i) result = (result << 8U) | value[i];
  return result;
}

std::size_t MessageLengthAt(std::span<const unsigned char> data,
                            std::size_t offset) {
  if (offset + 8 > data.size()) throw ValidationError("truncated GRIB header at byte offset " + std::to_string(offset));
  const unsigned edition = data[offset + 7];
  if (edition == 1) return static_cast<std::size_t>(ReadBigEndian(data.data() + offset + 4, 3));
  if (edition == 2) {
    if (offset + 16 > data.size()) throw ValidationError("truncated GRIB2 header at byte offset " + std::to_string(offset));
    const auto length = ReadBigEndian(data.data() + offset + 8, 8);
    if (length > std::numeric_limits<std::size_t>::max()) throw ValidationError("GRIB message length exceeds platform limits");
    return static_cast<std::size_t>(length);
  }
  throw ValidationError("unsupported GRIB edition " + std::to_string(edition) + " at byte offset " + std::to_string(offset));
}

void ValidateMessage(std::span<const unsigned char> data, std::size_t offset,
                     std::size_t length) {
  if (length == 0 || length > data.size() - offset) {
    throw ValidationError("invalid GRIB message length " + std::to_string(length) + " at byte offset " + std::to_string(offset));
  }
  if (length < 4 || std::memcmp(data.data() + offset + length - 4, "7777", 4) != 0) {
    throw ValidationError("GRIB terminator not found for message at byte offset " + std::to_string(offset));
  }
}

void Check(int error, const std::string& operation) {
  if (error != 0) throw ValidationError(operation + ": " + codes_get_error_message(error));
}

void SetLong(codes_handle* handle, const char* key, long value) {
  Check(codes_set_long(handle, key, value), std::string("setting ") + key);
}

void SetDouble(codes_handle* handle, const char* key, double value) {
  Check(codes_set_double(handle, key, value), std::string("setting ") + key);
}

std::optional<long> GetLong(codes_handle* handle, const char* key) {
  long value = 0;
  return codes_get_long(handle, key, &value) == 0 ? std::optional<long>{value} : std::nullopt;
}

std::optional<TimePoint> ValidityTime(codes_handle* handle) {
  const auto date = GetLong(handle, "validityDate");
  const auto time = GetLong(handle, "validityTime");
  if (!date || !time) return std::nullopt;
  const int year = static_cast<int>(*date / 10000);
  const int month = static_cast<int>((*date / 100) % 100);
  const int day = static_cast<int>(*date % 100);
  const int hour = static_cast<int>(*time / 100);
  const int minute = static_cast<int>(*time % 100);
  char value[32];
  std::snprintf(value, sizeof(value), "%04d-%02d-%02dT%02d:%02d:00Z", year,
                month, day, hour, minute);
  return ParseUtcDateTime(value);
}

std::optional<double> GetDouble(codes_handle* handle, const char* key) {
  double value = 0.0;
  return codes_get_double(handle, key, &value) == 0 ? std::optional<double>{value} : std::nullopt;
}

std::optional<std::string> GetString(codes_handle* handle, const char* key) {
  std::array<char, 512> value{};
  std::size_t size = value.size();
  return codes_get_string(handle, key, value.data(), &size) == 0
             ? std::optional<std::string>{value.data()}
             : std::nullopt;
}

double NormalizeLongitude(double value) {
  double normalized = std::fmod(value + 180.0, 360.0);
  if (normalized < 0.0) normalized += 360.0;
  normalized -= 180.0;
  return normalized == -180.0 && value > 0.0 ? 180.0 : normalized;
}

void Increment(Json::Value& object, const std::string& key) {
  object[key] = object.get(key, 0).asUInt64() + 1;
}

std::filesystem::path TemporarySibling(const std::filesystem::path& output) {
  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    auto candidate = output;
    candidate += "." + std::to_string(::getpid()) + "." + std::to_string(attempt) + ".tmp";
    if (!std::filesystem::exists(candidate)) return candidate;
  }
  throw ValidationError("unable to allocate temporary output beside " + output.string());
}

}  // namespace

GribScanResult ScanGribMessages(const std::filesystem::path& path) {
  const auto data = ReadBytes(path);
  return ScanGribBytes(data);
}

GribScanResult ScanGribBytes(std::span<const unsigned char> data) {
  std::size_t offset = 0;
  std::size_t count = 0;
  while (offset < data.size()) {
    if (offset + 4 > data.size() || std::memcmp(data.data() + offset, "GRIB", 4) != 0) {
      throw ValidationError("GRIB marker not found at byte offset " + std::to_string(offset));
    }
    const std::size_t length = MessageLengthAt(data, offset);
    ValidateMessage(data, offset, length);
    offset += length;
    ++count;
  }
  return {count, data.size()};
}

GribNormalizeResult NormalizeGribStream(const std::filesystem::path& input,
                                        const std::filesystem::path& output) {
  const auto data = ReadBytes(input);
  std::vector<unsigned char> clean;
  std::size_t offset = 0, skipped = 0, count = 0;
  while (offset < data.size()) {
    static constexpr std::array<unsigned char, 4> kMarker{'G', 'R', 'I', 'B'};
    auto marker = std::search(data.begin() + static_cast<std::ptrdiff_t>(offset),
                              data.end(), kMarker.begin(), kMarker.end());
    if (marker == data.end()) {
      skipped += data.size() - offset;
      break;
    }
    const std::size_t position = static_cast<std::size_t>(marker - data.begin());
    skipped += position - offset;
    const std::size_t length = MessageLengthAt(data, position);
    ValidateMessage(data, position, length);
    clean.insert(clean.end(), marker, marker + static_cast<std::ptrdiff_t>(length));
    offset = position + length;
    ++count;
  }
  if (count == 0) throw ValidationError("no complete GRIB messages found in " + input.string());
  std::filesystem::create_directories(output.parent_path().empty() ? "." : output.parent_path());
  std::ofstream stream(output, std::ios::binary | std::ios::trunc);
  if (!stream || !stream.write(reinterpret_cast<const char*>(clean.data()), static_cast<std::streamsize>(clean.size()))) {
    throw ValidationError("unable to write normalized GRIB: " + output.string());
  }
  stream.close();
  ScanGribMessages(output);
  return {count, data.size(), clean.size(), skipped};
}

Json::Value InspectGrib(const std::filesystem::path& path) {
  const auto scan = ScanGribMessages(path);
  const auto data = ReadBytes(path);
  Json::Value result(Json::objectValue);
  result["path"] = path.string();
  result["message_count"] = Json::UInt64(scan.message_count);
  result["byte_count"] = Json::UInt64(scan.byte_count);
  result["stream_valid"] = true;
  Json::Value editions(Json::objectValue);
  for (std::size_t offset = 0; offset < data.size();) {
    Increment(editions, std::to_string(data[offset + 7]));
    offset += MessageLengthAt(data, offset);
  }
  result["edition_counts"] = editions;
  result["eccodes_available"] = true;

  Json::Value parameters(Json::objectValue), names(Json::objectValue),
      short_names(Json::objectValue), grib2(Json::objectValue);
  Json::Value currents(Json::objectValue);
  currents["u_49"] = 0;
  currents["v_50"] = 0;
  std::set<std::string> valid_times;
  bool have_coverage = false;
  Json::Value coverage(Json::objectValue);
  FILE* raw = std::fopen(path.c_str(), "rb");
  if (!raw) throw ValidationError("unable to open GRIB with ecCodes: " + path.string());
  struct FileCloser { void operator()(FILE* value) const { std::fclose(value); } };
  std::unique_ptr<FILE, FileCloser> file(raw);
  while (true) {
    int error = 0;
    HandlePtr handle(codes_handle_new_from_file(nullptr, file.get(), PRODUCT_GRIB, &error), &codes_handle_delete);
    if (!handle) {
      if (error == CODES_SUCCESS || std::feof(file.get())) break;
      Check(error, "reading GRIB message");
      break;
    }
    auto parameter = GetLong(handle.get(), "indicatorOfParameter");
    if (!parameter) parameter = GetLong(handle.get(), "parameterNumber");
    if (parameter) {
      const std::string key = std::to_string(*parameter);
      Increment(parameters, key);
      if (auto name = GetString(handle.get(), "parameterName")) names[key] = *name;
      else if (auto name = GetString(handle.get(), "shortName")) names[key] = *name;
      if (*parameter == 49) currents["u_49"] = currents["u_49"].asUInt64() + 1;
      if (*parameter == 50) currents["v_50"] = currents["v_50"].asUInt64() + 1;
    }
    if (auto short_name = GetString(handle.get(), "shortName")) Increment(short_names, *short_name);
    auto discipline = GetLong(handle.get(), "discipline");
    auto category = GetLong(handle.get(), "parameterCategory");
    auto number = GetLong(handle.get(), "parameterNumber");
    if (discipline && category && number) Increment(grib2, std::to_string(*discipline) + ":" + std::to_string(*category) + ":" + std::to_string(*number));
    auto date = GetLong(handle.get(), "validityDate");
    auto time = GetLong(handle.get(), "validityTime");
    if (date && time) {
      std::ostringstream value;
      value << std::setw(8) << std::setfill('0') << *date << "T" << std::setw(4) << *time;
      valid_times.insert(value.str());
    }
    if (!have_coverage) {
      auto west = GetDouble(handle.get(), "longitudeOfFirstGridPointInDegrees");
      auto south = GetDouble(handle.get(), "latitudeOfFirstGridPointInDegrees");
      auto east = GetDouble(handle.get(), "longitudeOfLastGridPointInDegrees");
      auto north = GetDouble(handle.get(), "latitudeOfLastGridPointInDegrees");
      if (west && south && east && north) {
        coverage["west"] = NormalizeLongitude(*west);
        coverage["south"] = *south;
        coverage["east"] = NormalizeLongitude(*east);
        coverage["north"] = *north;
        have_coverage = true;
      }
    }
  }
  result["parameter_counts"] = parameters;
  result["parameter_names"] = names;
  result["short_name_counts"] = short_names;
  result["grib2_parameter_counts"] = grib2;
  result["current_component_counts"] = currents;
  if (!valid_times.empty()) {
    result["first_valid_time"] = *valid_times.begin();
    result["last_valid_time"] = *valid_times.rbegin();
  }
  if (have_coverage) result["coverage"] = coverage;
  return result;
}

GribWriteSummary WriteGrib1Currents(const std::vector<CurrentGrid>& grids,
                                    const std::filesystem::path& output) {
  if (grids.empty()) throw ValidationError("no current grids were provided for GRIB writing");
  for (const auto& grid : grids) grid.Validate();
  std::filesystem::create_directories(output.parent_path().empty() ? "." : output.parent_path());
  std::ofstream stream(output, std::ios::binary | std::ios::trunc);
  if (!stream) throw ValidationError("unable to create GRIB output: " + output.string());
  const TimePoint reference = std::chrono::floor<std::chrono::hours>(grids.front().time);
  std::size_t count = 0;
  for (const auto& current : grids) {
    const auto forecast_hours = std::chrono::duration_cast<std::chrono::hours>(current.time - reference).count();
    if (forecast_hours < 0) throw ValidationError("current grids must not be earlier than the GRIB reference time");
    const auto epoch = reference.time_since_epoch().count();
    std::time_t raw_time = static_cast<std::time_t>(epoch);
    std::tm tm{};
    gmtime_r(&raw_time, &tm);
    for (const auto& [parameter, source_values] : std::array<std::pair<long, const std::vector<double>*>, 2>{{{49, &current.u_mps}, {50, &current.v_mps}}}) {
      HandlePtr handle(codes_handle_new_from_samples(nullptr, "regular_ll_sfc_grib1"), &codes_handle_delete);
      if (!handle) throw ValidationError("ecCodes could not create regular_ll_sfc_grib1 sample");
      SetLong(handle.get(), "editionNumber", 1);
      SetLong(handle.get(), "table2Version", 2);
      SetLong(handle.get(), "indicatorOfParameter", parameter);
      SetLong(handle.get(), "indicatorOfTypeOfLevel", 1);
      SetLong(handle.get(), "level", 0);
      SetLong(handle.get(), "dataDate", (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
      SetLong(handle.get(), "dataTime", tm.tm_hour * 100 + tm.tm_min);
      SetLong(handle.get(), "indicatorOfUnitOfTimeRange", 1);
      SetLong(handle.get(), "P1", static_cast<long>(forecast_hours));
      SetLong(handle.get(), "P2", 0);
      SetLong(handle.get(), "timeRangeIndicator", 0);
      SetLong(handle.get(), "Ni", static_cast<long>(current.grid.nx()));
      SetLong(handle.get(), "Nj", static_cast<long>(current.grid.ny()));
      SetDouble(handle.get(), "latitudeOfFirstGridPointInDegrees", current.grid.latitudes.front());
      SetDouble(handle.get(), "longitudeOfFirstGridPointInDegrees", current.grid.longitudes.front());
      SetDouble(handle.get(), "latitudeOfLastGridPointInDegrees", current.grid.latitudes.back());
      SetDouble(handle.get(), "longitudeOfLastGridPointInDegrees", current.grid.longitudes.back());
      SetDouble(handle.get(), "iDirectionIncrementInDegrees", current.grid.longitude_spacing_deg);
      SetDouble(handle.get(), "jDirectionIncrementInDegrees", current.grid.latitude_spacing_deg);
      SetLong(handle.get(), "iScansNegatively", 0);
      SetLong(handle.get(), "jScansPositively", 1);
      SetLong(handle.get(), "jPointsAreConsecutive", 0);
      SetLong(handle.get(), "bitsPerValue", 16);
      std::vector<double> values = *source_values;
      bool missing = false;
      for (std::size_t i = 0; i < values.size(); ++i) {
        if ((current.has_mask() && current.mask[i]) ||
            !std::isfinite(values[i])) {
          values[i] = 9999.0;
          missing = true;
        }
      }
      if (missing) {
        SetLong(handle.get(), "bitmapPresent", 1);
        SetDouble(handle.get(), "missingValue", 9999.0);
      }
      std::size_t size = values.size();
      Check(codes_set_double_array(handle.get(), "values", values.data(), size), "setting values");
      const void* message = nullptr;
      std::size_t length = 0;
      Check(codes_get_message(handle.get(), &message, &length), "encoding GRIB message");
      stream.write(static_cast<const char*>(message), static_cast<std::streamsize>(length));
      if (!stream) throw ValidationError("writing GRIB message failed");
      ++count;
    }
  }
  stream.close();
  const auto scan = ScanGribMessages(output);
  if (scan.message_count != count) throw ValidationError("written GRIB message count mismatch");
  return {count, output};
}

GribWriteSummary WriteRegularLatLonGrib2(
    const RegularGrid& grid, TimePoint reference,
    const std::vector<Grib2Field>& fields,
    const std::filesystem::path& output) {
  if (fields.empty()) throw ValidationError("no GRIB2 fields were provided");
  if (grid.nx() < 2 || grid.ny() < 2) throw ValidationError("GRIB2 output requires at least a 2 x 2 grid");
  const std::time_t raw_time = static_cast<std::time_t>(reference.time_since_epoch().count());
  std::tm tm{}; gmtime_r(&raw_time, &tm);
  std::filesystem::create_directories(output.parent_path().empty() ? "." : output.parent_path());
  std::ofstream stream(output, std::ios::binary | std::ios::trunc);
  if (!stream) throw ValidationError("unable to create GRIB2 output: " + output.string());
  std::size_t count = 0;
  for (const auto& field : fields) {
    if (field.values.size() != grid.size()) throw ValidationError("GRIB2 field " + field.short_name + " does not match grid shape");
    if (!field.mask.empty() && field.mask.size() != grid.size()) throw ValidationError("GRIB2 field mask does not match grid shape");
    if (field.forecast_hour < 0) throw ValidationError("GRIB2 forecast hour must not be negative");
    HandlePtr handle(codes_handle_new_from_samples(nullptr, "regular_ll_sfc_grib2"), &codes_handle_delete);
    if (!handle) throw ValidationError("ecCodes could not create regular_ll_sfc_grib2 sample");
    SetLong(handle.get(), "editionNumber", 2);
    SetLong(handle.get(), "discipline", 0);
    SetLong(handle.get(), "productDefinitionTemplateNumber", 0);
    SetLong(handle.get(), "typeOfGeneratingProcess", 2);
    SetLong(handle.get(), "generatingProcessIdentifier", 255);
    std::size_t short_name_length = field.short_name.size();
    Check(codes_set_string(handle.get(), "shortName", field.short_name.c_str(),
                           &short_name_length),
          "setting GRIB2 shortName " + field.short_name);
    SetLong(handle.get(), "dataDate", (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
    SetLong(handle.get(), "dataTime", tm.tm_hour * 100 + tm.tm_min);
    SetLong(handle.get(), "stepUnits", 1);
    SetLong(handle.get(), "forecastTime", field.forecast_hour);
    SetLong(handle.get(), "Ni", static_cast<long>(grid.nx()));
    SetLong(handle.get(), "Nj", static_cast<long>(grid.ny()));
    SetDouble(handle.get(), "latitudeOfFirstGridPointInDegrees", grid.latitudes.front());
    SetDouble(handle.get(), "longitudeOfFirstGridPointInDegrees", grid.longitudes.front());
    SetDouble(handle.get(), "latitudeOfLastGridPointInDegrees", grid.latitudes.back());
    SetDouble(handle.get(), "longitudeOfLastGridPointInDegrees", grid.longitudes.back());
    SetDouble(handle.get(), "iDirectionIncrementInDegrees", grid.longitude_spacing_deg);
    SetDouble(handle.get(), "jDirectionIncrementInDegrees", grid.latitude_spacing_deg);
    SetLong(handle.get(), "iScansNegatively", 0);
    SetLong(handle.get(), "jScansPositively", 1);
    SetLong(handle.get(), "jPointsAreConsecutive", 0);
    SetLong(handle.get(), "bitsPerValue", 24);
    std::vector<double> values = field.values;
    bool missing = false;
    for (std::size_t i = 0; i < values.size(); ++i) {
      if ((!field.mask.empty() && field.mask[i]) || !std::isfinite(values[i])) {
        values[i] = 9999.0;
        missing = true;
      }
    }
    if (missing) {
      SetLong(handle.get(), "bitmapPresent", 1);
      SetDouble(handle.get(), "missingValue", 9999.0);
    }
    Check(codes_set_double_array(handle.get(), "values", values.data(), values.size()),
          "setting GRIB2 values");
    const void* message = nullptr;
    std::size_t length = 0;
    Check(codes_get_message(handle.get(), &message, &length), "encoding GRIB2 message");
    stream.write(static_cast<const char*>(message), static_cast<std::streamsize>(length));
    if (!stream) throw ValidationError("writing GRIB2 message failed");
    ++count;
  }
  stream.close();
  const auto scan = ScanGribMessages(output);
  if (scan.message_count != count) throw ValidationError("written GRIB2 message count mismatch");
  return {count, output};
}

MergeStreamsResult MergeGribStreams(
    const std::vector<std::pair<std::string, std::filesystem::path>>& inputs,
    const std::filesystem::path& output, bool overwrite) {
  if (inputs.empty()) throw ValidationError("at least one GRIB input is required");
  if (std::filesystem::exists(output) && std::filesystem::is_directory(output)) throw ValidationError("output must be a file path, not a directory");
  if (std::filesystem::exists(output) && !overwrite) throw ValidationError("output already exists: " + output.string() + "; use --overwrite to replace it");
  std::map<std::string, std::size_t> counts;
  std::size_t expected = 0;
  for (const auto& [label, path] : inputs) {
    if (!std::filesystem::exists(path)) throw ValidationError(label + " GRIB not found: " + path.string());
    const auto scan = ScanGribMessages(path);
    counts[label] = scan.message_count;
    expected += scan.message_count;
  }
  std::filesystem::create_directories(output.parent_path().empty() ? "." : output.parent_path());
  const auto temporary = TemporarySibling(output);
  try {
    std::ofstream destination(temporary, std::ios::binary | std::ios::trunc);
    if (!destination) throw ValidationError("unable to create merged GRIB temporary file");
    for (const auto& [label, path] : inputs) {
      (void)label;
      std::ifstream source(path, std::ios::binary);
      destination << source.rdbuf();
      if (!source.eof() && source.fail()) throw ValidationError("failed reading input GRIB: " + path.string());
      if (!destination) throw ValidationError("failed writing merged GRIB");
    }
    destination.close();
    const auto scan = ScanGribMessages(temporary);
    if (scan.message_count != expected) throw ValidationError("merged GRIB message count mismatch");
    Json::Value inspection = InspectGrib(temporary);
    std::error_code error;
    std::filesystem::rename(temporary, output, error);
    if (error) throw ValidationError("publishing merged GRIB failed: " + error.message());
    return {counts, scan.message_count, scan.byte_count, inspection};
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

MergeStreamsResult CompositeGribStreamsPreferFirst(
    const std::vector<std::pair<std::string, std::filesystem::path>>& inputs,
    const std::filesystem::path& output, bool overwrite,
    std::optional<TimePoint> valid_from,
    std::optional<TimePoint> valid_through) {
  if (inputs.empty())
    throw ValidationError("at least one GRIB input is required");
  if (std::filesystem::exists(output) &&
      std::filesystem::is_directory(output))
    throw ValidationError("output must be a file path, not a directory");
  if (std::filesystem::exists(output) && !overwrite)
    throw ValidationError("output already exists: " + output.string() +
                          "; use --overwrite to replace it");

  std::map<std::string, std::size_t> counts;
  for (const auto& [label, path] : inputs) {
    if (!std::filesystem::is_regular_file(path))
      throw ValidationError(label + " GRIB not found: " + path.string());
    counts[label] = ScanGribMessages(path).message_count;
  }

  std::filesystem::create_directories(
      output.parent_path().empty() ? "." : output.parent_path());
  const auto temporary = TemporarySibling(output);
  std::set<std::string> retained;
  std::size_t output_count = 0;
  try {
    std::ofstream destination(temporary, std::ios::binary | std::ios::trunc);
    if (!destination)
      throw ValidationError("unable to create composite GRIB temporary file");

    for (const auto& [label, path] : inputs) {
      FILE* raw = std::fopen(path.c_str(), "rb");
      if (!raw)
        throw ValidationError("unable to open " + label + " GRIB: " +
                              path.string());
      struct FileCloser {
        void operator()(FILE* value) const { std::fclose(value); }
      };
      std::unique_ptr<FILE, FileCloser> file(raw);
      std::size_t source_index = 0;
      while (true) {
        int error = 0;
        HandlePtr handle(codes_handle_new_from_file(
                             nullptr, file.get(), PRODUCT_GRIB, &error),
                         &codes_handle_delete);
        if (!handle) {
          if (error == CODES_SUCCESS || std::feof(file.get())) break;
          Check(error, "reading " + label + " GRIB message");
          break;
        }

        if (const auto validity = ValidityTime(handle.get()); validity) {
          if ((valid_from && *validity < *valid_from) ||
              (valid_through && *validity > *valid_through))
            continue;
        }

        auto short_name = GetString(handle.get(), "shortName");
        if (!short_name || short_name->empty() || *short_name == "unknown") {
          if (const auto parameter =
                  GetLong(handle.get(), "indicatorOfParameter"))
            short_name = "parameter-" + std::to_string(*parameter);
          else if (const auto parameter = GetLong(handle.get(), "paramId"))
            short_name = "parameter-" + std::to_string(*parameter);
        }
        const auto level_type = GetString(handle.get(), "typeOfLevel")
                                    .value_or("unknown-level");
        const auto level = GetLong(handle.get(), "level");
        const auto date = GetLong(handle.get(), "validityDate");
        const auto time = GetLong(handle.get(), "validityTime");
        std::string key;
        if (short_name && date && time) {
          key = *short_name + '|' + level_type + '|' +
                std::to_string(level.value_or(0)) + '|' +
                std::to_string(*date) + '|' +
                std::to_string(*time);
        } else {
          // Preserve unusual messages which do not expose the standard
          // semantic keys; they cannot safely be classified as duplicates.
          key = label + "|unclassified|" + std::to_string(source_index);
        }
        ++source_index;
        if (!retained.insert(key).second) continue;

        const void* message = nullptr;
        std::size_t length = 0;
        Check(codes_get_message(handle.get(), &message, &length),
              "reading encoded " + label + " GRIB message");
        destination.write(static_cast<const char*>(message),
                          static_cast<std::streamsize>(length));
        if (!destination)
          throw ValidationError("failed writing composite GRIB");
        ++output_count;
      }
    }
    destination.close();
    const auto scan = ScanGribMessages(temporary);
    if (scan.message_count != output_count)
      throw ValidationError("composite GRIB message count mismatch");
    Json::Value inspection = InspectGrib(temporary);
    std::error_code error;
    std::filesystem::rename(temporary, output, error);
    if (error)
      throw ValidationError("publishing composite GRIB failed: " +
                            error.message());
    return {counts, scan.message_count, scan.byte_count, inspection};
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

}  // namespace environmental_grib
