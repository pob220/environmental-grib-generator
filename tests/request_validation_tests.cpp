#include "environmental_grib/area_validation.h"
#include "environmental_grib/copernicus_auth.h"
#include "environmental_grib/copernicus.h"
#include "environmental_grib/geo.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace eg = environmental_grib;
namespace {
int checks = 0;
void Check(bool value, const char* description) {
  ++checks;
  if (!value) throw std::runtime_error(description);
}
void Invalid(eg::BoundingBox bbox, eg::AreaCoordinate field) {
  const auto issue = eg::ValidateDownloadArea(bbox.west,bbox.south,bbox.east,bbox.north);
  Check(issue.message && issue.field == field, "invalid area identifies the coordinate to edit");
  try { bbox.Validate(); }
  catch (const eg::ValidationError& error) {
    Check(error.what() == std::string(issue.message), "GUI and engine use the same area rule/message");
    return;
  }
  throw std::runtime_error("engine accepted invalid area");
}
void Auth(long status, const std::string& body, const char* expected) {
  const auto message = eg::CopernicusAuthenticationFailure(status,body);
  Check(message.find(expected) != std::string::npos,"authentication error has useful classification");
  Check(message.find("SECRET") == std::string::npos,"authentication response cannot leak secrets");
  Check(message.find("HTTP " + std::to_string(status)) != std::string::npos,"authentication status retained");
}
}
int main() {
  try {
    using F=eg::AreaCoordinate;
    for (const auto bbox : {eg::BoundingBox{-8.5,50.5,-2.5,56.5},
                           eg::BoundingBox{170,-10,-170,10},
                           eg::BoundingBox{-180,-90,180,90},
                           eg::BoundingBox{0,0,1,1}}) {
      Check(!eg::ValidateDownloadArea(bbox.west,bbox.south,bbox.east,bbox.north).message,"valid custom/date-line/global/zero-endpoint area accepted");
      bbox.Validate();
    }
    Invalid({0,0,0,1},F::East);
    Invalid({180,0,-180,1},F::East);
    Invalid({0,1,1,1},F::North);
    Invalid({0,2,1,1},F::North);
    Invalid({-181,0,1,1},F::West);
    Invalid({0,0,181,1},F::East);
    Invalid({0,-91,1,1},F::South);
    Invalid({0,0,1,91},F::North);
    Invalid({std::numeric_limits<double>::quiet_NaN(),0,1,1},F::West);
    Invalid({0,std::numeric_limits<double>::infinity(),1,1},F::South);
    Invalid({0,0,std::numeric_limits<double>::infinity(),1},F::East);
    Invalid({0,0,1,std::numeric_limits<double>::quiet_NaN()},F::North);
    // Invalid requests must fail before credentials or downloads are consulted.
    for (const char* provider : {"copernicus_nws","copernicus_global","copernicus_ibi","copernicus_mediterranean"}) {
      eg::CopernicusRequest request; request.provider=provider; request.bbox={0,0,0,1};
      int calls=0;
      eg::CredentialValidator validator=[&](const std::string&,const std::string&,double){++calls;return true;};
      try {
        if (request.provider=="copernicus_nws") eg::GenerateCopernicusNws(request,{},validator);
        else if(request.provider=="copernicus_global") eg::GenerateCopernicusGlobal(request,{},validator);
        else if(request.provider=="copernicus_ibi") eg::GenerateCopernicusIbi(request,{},validator);
        else eg::GenerateCopernicusMediterranean(request,{},validator);
        throw std::runtime_error("invalid area reached provider");
      } catch(const eg::ValidationError& error) {
        Check(std::string(error.what()).find("non-zero width")!=std::string::npos && calls==0,"invalid area rejected before authentication for every current region");
      }
    }
    Auth(400,R"({"error":"invalid_grant","error_description":"Invalid user credentials","access_token":"SECRET"})","rejected the username/email or password");
    Auth(400,R"({"error":"invalid_grant","error_description":"Account is not fully set up SECRET"})","account needs attention");
    Auth(400,R"({"error":"invalid_grant","error_description":"Account temporarily disabled SECRET"})","temporarily locked");
    Auth(400,R"({"error":"invalid_grant","error_description":"SECRET unknown detail"})","did not accept this account login");
    for (const char* code : {"invalid_request","invalid_client","unauthorized_client","unsupported_grant_type","invalid_scope"})
      Auth(400,std::string("{\"error\":\"")+code+"\",\"error_description\":\"SECRET\"}","rejected xGRIB's sign-in request");
    Auth(401,"", "refused this sign-in");
    Auth(403,R"({"error":"SECRET"})", "refused this sign-in");
    Auth(429,R"({"error_description":"SECRET"})", "temporarily unavailable");
    Auth(503,"<html>SECRET</html>", "temporarily unavailable");
    Auth(503,R"({"error":"invalid_grant","error_description":"Invalid user credentials"})", "temporarily unavailable");
    for (const char* body : {"", "<html>SECRET</html>", "{broken SECRET", "[]", "null",
         R"({"error":{"password":"SECRET"},"error_description":["SECRET"]})",
         R"({"error":"SECRET","error_description":"SECRET"})"})
      Auth(400,body,"did not provide a recognised authentication reason");
    std::cout << "PASS: " << checks << " request validation and safe authentication checks\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}
