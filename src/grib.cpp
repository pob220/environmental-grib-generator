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

#include "environmental_grib/error.h"
#include "environmental_grib/geo.h"
#include "environmental_grib/platform.h"

namespace environmental_grib {
namespace {

using HandlePtr = std::unique_ptr<codes_handle, decltype(&codes_handle_delete)>;

std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) throw ValidationError("unable to read file: " + path.string());
  const auto end = stream.tellg();
  if (end < 0)
    throw ValidationError("unable to determine file size: " + path.string());
  std::vector<unsigned char> bytes(static_cast<std::size_t>(end));
  stream.seekg(0);
  if (!bytes.empty() &&
      !stream.read(reinterpret_cast<char*>(bytes.data()), end)) {
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
  if (offset + 8 > data.size())
    throw ValidationError("truncated GRIB header at byte offset " +
                          std::to_string(offset));
  const unsigned edition = data[offset + 7];
  if (edition == 1)
    return static_cast<std::size_t>(ReadBigEndian(data.data() + offset + 4, 3));
  if (edition == 2) {
    if (offset + 16 > data.size())
      throw ValidationError("truncated GRIB2 header at byte offset " +
                            std::to_string(offset));
    const auto length = ReadBigEndian(data.data() + offset + 8, 8);
    if (length > std::numeric_limits<std::size_t>::max())
      throw ValidationError("GRIB message length exceeds platform limits");
    return static_cast<std::size_t>(length);
  }
  throw ValidationError("unsupported GRIB edition " + std::to_string(edition) +
                        " at byte offset " + std::to_string(offset));
}

void ValidateMessage(std::span<const unsigned char> data, std::size_t offset,
                     std::size_t length) {
  if (length == 0 || length > data.size() - offset) {
    throw ValidationError("invalid GRIB message length " +
                          std::to_string(length) + " at byte offset " +
                          std::to_string(offset));
  }
  if (length < 4 ||
      std::memcmp(data.data() + offset + length - 4, "7777", 4) != 0) {
    throw ValidationError(
        "GRIB terminator not found for message at byte offset " +
        std::to_string(offset));
  }
}

void Check(int error, const std::string& operation) {
  if (error != 0)
    throw ValidationError(operation + ": " + codes_get_error_message(error));
}

void SetLong(codes_handle* handle, const char* key, long value) {
  Check(codes_set_long(handle, key, value), std::string("setting ") + key);
}

void SetDouble(codes_handle* handle, const char* key, double value) {
  Check(codes_set_double(handle, key, value), std::string("setting ") + key);
}

std::optional<long> GetLong(codes_handle* handle, const char* key) {
  long value = 0;
  return codes_get_long(handle, key, &value) == 0 ? std::optional<long>{value}
                                                  : std::nullopt;
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
  return codes_get_double(handle, key, &value) == 0
             ? std::optional<double>{value}
             : std::nullopt;
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

std::optional<std::vector<double>> GetValues(codes_handle* handle) {
  std::size_t size = 0;
  if (codes_get_size(handle, "values", &size) != 0) return std::nullopt;
  std::vector<double> values(size);
  if (codes_get_double_array(handle, "values", values.data(), &size) != 0)
    return std::nullopt;
  values.resize(size);
  return values;
}

std::optional<std::string> ValidityString(codes_handle* handle) {
  const auto date = GetLong(handle, "validityDate");
  const auto time = GetLong(handle, "validityTime");
  if (!date || !time) return std::nullopt;
  std::ostringstream value;
  value << std::setw(8) << std::setfill('0') << *date << "T" << std::setw(4)
        << std::setfill('0') << *time;
  return value.str();
}

void AddStringArray(Json::Value& destination,
                    const std::vector<std::string>& values) {
  for (const auto& value : values) destination.append(value);
}

std::vector<std::string> ObjectKeys(const Json::Value& object) {
  std::vector<std::string> result;
  for (const auto& name : object.getMemberNames()) result.push_back(name);
  return result;
}

std::filesystem::path TemporarySibling(const std::filesystem::path& output) {
  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    auto candidate = output;
    candidate += "." + std::to_string(ProcessId()) + "." +
                 std::to_string(attempt) + ".tmp";
    if (!std::filesystem::exists(candidate)) return candidate;
  }
  throw ValidationError("unable to allocate temporary output beside " +
                        output.string());
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
    if (offset + 4 > data.size() ||
        std::memcmp(data.data() + offset, "GRIB", 4) != 0) {
      throw ValidationError("GRIB marker not found at byte offset " +
                            std::to_string(offset));
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
    auto marker =
        std::search(data.begin() + static_cast<std::ptrdiff_t>(offset),
                    data.end(), kMarker.begin(), kMarker.end());
    if (marker == data.end()) {
      skipped += data.size() - offset;
      break;
    }
    const std::size_t position =
        static_cast<std::size_t>(marker - data.begin());
    skipped += position - offset;
    const std::size_t length = MessageLengthAt(data, position);
    ValidateMessage(data, position, length);
    clean.insert(clean.end(), marker,
                 marker + static_cast<std::ptrdiff_t>(length));
    offset = position + length;
    ++count;
  }
  if (count == 0)
    throw ValidationError("no complete GRIB messages found in " +
                          input.string());
  std::filesystem::create_directories(
      output.parent_path().empty() ? "." : output.parent_path());
  std::ofstream stream(output, std::ios::binary | std::ios::trunc);
  if (!stream || !stream.write(reinterpret_cast<const char*>(clean.data()),
                               static_cast<std::streamsize>(clean.size()))) {
    throw ValidationError("unable to write normalized GRIB: " +
                          output.string());
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
  currents["u_grib2"] = 0;
  currents["v_grib2"] = 0;
  Json::Value messages(Json::arrayValue);
  std::set<std::string> valid_times;
  bool have_coverage = false;
  Json::Value coverage(Json::objectValue);
  double coverage_west = 0.0, coverage_south = 0.0, coverage_east = 0.0,
         coverage_north = 0.0;
  FILE* raw = std::fopen(path.c_str(), "rb");
  if (!raw)
    throw ValidationError("unable to open GRIB with ecCodes: " + path.string());
  struct FileCloser {
    void operator()(FILE* value) const { std::fclose(value); }
  };
  std::unique_ptr<FILE, FileCloser> file(raw);
  while (true) {
    int error = 0;
    HandlePtr handle(
        codes_handle_new_from_file(nullptr, file.get(), PRODUCT_GRIB, &error),
        &codes_handle_delete);
    if (!handle) {
      if (error == CODES_SUCCESS || std::feof(file.get())) break;
      Check(error, "reading GRIB message");
      break;
    }
    Json::Value message(Json::objectValue);
    message["index"] = Json::UInt64(messages.size());
    if (const auto edition = GetLong(handle.get(), "editionNumber"))
      message["edition"] = Json::Int64(*edition);
    auto parameter = GetLong(handle.get(), "indicatorOfParameter");
    if (!parameter) parameter = GetLong(handle.get(), "parameterNumber");
    if (parameter) {
      const std::string key = std::to_string(*parameter);
      Increment(parameters, key);
      if (auto name = GetString(handle.get(), "parameterName"))
        names[key] = *name;
      else if (auto name = GetString(handle.get(), "shortName"))
        names[key] = *name;
      if (*parameter == 49) currents["u_49"] = currents["u_49"].asUInt64() + 1;
      if (*parameter == 50) currents["v_50"] = currents["v_50"].asUInt64() + 1;
      message["parameter_number"] = Json::Int64(*parameter);
    }
    if (auto short_name = GetString(handle.get(), "shortName")) {
      Increment(short_names, *short_name);
      message["short_name"] = *short_name;
    }
    if (auto parameter_name = GetString(handle.get(), "parameterName"))
      message["parameter_name"] = *parameter_name;
    auto discipline = GetLong(handle.get(), "discipline");
    auto category = GetLong(handle.get(), "parameterCategory");
    auto number = GetLong(handle.get(), "parameterNumber");
    if (discipline && category && number) {
      const std::string key = std::to_string(*discipline) + ":" +
                              std::to_string(*category) + ":" +
                              std::to_string(*number);
      Increment(grib2, key);
      message["discipline"] = Json::Int64(*discipline);
      message["parameter_category"] = Json::Int64(*category);
      message["parameter_number"] = Json::Int64(*number);
      if (*discipline == 10 && *category == 1 && *number == 2)
        currents["u_grib2"] = currents["u_grib2"].asUInt64() + 1;
      if (*discipline == 10 && *category == 1 && *number == 3)
        currents["v_grib2"] = currents["v_grib2"].asUInt64() + 1;
    }
    if (const auto validity = ValidityString(handle.get())) {
      valid_times.insert(*validity);
      message["valid_time"] = *validity;
    }
    if (const auto level_type = GetString(handle.get(), "typeOfLevel"))
      message["level_type"] = *level_type;
    if (const auto level = GetLong(handle.get(), "level"))
      message["level"] = Json::Int64(*level);

    auto first_lon =
        GetDouble(handle.get(), "longitudeOfFirstGridPointInDegrees");
    auto first_lat =
        GetDouble(handle.get(), "latitudeOfFirstGridPointInDegrees");
    auto last_lon =
        GetDouble(handle.get(), "longitudeOfLastGridPointInDegrees");
    auto last_lat = GetDouble(handle.get(), "latitudeOfLastGridPointInDegrees");
    if (first_lon && first_lat && last_lon && last_lat) {
      const double west = std::min(NormalizeLongitude(*first_lon),
                                   NormalizeLongitude(*last_lon));
      const double east = std::max(NormalizeLongitude(*first_lon),
                                   NormalizeLongitude(*last_lon));
      const double south = std::min(*first_lat, *last_lat);
      const double north = std::max(*first_lat, *last_lat);
      Json::Value grid(Json::objectValue);
      grid["west"] = west;
      grid["south"] = south;
      grid["east"] = east;
      grid["north"] = north;
      if (const auto ni = GetLong(handle.get(), "Ni"))
        grid["ni"] = Json::Int64(*ni);
      if (const auto nj = GetLong(handle.get(), "Nj"))
        grid["nj"] = Json::Int64(*nj);
      if (const auto increment =
              GetDouble(handle.get(), "iDirectionIncrementInDegrees"))
        grid["longitude_increment"] = *increment;
      if (const auto increment =
              GetDouble(handle.get(), "jDirectionIncrementInDegrees"))
        grid["latitude_increment"] = *increment;
      message["grid"] = grid;
      if (!have_coverage) {
        coverage_west = west;
        coverage_south = south;
        coverage_east = east;
        coverage_north = north;
        have_coverage = true;
      } else {
        coverage_west = std::min(coverage_west, west);
        coverage_south = std::min(coverage_south, south);
        coverage_east = std::max(coverage_east, east);
        coverage_north = std::max(coverage_north, north);
      }
    }

    if (const auto values = GetValues(handle.get())) {
      Json::Value statistics(Json::objectValue);
      statistics["count"] = Json::UInt64(values->size());
      const auto missing_value = GetDouble(handle.get(), "missingValue");
      std::size_t missing_count = 0;
      double sum = 0.0;
      double minimum = std::numeric_limits<double>::infinity();
      double maximum = -std::numeric_limits<double>::infinity();
      Json::Value sample(Json::arrayValue);
      for (const double value : *values) {
        const bool missing =
            !std::isfinite(value) || (missing_value && value == *missing_value);
        if (missing) {
          ++missing_count;
          continue;
        }
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
        sum += value;
        if (sample.size() < 8) sample.append(value);
      }
      statistics["missing_count"] = Json::UInt64(missing_count);
      const std::size_t present = values->size() - missing_count;
      if (present) {
        statistics["minimum"] = minimum;
        statistics["maximum"] = maximum;
        statistics["mean"] = sum / static_cast<double>(present);
      }
      statistics["sample"] = sample;
      message["values"] = statistics;
    }
    messages.append(message);
  }
  result["parameter_counts"] = parameters;
  result["parameter_names"] = names;
  result["short_name_counts"] = short_names;
  result["grib2_parameter_counts"] = grib2;
  result["current_component_counts"] = currents;
  result["messages"] = messages;
  if (!valid_times.empty()) {
    result["first_valid_time"] = *valid_times.begin();
    result["last_valid_time"] = *valid_times.rbegin();
    Json::Value times(Json::arrayValue);
    for (const auto& value : valid_times) times.append(value);
    result["valid_times"] = times;
  }
  if (have_coverage) {
    coverage["west"] = coverage_west;
    coverage["south"] = coverage_south;
    coverage["east"] = coverage_east;
    coverage["north"] = coverage_north;
    result["coverage"] = coverage;
  }
  return result;
}

GribWriteSummary WriteGrib1Currents(const std::vector<CurrentGrid>& grids,
                                    const std::filesystem::path& output) {
  if (grids.empty())
    throw ValidationError("no current grids were provided for GRIB writing");
  for (const auto& grid : grids) grid.Validate();
  std::filesystem::create_directories(
      output.parent_path().empty() ? "." : output.parent_path());
  std::ofstream stream(output, std::ios::binary | std::ios::trunc);
  if (!stream)
    throw ValidationError("unable to create GRIB output: " + output.string());
  const TimePoint reference =
      std::chrono::floor<std::chrono::hours>(grids.front().time);
  std::size_t count = 0;
  for (const auto& current : grids) {
    const auto forecast_hours =
        std::chrono::duration_cast<std::chrono::hours>(current.time - reference)
            .count();
    if (forecast_hours < 0)
      throw ValidationError(
          "current grids must not be earlier than the GRIB reference time");
    const auto epoch = reference.time_since_epoch().count();
    std::time_t raw_time = static_cast<std::time_t>(epoch);
    const std::tm tm = UtcTime(raw_time);
    for (const auto& [parameter, source_values] :
         std::array<std::pair<long, const std::vector<double>*>, 2>{
             {{49, &current.u_mps}, {50, &current.v_mps}}}) {
      HandlePtr handle(
          codes_handle_new_from_samples(nullptr, "regular_ll_sfc_grib1"),
          &codes_handle_delete);
      if (!handle)
        throw ValidationError(
            "ecCodes could not create regular_ll_sfc_grib1 sample");
      SetLong(handle.get(), "editionNumber", 1);
      SetLong(handle.get(), "table2Version", 2);
      SetLong(handle.get(), "indicatorOfParameter", parameter);
      SetLong(handle.get(), "indicatorOfTypeOfLevel", 1);
      SetLong(handle.get(), "level", 0);
      SetLong(handle.get(), "dataDate",
              (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
      SetLong(handle.get(), "dataTime", tm.tm_hour * 100 + tm.tm_min);
      SetLong(handle.get(), "indicatorOfUnitOfTimeRange", 1);
      SetLong(handle.get(), "P1", static_cast<long>(forecast_hours));
      SetLong(handle.get(), "P2", 0);
      SetLong(handle.get(), "timeRangeIndicator", 0);
      SetLong(handle.get(), "Ni", static_cast<long>(current.grid.nx()));
      SetLong(handle.get(), "Nj", static_cast<long>(current.grid.ny()));
      SetDouble(handle.get(), "latitudeOfFirstGridPointInDegrees",
                current.grid.latitudes.front());
      SetDouble(handle.get(), "longitudeOfFirstGridPointInDegrees",
                current.grid.longitudes.front());
      SetDouble(handle.get(), "latitudeOfLastGridPointInDegrees",
                current.grid.latitudes.back());
      SetDouble(handle.get(), "longitudeOfLastGridPointInDegrees",
                current.grid.longitudes.back());
      SetDouble(handle.get(), "iDirectionIncrementInDegrees",
                current.grid.longitude_spacing_deg);
      SetDouble(handle.get(), "jDirectionIncrementInDegrees",
                current.grid.latitude_spacing_deg);
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
      Check(codes_set_double_array(handle.get(), "values", values.data(), size),
            "setting values");
      const void* message = nullptr;
      std::size_t length = 0;
      Check(codes_get_message(handle.get(), &message, &length),
            "encoding GRIB message");
      stream.write(static_cast<const char*>(message),
                   static_cast<std::streamsize>(length));
      if (!stream) throw ValidationError("writing GRIB message failed");
      ++count;
    }
  }
  stream.close();
  const auto scan = ScanGribMessages(output);
  if (scan.message_count != count)
    throw ValidationError("written GRIB message count mismatch");
  return {count, output};
}

GribWriteSummary WriteRegularLatLonGrib2(const RegularGrid& grid,
                                         TimePoint reference,
                                         const std::vector<Grib2Field>& fields,
                                         const std::filesystem::path& output) {
  if (fields.empty()) throw ValidationError("no GRIB2 fields were provided");
  if (grid.nx() < 2 || grid.ny() < 2)
    throw ValidationError("GRIB2 output requires at least a 2 x 2 grid");
  const std::time_t raw_time =
      static_cast<std::time_t>(reference.time_since_epoch().count());
  const std::tm tm = UtcTime(raw_time);
  std::filesystem::create_directories(
      output.parent_path().empty() ? "." : output.parent_path());
  std::ofstream stream(output, std::ios::binary | std::ios::trunc);
  if (!stream)
    throw ValidationError("unable to create GRIB2 output: " + output.string());
  std::size_t count = 0;
  for (const auto& field : fields) {
    if (field.values.size() != grid.size())
      throw ValidationError("GRIB2 field " + field.short_name +
                            " does not match grid shape");
    if (!field.mask.empty() && field.mask.size() != grid.size())
      throw ValidationError("GRIB2 field mask does not match grid shape");
    if (field.forecast_hour < 0)
      throw ValidationError("GRIB2 forecast hour must not be negative");
    HandlePtr handle(
        codes_handle_new_from_samples(nullptr, "regular_ll_sfc_grib2"),
        &codes_handle_delete);
    if (!handle)
      throw ValidationError(
          "ecCodes could not create regular_ll_sfc_grib2 sample");
    SetLong(handle.get(), "editionNumber", 2);
    SetLong(handle.get(), "discipline", 0);
    SetLong(handle.get(), "productDefinitionTemplateNumber", 0);
    SetLong(handle.get(), "typeOfGeneratingProcess", 2);
    SetLong(handle.get(), "generatingProcessIdentifier", 255);
    std::size_t short_name_length = field.short_name.size();
    Check(codes_set_string(handle.get(), "shortName", field.short_name.c_str(),
                           &short_name_length),
          "setting GRIB2 shortName " + field.short_name);
    SetLong(handle.get(), "dataDate",
            (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
    SetLong(handle.get(), "dataTime", tm.tm_hour * 100 + tm.tm_min);
    SetLong(handle.get(), "stepUnits", 1);
    SetLong(handle.get(), "forecastTime", field.forecast_hour);
    SetLong(handle.get(), "Ni", static_cast<long>(grid.nx()));
    SetLong(handle.get(), "Nj", static_cast<long>(grid.ny()));
    SetDouble(handle.get(), "latitudeOfFirstGridPointInDegrees",
              grid.latitudes.front());
    SetDouble(handle.get(), "longitudeOfFirstGridPointInDegrees",
              grid.longitudes.front());
    SetDouble(handle.get(), "latitudeOfLastGridPointInDegrees",
              grid.latitudes.back());
    SetDouble(handle.get(), "longitudeOfLastGridPointInDegrees",
              grid.longitudes.back());
    SetDouble(handle.get(), "iDirectionIncrementInDegrees",
              grid.longitude_spacing_deg);
    SetDouble(handle.get(), "jDirectionIncrementInDegrees",
              grid.latitude_spacing_deg);
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
    Check(codes_set_double_array(handle.get(), "values", values.data(),
                                 values.size()),
          "setting GRIB2 values");
    const void* message = nullptr;
    std::size_t length = 0;
    Check(codes_get_message(handle.get(), &message, &length),
          "encoding GRIB2 message");
    stream.write(static_cast<const char*>(message),
                 static_cast<std::streamsize>(length));
    if (!stream) throw ValidationError("writing GRIB2 message failed");
    ++count;
  }
  stream.close();
  const auto scan = ScanGribMessages(output);
  if (scan.message_count != count)
    throw ValidationError("written GRIB2 message count mismatch");
  return {count, output};
}

MergeStreamsResult MergeGribStreams(
    const std::vector<std::pair<std::string, std::filesystem::path>>& inputs,
    const std::filesystem::path& output, bool overwrite) {
  if (inputs.empty())
    throw ValidationError("at least one GRIB input is required");
  if (std::filesystem::exists(output) && std::filesystem::is_directory(output))
    throw ValidationError("output must be a file path, not a directory");
  if (std::filesystem::exists(output) && !overwrite)
    throw ValidationError("output already exists: " + output.string() +
                          "; use --overwrite to replace it");
  std::map<std::string, std::size_t> counts;
  std::size_t expected = 0;
  for (const auto& [label, path] : inputs) {
    if (!std::filesystem::exists(path))
      throw ValidationError(label + " GRIB not found: " + path.string());
    const auto scan = ScanGribMessages(path);
    counts[label] = scan.message_count;
    expected += scan.message_count;
  }
  std::filesystem::create_directories(
      output.parent_path().empty() ? "." : output.parent_path());
  const auto temporary = TemporarySibling(output);
  try {
    std::ofstream destination(temporary, std::ios::binary | std::ios::trunc);
    if (!destination)
      throw ValidationError("unable to create merged GRIB temporary file");
    for (const auto& [label, path] : inputs) {
      (void)label;
      std::ifstream source(path, std::ios::binary);
      destination << source.rdbuf();
      if (!source.eof() && source.fail())
        throw ValidationError("failed reading input GRIB: " + path.string());
      if (!destination) throw ValidationError("failed writing merged GRIB");
    }
    destination.close();
    const auto scan = ScanGribMessages(temporary);
    if (scan.message_count != expected)
      throw ValidationError("merged GRIB message count mismatch");
    Json::Value inspection = InspectGrib(temporary);
    std::error_code error;
    std::filesystem::rename(temporary, output, error);
    if (error)
      throw ValidationError("publishing merged GRIB failed: " +
                            error.message());
    return {counts, scan.message_count, scan.byte_count, inspection};
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

namespace {

bool CountPresent(const Json::Value& object, const std::string& key) {
  return object.isObject() && object.get(key, 0).asUInt64() > 0;
}

bool HasWindPair(const Json::Value& inspection) {
  const auto& short_names = inspection["short_name_counts"];
  const auto& parameters = inspection["parameter_counts"];
  const bool u = CountPresent(short_names, "10u") ||
                 CountPresent(short_names, "u10") ||
                 CountPresent(parameters, "33");
  const bool v = CountPresent(short_names, "10v") ||
                 CountPresent(short_names, "v10") ||
                 CountPresent(parameters, "34");
  return u && v;
}

bool HasCurrentPair(const Json::Value& inspection) {
  const auto& currents = inspection["current_component_counts"];
  return (currents.get("u_49", 0).asUInt64() > 0 &&
          currents.get("v_50", 0).asUInt64() > 0) ||
         (currents.get("u_grib2", 0).asUInt64() > 0 &&
          currents.get("v_grib2", 0).asUInt64() > 0);
}

bool HasWaveField(const Json::Value& inspection) {
  const auto& names = inspection["short_name_counts"];
  for (const char* name : {"swh", "htsgw", "perpw", "dirpw", "mwp", "mwd",
                           "shww", "mpww", "mdww"}) {
    if (CountPresent(names, name)) return true;
  }
  return false;
}

bool RangesOverlap(double first_minimum, double first_maximum,
                   double second_minimum, double second_maximum) {
  return std::max(first_minimum, second_minimum) <=
         std::min(first_maximum, second_maximum);
}

bool CoverageOverlaps(const Json::Value& first, const Json::Value& second) {
  if (!first.isObject() || !second.isObject()) return false;
  return RangesOverlap(first["west"].asDouble(), first["east"].asDouble(),
                       second["west"].asDouble(), second["east"].asDouble()) &&
         RangesOverlap(first["south"].asDouble(), first["north"].asDouble(),
                       second["south"].asDouble(), second["north"].asDouble());
}

bool TimeRangesOverlap(const Json::Value& first, const Json::Value& second) {
  if (!first.isMember("first_valid_time") ||
      !first.isMember("last_valid_time") ||
      !second.isMember("first_valid_time") ||
      !second.isMember("last_valid_time"))
    return false;
  return std::max(first["first_valid_time"].asString(),
                  second["first_valid_time"].asString()) <=
         std::min(first["last_valid_time"].asString(),
                  second["last_valid_time"].asString());
}

void AppendUnique(std::vector<std::string>& destination,
                  const std::string& value) {
  if (std::find(destination.begin(), destination.end(), value) ==
      destination.end())
    destination.push_back(value);
}

}  // namespace

EnvironmentalMergeResult MergeEnvironmentalGribs(
    const EnvironmentalMergeRequest& request) {
  EnvironmentalMergeResult result;
  result.output = request.output;
  if (request.output.empty()) {
    result.errors.emplace_back("an output GRIB path is required");
    return result;
  }

  const std::vector<
      std::pair<std::string, std::optional<std::filesystem::path>>>
      requested{{"weather", request.weather},
                {"current", request.current},
                {"waves", request.waves}};
  std::vector<std::pair<std::string, std::filesystem::path>> inputs;
  std::map<std::string, Json::Value> inspections;
  for (const auto& [role, path] : requested) {
    if (!path) continue;
    Json::Value validation(Json::objectValue);
    validation["path"] = path->string();
    validation["valid"] = false;
    try {
      const auto inspection = InspectGrib(*path);
      bool role_valid = role == "weather"   ? HasWindPair(inspection)
                        : role == "current" ? HasCurrentPair(inspection)
                                            : HasWaveField(inspection);
      validation["role_valid"] = role_valid;
      validation["inspection"] = inspection;
      if (!role_valid) {
        const std::string error =
            role + " input does not contain a recognized paired " +
            (role == "weather"   ? "U/V wind field"
             : role == "current" ? "U/V current field"
                                 : "wave field");
        validation["error"] = error;
        result.errors.push_back(error + ": " + path->string());
      } else {
        validation["valid"] = true;
        inputs.emplace_back(role, *path);
        inspections.emplace(role, inspection);
        result.input_message_counts[role] =
            inspection["message_count"].asUInt64();
        for (const auto& field : ObjectKeys(inspection["short_name_counts"]))
          AppendUnique(result.fields_discovered, field);
        for (const auto& time : inspection["valid_times"])
          AppendUnique(result.time_records_discovered, time.asString());
      }
    } catch (const std::exception& error) {
      validation["error"] = error.what();
      result.errors.push_back(role + " input is invalid: " + error.what());
    }
    result.input_validation[role] = validation;
  }

  if (!request.weather && !request.current && !request.waves)
    result.errors.emplace_back(
        "at least one weather, current, or wave input is required");

  if (inspections.count("weather") && inspections.count("current")) {
    const auto& weather = inspections.at("weather");
    const auto& current = inspections.at("current");
    if (!CoverageOverlaps(weather["coverage"], current["coverage"]))
      result.errors.emplace_back(
          "weather and current GRIB geographic coverage does not overlap");
    if (!TimeRangesOverlap(weather, current))
      result.errors.emplace_back(
          "weather and current GRIB valid-time ranges do not overlap");
    const auto& weather_times = weather["valid_times"];
    const auto& current_times = current["valid_times"];
    if (weather_times != current_times)
      result.warnings.emplace_back(
          "weather and current time records differ; their compatible union "
          "will be retained");
  }
  if (!result.errors.empty()) return result;

  try {
    const auto merged =
        MergeGribStreams(inputs, request.output, request.overwrite);
    result.input_message_counts = merged.input_message_counts;
    result.output_message_count = merged.output_message_count;
    result.byte_count = merged.byte_count;
    result.output_inspection = InspectGrib(request.output);
    result.fields_written =
        ObjectKeys(result.output_inspection["short_name_counts"]);
    result.success = true;
  } catch (const std::exception& error) {
    result.errors.push_back(error.what());
  }
  return result;
}

Json::Value EnvironmentalMergeResultJson(
    const EnvironmentalMergeResult& result) {
  Json::Value value(Json::objectValue);
  value["success"] = result.success;
  value["output"] = result.output.string();
  value["output_message_count"] = Json::UInt64(result.output_message_count);
  value["byte_count"] = Json::UInt64(result.byte_count);
  for (const auto& [label, count] : result.input_message_counts)
    value["input_message_counts"][label] = Json::UInt64(count);
  value["input_validation"] = result.input_validation;
  value["output_inspection"] = result.output_inspection;
  AddStringArray(value["fields_discovered"], result.fields_discovered);
  AddStringArray(value["time_records_discovered"],
                 result.time_records_discovered);
  AddStringArray(value["fields_written"], result.fields_written);
  AddStringArray(value["warnings"], result.warnings);
  AddStringArray(value["errors"], result.errors);
  return value;
}

MergeStreamsResult CompositeGribStreamsPreferFirst(
    const std::vector<std::pair<std::string, std::filesystem::path>>& inputs,
    const std::filesystem::path& output, bool overwrite,
    std::optional<TimePoint> valid_from,
    std::optional<TimePoint> valid_through) {
  if (inputs.empty())
    throw ValidationError("at least one GRIB input is required");
  if (std::filesystem::exists(output) && std::filesystem::is_directory(output))
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
        throw ValidationError("unable to open " + label +
                              " GRIB: " + path.string());
      struct FileCloser {
        void operator()(FILE* value) const { std::fclose(value); }
      };
      std::unique_ptr<FILE, FileCloser> file(raw);
      std::size_t source_index = 0;
      while (true) {
        int error = 0;
        HandlePtr handle(codes_handle_new_from_file(nullptr, file.get(),
                                                    PRODUCT_GRIB, &error),
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
        const auto level_type =
            GetString(handle.get(), "typeOfLevel").value_or("unknown-level");
        const auto level = GetLong(handle.get(), "level");
        const auto date = GetLong(handle.get(), "validityDate");
        const auto time = GetLong(handle.get(), "validityTime");
        std::string key;
        if (short_name && date && time) {
          key = *short_name + '|' + level_type + '|' +
                std::to_string(level.value_or(0)) + '|' +
                std::to_string(*date) + '|' + std::to_string(*time);
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
