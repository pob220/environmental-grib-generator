#include "environmental_grib/gfs_fallback.h"
#include "environmental_grib/grib.h"
#include "environmental_grib/cancellation.h"
#include "environmental_grib/platform.h"
#include <sodium.h>
#include <atomic>
#include <array>
#include <fstream>
#include <iostream>
#include <sstream>

namespace eg = environmental_grib;
namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template <class Function>
void Fails(Function function, const char* message) {
  try {
    function();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error(message);
}
std::vector<unsigned char> Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}
eg::HostedHttpReply JsonReply(const Json::Value& value, long status = 200) {
  Json::StreamWriterBuilder builder;
  const auto text = Json::writeString(builder, value);
  return {status, {text.begin(), text.end()}, 0};
}
Json::Value Parse(const std::string& text) {
  Json::CharReaderBuilder builder;
  Json::Value value;
  std::string errors;
  std::istringstream stream(text);
  Check(Json::parseFromStream(builder, stream, &value, &errors),
        "invalid test JSON");
  return value;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3) {
      const std::string cycle = argv[1];
      Check(cycle.size() == 10, "live cycle must be YYYYMMDDHH");
      eg::GFSRequest request{
          {-8.5, 50.5, -2.5, 56.5}, eg::PathFromUtf8(argv[2]), 6};
      request.date = cycle.substr(0, 8);
      request.cycle = cycle.substr(8);
      request.waves = true;
      request.overwrite = true;
      int noaa_calls = 0;
      const auto result = eg::GenerateGfs(
          request,
          [&](const std::string&, double) -> std::vector<unsigned char> {
            ++noaa_calls;
            throw eg::HttpDownloadError(
                "Controlled NOAA connection failure for live failover "
                "validation",
                true);
          },
          {},
          [](const std::string& stage, const Json::Value&) {
            std::cout << stage << '\n';
          },
          {}, eg::BuiltinHostedGfsDownload());
      Check(noaa_calls == 1 && result.message_count == 9,
            "live failover did not preserve all fields/times");
      std::cout << "PASS live hosted failover: " << result.byte_count
                << " bytes, " << result.message_count << " messages\n";
      return 0;
    }
    const auto root = std::filesystem::temp_directory_path() /
                      ("gfs-fallback-tests-" + std::to_string(eg::ProcessId()));
    std::filesystem::create_directories(root);
    struct Cleanup {
      std::filesystem::path root;
      ~Cleanup() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
      }
    } cleanup{root};
    const eg::GFSCycle cycle{"20261004", "12"};
    eg::GFSRequest request{{-1, 51, 0, 52}, root / "forecast.grib2", 6};
    request.cycle = cycle.cycle;
    request.date = cycle.date;
    request.preset = "minimal";
    request.overwrite = true;
    auto fixture = [&](const std::vector<int>& hours,
                       eg::GFSCycle reference = eg::GFSCycle{"20261004", "12"},
                       double west = -1) {
      const auto grid = eg::BuildRegularGrid({west, 51, west + .75, 52}, .25);
      std::vector<eg::Grib2Field> fields;
      for (int hour : hours)
        for (const auto& name : {"10u", "10v"})
          fields.push_back(
              {hour,
               name,
               std::vector<double>(grid.size(),
                                   name == std::string("10u") ? 4.25 : -2.5),
               {}});
      const auto file = root / "fixture.grib2";
      const auto date = reference.date;
      eg::WriteRegularLatLonGrib2(
          grid,
          eg::ParseUtcDateTime(date.substr(0, 4) + "-" + date.substr(4, 2) +
                               "-" + date.substr(6, 2) + "T" + reference.cycle +
                               ":00:00Z"),
          fields, file);
      return Read(file);
    };
    const auto all = fixture({0, 3, 6});
    const auto parts =
        eg::ValidateHostedGfsBytes(all, request, cycle, {0, 3, 6});
    Check(parts.size() == 3, "native hosted validation lost timesteps");
    Fails(
        [&] {
          eg::ValidateHostedGfsBytes(fixture({0, 3, 6}, {"20261004", "06"}),
                                     request, cycle, {0, 3, 6});
        },
        "changed cycle accepted");
    Fails(
        [&] {
          eg::ValidateHostedGfsBytes(fixture({0, 3, 6}, cycle, -2), request,
                                     cycle, {0, 3, 6});
        },
        "changed region accepted");
    Fails(
        [&] {
          eg::ValidateHostedGfsBytes(fixture({0}), request, cycle, {0, 3, 6});
        },
        "missing times accepted");
    auto corrupt = all;
    corrupt.pop_back();
    Fails(
        [&] { eg::ValidateHostedGfsBytes(corrupt, request, cycle, {0, 3, 6}); },
        "truncated result accepted");
    for (long status : {302, 403, 408, 425, 429, 500, 502, 503, 504, -60})
      Check(eg::IsGfsFailoverError(
                eg::HttpDownloadError("outage", false, status)),
            "NOAA outage not classified");
    Check(!eg::IsGfsFailoverError(
              eg::HttpDownloadError("missing", true, 503, true)),
          "publishing file treated as outage");
    Check(!eg::IsGfsFailoverError(
              eg::HttpDownloadError("bad request", false, 400)),
          "invalid request treated as outage");

    int hosted_calls = 0;
    const auto normal = eg::GenerateGfs(
        request,
        [&](const std::string& url, double) {
          for (int hour : {0, 3, 6})
            if (url.find("f00" + std::to_string(hour)) != std::string::npos)
              return parts.at(hour);
          throw std::runtime_error("unexpected NOAA URL");
        },
        {}, {}, {},
        [&](const auto&, const auto&, const auto&, auto) {
          ++hosted_calls;
          return all;
        });
    Check(hosted_calls == 0 && normal.message_count == 6,
          "healthy NOAA used fallback");
    const auto partial = eg::GenerateGfs(
        request,
        [&](const std::string& url, double) {
          if (url.find("f006") != std::string::npos) return parts.at(6);
          throw eg::HttpDownloadError("Over Rate Limit", true, 429);
        },
        {}, {}, {},
        [&](const auto&, const auto& c, const std::vector<int>& hours, auto) {
          ++hosted_calls;
          Check(c.date == cycle.date && c.cycle == cycle.cycle &&
                    hours == std::vector<int>({0, 3}),
                "successful NOAA timestep downloaded again");
          std::vector<unsigned char> data = parts.at(0);
          data.insert(data.end(), parts.at(3).begin(), parts.at(3).end());
          return data;
        });
    Check(hosted_calls == 1 && partial.message_count == 6 &&
              partial.source.find("hosted") != std::string::npos,
          "partial failover failed");

    auto paired = request;
    paired.noaa_unavailable = std::make_shared<std::atomic<bool>>(false);
    int weather_noaa = 0, wave_noaa = 0;
    eg::GenerateGfs(
        paired,
        [&](const std::string&, double) -> std::vector<unsigned char> {
          ++weather_noaa;
          throw eg::HttpDownloadError("NOAA offline", true);
        },
        {}, {}, {},
        [&](const auto&, const auto&, const auto&, auto) { return all; });
    auto waves = paired;
    waves.waves = true;
    waves.output = root / "waves.grib2";
    const auto grid = eg::BuildRegularGrid({-1, 51, -.25, 52}, .25);
    std::vector<eg::Grib2Field> wave_fields;
    for (int hour : {0, 3, 6})
      for (const auto& field : {"swh", "perpw", "dirpw"})
        wave_fields.push_back(
            {hour, field, std::vector<double>(grid.size(), 1.0), {}});
    eg::WriteRegularLatLonGrib2(grid,
                                eg::ParseUtcDateTime("2026-10-04T12:00:00Z"),
                                wave_fields, root / "wave-fixture.grib2");
    const auto wave_bytes = Read(root / "wave-fixture.grib2");
    const auto wave_result = eg::GenerateGfs(
        waves,
        [&](const std::string&, double) -> std::vector<unsigned char> {
          ++wave_noaa;
          throw eg::HttpDownloadError("NOAA offline", true);
        },
        {}, {}, {},
        [&](const auto&, const auto&, const auto&, auto) {
          return wave_bytes;
        });
    Check(weather_noaa == 1 && wave_noaa == 0 && wave_result.message_count == 9,
          "paired components repeated NOAA requests after outage");

    int attempts = 0;
    auto automatic = request;
    automatic.cycle = "auto";
    automatic.date = std::nullopt;
    automatic.max_auto_cycles = 3;
    int missing_noaa = 0, missing_host = 0;
    auto publishing = automatic;
    publishing.max_auto_cycles = 2;
    const auto older = eg::GenerateGfs(
        publishing,
        [&](const std::string& url, double) {
          ++missing_noaa;
          if (url.find("gfs.20261004%2F12") != std::string::npos)
            throw eg::HttpDownloadError("file not published", false, 404, true);
          return parts.at(6);
        },
        eg::ParseUtcDateTime("2026-10-04T13:00:00Z"), {}, {},
        [&](const auto&, const auto&, const auto&, auto) {
          ++missing_host;
          return all;
        });
    Check(missing_host == 0 && missing_noaa == 4 && older.cycle.cycle == "06",
          "publishing forecast did not retain normal cycle selection");
    // Restore the full known-good output before verifying atomic failure.
    {
      std::ofstream file(request.output, std::ios::binary);
      file.write(reinterpret_cast<const char*>(all.data()), all.size());
    }
    try {
      eg::GenerateGfs(
          automatic,
          [&](const std::string&, double) -> std::vector<unsigned char> {
            ++attempts;
            throw eg::HttpDownloadError("NOAA timeout", true);
          },
          eg::ParseUtcDateTime("2026-10-04T13:00:00Z"), {}, {},
          [&](const auto&, const auto&, const auto&,
              auto) -> std::vector<unsigned char> {
            throw eg::HostedGfsServiceError("service unavailable");
          });
      throw std::runtime_error("unavailable service unexpectedly succeeded");
    } catch (const eg::HostedGfsServiceError&) {
    }
    Check(attempts == 1, "service outage retried every older cycle");
    Check(Read(request.output) == all,
          "failed generation changed existing output");

    int noaa_attempts = 0, other_attempts = 0;
    auto noaa = eg::MakeRetryingHttpGet(
        [&](const std::string&, double) -> std::vector<unsigned char> {
          ++noaa_attempts;
          throw eg::HttpDownloadError("limited", true, 429);
        },
        "NOAA GFS weather", {}, {3, 1, 2}, [](int) {});
    Fails([&] { noaa("https://nomads.ncep.noaa.gov/test", 1); },
          "failed NOAA request unexpectedly succeeded");
    auto other = eg::MakeRetryingHttpGet(
        [&](const std::string&, double) -> std::vector<unsigned char> {
          ++other_attempts;
          throw eg::HttpDownloadError("limited", true, 429);
        },
        "Other provider", {}, {3, 1, 2}, [](int) {});
    Fails([&] { other("https://example.test/test", 1); },
          "failed provider unexpectedly succeeded");
    Check(noaa_attempts == 1 && other_attempts == 3,
          "provider retry isolation changed");

    std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
    Check(sodium_init() >= 0, "checksum initialization failed");
    crypto_hash_sha256(hash.data(), all.data(), all.size());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
    Json::Value expected;
    const std::string status_path = "/v1/jobs/" + std::string(32, 'a');
    const std::string result_path =
        "/v1/results/" + std::string(64, 'b') + ".grib2";
    int submitted = 0, waited = 0;
    auto http = [&](const std::string& url, const std::string& method,
                    const std::string& body, const std::string& client,
                    double) {
      Check(client.size() == 32 && url.starts_with(eg::kGfsFallbackOrigin),
            "invalid hosted client/origin");
      Json::Value value;
      if (url.ends_with("/v1/capabilities")) {
        for (int hour : {0, 3, 6})
          value["coverage"]["2026100412"]["weather"].append(hour);
        return JsonReply(value);
      }
      if (method == "POST") {
        expected = Parse(body);
        if (++submitted == 1) return eg::HostedHttpReply{429, {}, 7};
        value["status_url"] = status_path;
        value["state"] = "queued";
        return JsonReply(value, 202);
      }
      if (url.ends_with(status_path)) {
        value["state"] = "complete";
        value["result"]["request"] = expected;
        value["result"]["result_url"] = result_path;
        value["result"]["sha256"] = hex.data();
        value["result"]["bytes"] = Json::UInt64(all.size());
        return JsonReply(value);
      }
      Check(url.ends_with(result_path), "unexpected hosted download path");
      return eg::HostedHttpReply{200, all, 0};
    };
    Check(eg::DownloadHostedGfs(request, cycle, {0, 3, 6}, {}, http,
                                [&](int ms) { waited += ms; }) == all &&
              submitted == 2 && waited == 8000,
          "Retry-After or complete hosted protocol failed");
    auto wrong = [&](const auto& url, const auto& method, const auto& body,
                     const auto& client, double timeout) {
      auto result = http(url, method, body, client, timeout);
      if (url.ends_with(status_path)) {
        auto value = Parse(std::string(result.body.begin(), result.body.end()));
        value["result"]["sha256"] = std::string(64, '0');
        return JsonReply(value);
      }
      return result;
    };
    Fails(
        [&] {
          eg::DownloadHostedGfs(request, cycle, {0, 3, 6}, {}, wrong,
                                [](int) {});
        },
        "invalid checksum accepted");
    auto redirect = [&](const auto& url, const auto& method, const auto& body,
                        const auto& client, double timeout) {
      auto result = http(url, method, body, client, timeout);
      if (method == "POST") {
        Json::Value value;
        value["state"] = "queued";
        value["status_url"] = "https://other.example/job";
        return JsonReply(value, 202);
      }
      return result;
    };
    Fails(
        [&] {
          eg::DownloadHostedGfs(request, cycle, {0, 3, 6}, {}, redirect,
                                [](int) {});
        },
        "cross-origin status URL accepted");
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    {
      eg::ExecutionScope scope({cancel, {}});
      Fails(
          [&] {
            eg::DownloadHostedGfs(request, cycle, {0, 3, 6}, {}, http,
                                  [&](int) { cancel->store(true); });
          },
          "cancelled hosted wait did not stop");
    }
    std::cout << "PASS: GFS failure classification, provider isolation, "
                 "retained timesteps, exact forecast/grid, checksum, "
                 "Retry-After, service outage and cancellation\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
