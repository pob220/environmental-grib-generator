cmake_minimum_required(VERSION 3.20)
project(environmental_grib_generator VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)
find_package(PkgConfig REQUIRED)
find_package(BZip2 REQUIRED)
pkg_check_modules(ECCODES REQUIRED IMPORTED_TARGET eccodes)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
pkg_check_modules(NETCDF REQUIRED IMPORTED_TARGET netcdf)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(QHULL_R REQUIRED IMPORTED_TARGET qhull_r)
pkg_check_modules(BLOSC REQUIRED IMPORTED_TARGET blosc)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)
find_path(PROJ_INCLUDE_DIR proj.h HINTS $ENV{PROJ_ROOT} PATH_SUFFIXES include)
find_library(PROJ_LIBRARY NAMES proj HINTS $ENV{PROJ_ROOT} PATH_SUFFIXES lib)

add_library(environmental_grib_engine
  src/arco.cpp
  src/environment.cpp
  src/copernicus.cpp
  src/geo.cpp
  src/grib.cpp
  src/job.cpp
  src/model.cpp
  src/netcdf.cpp
  src/providers.cpp
  src/remote_currents.cpp
  src/rtofs.cpp
  src/security.cpp
  src/sources.cpp
  src/tpxo.cpp
  src/tpxo_model.cpp
  src/ukv.cpp
  src/weather.cpp
  src/waves.cpp
)
target_compile_features(environmental_grib_engine PUBLIC cxx_std_20)
target_include_directories(environmental_grib_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(environmental_grib_engine
  PUBLIC PkgConfig::ECCODES PkgConfig::JSONCPP PkgConfig::NETCDF PkgConfig::CURL
         PkgConfig::QHULL_R BZip2::BZip2
         PkgConfig::BLOSC PkgConfig::LIBZIP
)
target_compile_options(environmental_grib_engine PRIVATE
  -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
if(PROJ_INCLUDE_DIR AND PROJ_LIBRARY)
  target_include_directories(environmental_grib_engine PRIVATE ${PROJ_INCLUDE_DIR})
  target_link_libraries(environmental_grib_engine PUBLIC ${PROJ_LIBRARY})
  target_compile_definitions(environmental_grib_engine PUBLIC ENVIRONMENTAL_GRIB_HAVE_PROJ=1)
endif()

add_executable(environmental-grib src/cli.cpp)
target_link_libraries(environmental-grib PRIVATE environmental_grib_engine)

if(BUILD_TESTING)
  add_executable(environmental_grib_tests tests/engine_tests.cpp)
  target_link_libraries(environmental_grib_tests PRIVATE environmental_grib_engine)
  add_test(NAME environmental_grib_tests COMMAND environmental_grib_tests)
endif()

install(TARGETS environmental_grib_engine environmental-grib
  EXPORT environmental_grib_targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
