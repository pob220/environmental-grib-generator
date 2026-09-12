#include "environmental_grib/estimate.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <set>

#include "environmental_grib/error.h"
#include "environmental_grib/platform.h"
#include "environmental_grib/ukv.h"

namespace environmental_grib {
namespace {
using Count = std::uint64_t;
Count Multiply(Count a, Count b) {
  if (b && a > std::numeric_limits<Count>::max() / b)
    throw ValidationError("estimate exceeds supported size");
  return a * b;
}
Count Add(Count a, Count b) {
  if (a > std::numeric_limits<Count>::max() - b)
    throw ValidationError("estimate exceeds supported size");
  return a + b;
}
}

Json::Value EstimateEnvironment(const EnvironmentRequest& r) {
  r.bbox.Validate();
  // Bound work before calling the shared vector-producing time planners.
  if (r.hours < 0 || r.hours > 24 * 366 || r.step_hours <= 0 ||
      r.hours % r.step_hours)
    throw ValidationError("estimate requires 0..8784 hours divisible by stepHours");
  Json::Value result(Json::objectValue);
  result["schemaVersion"] = 1;
  result["components"] = Json::Value(Json::arrayValue);
  result["notes"] = Json::Value(Json::arrayValue);
  result["notes"].append("Decoded size counts double-precision numeric arrays only; generation, decoding and routing require additional memory.");
  Count decoded = 0, packed_upper = 0, records = 0;
  bool complete = true, file_complete = true, approximate = false;
  auto unknown = [&](const std::string& component, const std::string& reason) {
    Json::Value item(Json::objectValue);
    item["component"] = component;
    item["status"] = "unknown";
    item["reason"] = reason;
    result["components"].append(item);
    complete = file_complete = false;
  };
  auto known = [&](const std::string& component, Count cells, Count fields,
                   int packed_bytes, bool approx = false) {
    const Count values = Multiply(cells, fields);
    const Count bytes = Multiply(values, sizeof(double));
    Json::Value item(Json::objectValue);
    item["component"] = component;
    item["status"] = approx ? "approximate" : "planned";
    item["records"] = Json::UInt64(fields);
    item["cellsPerRecord"] = Json::UInt64(cells);
    item["decodedBytes"] = Json::UInt64(bytes);
    // Our simple-packed writers use 16/24 bits. Allow a full bitmap and
    // 512 bytes of framing per record. Constant/masked fields can be much
    // smaller, so this is an upper planning estimate, not expected disk size.
    if (packed_bytes) {
      const Count per_record = Add(Add(Multiply(cells, packed_bytes),
                                      Add(cells, 7) / 8), 512);
      const Count upper = Multiply(per_record, fields);
      item["fileUpperBytes"] = Json::UInt64(upper);
      packed_upper = Add(packed_upper, upper);
    } else {
      file_complete = false;
      item["reason"] = "Downloaded packing is not known before download.";
    }
    decoded = Add(decoded, bytes);
    records = Add(records, fields);
    approximate |= approx;
    result["components"].append(item);
  };
  auto cells = [&](double spacing) -> Count {
    const auto [nx, ny] = RegularGridDimensions(r.bbox, spacing);
    return Multiply(nx, ny);
  };
  // Extension may fetch older cycles, whole fallback streams and crop their
  // overlap. Do not guess a total until the shared merge plan is exposed.
  if (r.extend_forecast) {
    unknown("forecast extension", "Source cycles and fallback overlap are not yet known.");
  } else {
    if (r.weather_provider == "ukmo_ukv") {
      if (!kUkvDomain.Contains(r.bbox))
        throw ValidationError("area is outside UKV coverage");
      Count fields = 0;
      for (int hour : UkvForecastHours(r.hours, r.step_hours))
        fields = Add(fields, UkvOutputFieldCount(r.weather_preset, hour));
      known("UKV weather", cells(r.weather_grid_spacing_deg), fields, 3);
    } else if (r.weather_provider == "gfs" &&
               (r.weather_preset == "minimal" || r.weather_preset == "routing")) {
      if (!std::set<int>{1, 3, 6, 12}.contains(r.step_hours) || r.hours > 120)
        unknown("GFS weather", "Long-lead source cadence requires an inventory.");
      else {
        Count fields = 0;
        for (const auto& [key, value] : GfsVariablesForPreset(r.weather_preset))
          if (key.starts_with("var_")) ++fields;
        // NOMADS' quarter-degree subset may align outward at its edges.
        const Count nx = static_cast<Count>(std::ceil((r.bbox.east-r.bbox.west)/0.25)) + 2;
        const Count ny = static_cast<Count>(std::ceil((r.bbox.north-r.bbox.south)/0.25)) + 2;
        known("GFS weather", Multiply(nx, ny),
              Multiply(fields, ForecastHourSequence(r.hours, r.step_hours).size()), 0, true);
      }
    } else if (r.weather_provider != "none") {
      unknown("weather", "Native grid or selected field/level inventory is needed.");
    }
    if (r.include_waves) {
      if (r.wave_provider == "copernicus_global_waves") {
        if (r.wave_step_hours != 3 || r.hours > 240 || r.hours % 3)
          throw ValidationError("Copernicus waves require 3-hour steps and 0..240 hours divisible by 3");
        // GenerateEnvironment passes weather_grid_spacing_deg to the wave
        // writer too; do not substitute the provider's native resolution.
        known("Copernicus waves", cells(r.weather_grid_spacing_deg),
              Multiply(3, BuildTimeSequence(r.start, r.hours, r.wave_step_hours).size()), 3);
      } else {
        unknown("waves", "Wave source grid and record inventory are needed.");
      }
    }
    if (std::set<std::string>{"synthetic", "netcdf", "tpxo", "offline-tidal",
                             "copernicus_nws", "copernicus_global",
                             "copernicus_ibi", "copernicus_mediterranean"}
            .contains(r.current_source)) {
      known("currents", cells(r.current_grid_spacing_deg),
            Multiply(2, BuildTimeSequence(r.start, r.hours, r.step_hours).size()), 2);
    } else if (r.current_source != "none") {
      unknown("currents", "Source/cache grid and time coverage are needed.");
    }
  }
  result["complete"] = complete;
  result["approximate"] = approximate;
  result["knownRecords"] = Json::UInt64(records);
  result["knownDecodedBytes"] = Json::UInt64(decoded);
  if (complete) result["decodedBytes"] = Json::UInt64(decoded);
  if (file_complete) result["fileUpperBytes"] = Json::UInt64(packed_upper);
  return result;
}

Json::Value BuildSizeComparison(const EnvironmentRequest& r,
                               const Json::Value& estimate,
                               const EnvironmentResult& generated) {
  Json::Value report(Json::objectValue);
  report["schemaVersion"] = 1;
  report["requestBasis"] = "Captured before generation; credentials and local source paths omitted.";
  report["outputFile"] = PathToUtf8(generated.output.filename());
  auto& request = report["request"];
  request["bbox"]["west"] = r.bbox.west;
  request["bbox"]["south"] = r.bbox.south;
  request["bbox"]["east"] = r.bbox.east;
  request["bbox"]["north"] = r.bbox.north;
  request["start"] = FormatUtcDateTime(r.start);
  request["hours"] = r.hours;
  request["stepHours"] = r.step_hours;
  request["cycle"] = r.cycle;
  if (r.date) request["date"] = *r.date;
  request["weatherProvider"] = r.weather_provider;
  request["weatherPreset"] = r.weather_preset;
  request["weatherGridSpacingDeg"] = r.weather_grid_spacing_deg;
  request["includeWaves"] = r.include_waves;
  request["waveProvider"] = r.wave_provider;
  request["waveStepHours"] = r.wave_step_hours;
  request["currentSource"] = r.current_source;
  request["currentGridSpacingDeg"] = r.current_grid_spacing_deg;
  request["extendForecast"] = r.extend_forecast;
  request["fallbackWeatherProvider"] = r.fallback_weather_provider;
  request["fallbackWaveProvider"] = r.fallback_wave_provider;
  request["fallbackCurrentSource"] = r.fallback_current_source;
  report["estimate"] = estimate;
  auto& actual = report["actual"];
  actual["fileBytes"] = Json::UInt64(generated.byte_count);
  actual["records"] = Json::UInt64(generated.message_count);
  const auto& messages = generated.inspection["messages"];
  bool complete = messages.isArray() && generated.message_count > 0 &&
                  messages.size() == generated.message_count;
  Count numeric = 0;
  // Reuse the existing final validation inventory: no additional file scan,
  // array decoding or forecast loading solely for the size report.
  if (messages.isArray()) {
    for (const auto& message : messages) {
      const auto& count = message["values"]["count"];
      if (!count.isUInt64() || count.asUInt64() == 0) {
        complete = false;
        continue;
      }
      numeric = Add(numeric, Multiply(count.asUInt64(), sizeof(double)));
    }
  }
  actual["numericComplete"] = complete;
  actual["knownDecodedBytes"] = Json::UInt64(numeric);
  if (complete) actual["decodedBytes"] = Json::UInt64(numeric);
  report["numericStatus"] = "unknown";
  if (complete && estimate["decodedBytes"].isUInt64()) {
    const auto predicted = estimate["decodedBytes"].asUInt64();
    report["numericStatus"] = predicted == numeric ? "exact" :
        predicted > numeric ? "overestimate" : "underestimate";
  }
  report["fileStatus"] = "unknown";
  if (estimate["fileUpperBytes"].isUInt64())
    report["fileStatus"] = generated.byte_count <= estimate["fileUpperBytes"].asUInt64()
        ? "within_upper_estimate" : "exceeds_upper_estimate";
  report["note"] = "Numeric storage includes missing cells, each field, level and time at eight bytes per cell; not total application RAM. Contains requested area/time; review before sharing.";
  return report;
}
}  // namespace environmental_grib
