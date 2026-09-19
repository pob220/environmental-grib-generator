cmake_minimum_required(VERSION 3.20)
project(environmental_grib_generator VERSION 0.1.8 LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)
find_package(BZip2 REQUIRED)
if(WIN32 OR ANDROID)
  if(ANDROID)
    # ecCodes' static Android export does not discover these transitive targets.
    find_package(PNG REQUIRED)
    find_package(tl-expected CONFIG REQUIRED)
    find_package(libaec CONFIG REQUIRED)
  endif()
  # vcpkg's exported CMake targets carry proper MSVC link paths.  Passing its
  # pkg-config -L flags through pkgconfiglite instead makes Visual Studio treat
  # the terminal "lib" directory as an object named lib.obj.
  find_package(eccodes CONFIG REQUIRED)
  find_package(jsoncpp CONFIG REQUIRED)
  find_package(netCDF CONFIG REQUIRED)
  find_package(CURL REQUIRED)
  if(ANDROID)
    find_package(OpenSSL REQUIRED)
    # OpenCPN exports an older OpenSSL. Force our complete, hidden TLS stack
    # into the plugin so the linker cannot resolve half of it against the host.
    set(ENVIRONMENTAL_GRIB_CURL_TARGET
      "-Wl,--whole-archive,$<TARGET_FILE:CURL::libcurl>,$<TARGET_FILE:OpenSSL::SSL>,$<TARGET_FILE:OpenSSL::Crypto>,--no-whole-archive"
      CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
  else()
    set(ENVIRONMENTAL_GRIB_CURL_TARGET CURL::libcurl)
  endif()
  find_package(Qhull CONFIG REQUIRED)
  find_package(blosc CONFIG REQUIRED)
  if(ANDROID)
    set(ENVIRONMENTAL_GRIB_BLOSC_TARGET blosc_static)
    # ecCodes 2.48 registers accessors through static initializers. Without
    # whole-archive, unreferenced accessor objects disappear from a static link.
    set(ENVIRONMENTAL_GRIB_ECCODES_TARGET
      "-Wl,--whole-archive,$<TARGET_FILE:eccodes>,--no-whole-archive" eccodes)
  else()
    set(ENVIRONMENTAL_GRIB_BLOSC_TARGET blosc_shared)
    set(ENVIRONMENTAL_GRIB_ECCODES_TARGET eccodes)
  endif()
  find_package(libzip CONFIG REQUIRED)
  find_package(unofficial-sodium CONFIG REQUIRED)
  find_package(zstd CONFIG REQUIRED)
  set(ENVIRONMENTAL_GRIB_DEPENDENCIES
    ${ENVIRONMENTAL_GRIB_ECCODES_TARGET} JsonCpp::JsonCpp netCDF::netcdf ${ENVIRONMENTAL_GRIB_CURL_TARGET} Qhull::qhull_r
    ${ENVIRONMENTAL_GRIB_BLOSC_TARGET} libzip::zip unofficial-sodium::sodium zstd::libzstd)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(ECCODES REQUIRED IMPORTED_TARGET eccodes)
  # Some multiarch distro packages expand the eccodes.pc includedir relative
  # to the pkgconfig directory (for example /usr/lib/include on Ubuntu arm64).
  # Resolve and validate the installed public header before exporting the
  # imported target to the engine and helper builds.
  find_path(ECCODES_HEADER_DIRECTORY NAMES eccodes.h
            HINTS ${ECCODES_INCLUDE_DIRS})
  if(NOT ECCODES_HEADER_DIRECTORY)
    message(FATAL_ERROR "Could not locate the ecCodes public headers")
  endif()
  set_property(TARGET PkgConfig::ECCODES PROPERTY INTERFACE_INCLUDE_DIRECTORIES
               "${ECCODES_HEADER_DIRECTORY}")
  pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
  pkg_check_modules(NETCDF REQUIRED IMPORTED_TARGET netcdf)
  pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
  pkg_check_modules(QHULL_R REQUIRED IMPORTED_TARGET qhull_r)
  pkg_check_modules(BLOSC REQUIRED IMPORTED_TARGET blosc)
  pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
  pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
  set(ENVIRONMENTAL_GRIB_DEPENDENCIES
    PkgConfig::ECCODES PkgConfig::JSONCPP PkgConfig::NETCDF PkgConfig::CURL
    PkgConfig::QHULL_R PkgConfig::BLOSC PkgConfig::LIBZIP PkgConfig::SODIUM
    PkgConfig::ZSTD)
endif()
if(ANDROID)
  find_package(PROJ CONFIG REQUIRED)
else()
  find_path(PROJ_INCLUDE_DIR proj.h HINTS $ENV{PROJ_ROOT} PATH_SUFFIXES include)
  find_library(PROJ_LIBRARY NAMES proj HINTS $ENV{PROJ_ROOT} PATH_SUFFIXES lib)
endif()

if(MSVC)
  set(ENVIRONMENTAL_GRIB_WINDOWS_UTF8_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/windows-utf8.manifest")
  function(environmental_grib_enable_windows_utf8 target)
    target_link_options(${target} PRIVATE
      "/MANIFEST:EMBED"
      "/MANIFESTINPUT:${ENVIRONMENTAL_GRIB_WINDOWS_UTF8_MANIFEST}")
  endfunction()

  # vcpkg builds ecCodes with OpenMP threading on Windows.  Install both the
  # MSVC runtime and vcomp140.dll beside the executable so a packaged helper
  # does not depend on Visual Studio or a machine-wide redistributable being
  # present on PATH.
  set(CMAKE_INSTALL_OPENMP_LIBRARIES TRUE)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
  include(InstallRequiredSystemLibraries)
endif()

add_library(environmental_grib_engine STATIC
  src/arco.cpp
  src/environment.cpp
  src/estimate.cpp
  src/copernicus.cpp
  src/geo.cpp
  src/grib.cpp
  src/job.cpp
  src/metno.cpp
  src/model.cpp
  src/netcdf.cpp
  src/providers.cpp
  src/random_access.cpp
  src/remote_currents.cpp
  src/rtofs.cpp
  src/security.cpp
  src/sources.cpp
  src/tpxo.cpp
  src/tpxo_model.cpp
  src/ukv.cpp
  src/weather.cpp
  src/waves.cpp
  src/xtd.cpp
  src/xtd_package.cpp
)
target_compile_features(environmental_grib_engine PUBLIC cxx_std_20)
target_include_directories(environmental_grib_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(environmental_grib_engine
  PUBLIC ${ENVIRONMENTAL_GRIB_DEPENDENCIES} BZip2::BZip2
)
if(MSVC)
  target_compile_definitions(environmental_grib_engine PUBLIC NOMINMAX)
  target_compile_options(environmental_grib_engine PRIVATE
    /W4 /wd4324 /wd4458)
else()
  target_compile_options(environmental_grib_engine PRIVATE
    -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()
if(ANDROID)
  target_link_libraries(environmental_grib_engine PUBLIC PROJ::proj)
  target_compile_definitions(environmental_grib_engine PUBLIC ENVIRONMENTAL_GRIB_HAVE_PROJ=1)
elseif(PROJ_INCLUDE_DIR AND PROJ_LIBRARY)
  target_include_directories(environmental_grib_engine PRIVATE ${PROJ_INCLUDE_DIR})
  target_link_libraries(environmental_grib_engine PUBLIC ${PROJ_LIBRARY})
  target_compile_definitions(environmental_grib_engine PUBLIC ENVIRONMENTAL_GRIB_HAVE_PROJ=1)
endif()

if(NOT ANDROID)
add_executable(environmental-grib src/cli.cpp)
target_link_libraries(environmental-grib PRIVATE environmental_grib_engine)
if(MSVC)
  environmental_grib_enable_windows_utf8(environmental-grib)
endif()
endif()

if(BUILD_TESTING)
  add_executable(environmental_grib_cancellation_tests tests/cancellation_tests.cpp)
  target_link_libraries(environmental_grib_cancellation_tests PRIVATE environmental_grib_engine)
  add_test(NAME environmental_grib_cancellation_tests COMMAND environmental_grib_cancellation_tests)
  add_executable(environmental_grib_estimate_tests tests/estimate_tests.cpp)
  target_link_libraries(environmental_grib_estimate_tests PRIVATE environmental_grib_engine)
  if(MSVC)
    environmental_grib_enable_windows_utf8(environmental_grib_estimate_tests)
  endif()
  add_test(NAME environmental_grib_estimate_tests COMMAND environmental_grib_estimate_tests)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND AND NOT ANDROID)
    add_test(NAME environmental_grib_size_report_cli_tests
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/size_report_cli_tests.py
              $<TARGET_FILE:environmental-grib>)
  endif()
  add_executable(environmental_grib_tests tests/engine_tests.cpp)
  target_link_libraries(environmental_grib_tests PRIVATE environmental_grib_engine)
  if(MSVC)
    environmental_grib_enable_windows_utf8(environmental_grib_tests)
  endif()
  add_test(NAME environmental_grib_tests COMMAND environmental_grib_tests)

  add_executable(environmental_grib_xtd_tests
    tests/xtd_tests.cpp
    tests/xtd_test_support.cpp
  )
  target_link_libraries(environmental_grib_xtd_tests PRIVATE environmental_grib_engine)
  if(MSVC)
    environmental_grib_enable_windows_utf8(environmental_grib_xtd_tests)
  endif()
  add_test(NAME environmental_grib_xtd_tests COMMAND environmental_grib_xtd_tests)

  # Private/offline authoring helper.  It consumes a locally licensed TPXO
  # source and emits a self-contained derived XTD height package; runtime
  # readers never link the NetCDF importer.
  add_executable(environmental_grib_tpxo_height_author
    tests/tpxo_height_author.cpp
    tests/xtd_test_support.cpp
  )
  target_include_directories(environmental_grib_tpxo_height_author PRIVATE
    tests)
  target_link_libraries(environmental_grib_tpxo_height_author PRIVATE
    environmental_grib_engine)

  add_executable(environmental_grib_merge_tests
    tests/environmental_merge_tests.cpp
  )
  target_link_libraries(environmental_grib_merge_tests PRIVATE
    environmental_grib_engine)
  if(MSVC)
    environmental_grib_enable_windows_utf8(environmental_grib_merge_tests)
  endif()
  set(ENVIRONMENTAL_GRIB_FIXTURE_DIR
    "${CMAKE_BINARY_DIR}/functional-fixtures")
  add_test(NAME environmental_grib_merge_tests
    COMMAND environmental_grib_merge_tests ${ENVIRONMENTAL_GRIB_FIXTURE_DIR})
  set_tests_properties(environmental_grib_merge_tests PROPERTIES
    FIXTURES_SETUP environmental_grib_combined_fixture)

  add_executable(environmental_grib_performance_benchmark
    tests/performance_benchmark.cpp
  )
  target_link_libraries(environmental_grib_performance_benchmark PRIVATE
    environmental_grib_engine)
  if(MSVC)
    environmental_grib_enable_windows_utf8(
      environmental_grib_performance_benchmark)
  endif()

  add_executable(environmental_grib_concurrency_tests
    tests/concurrency_tests.cpp
  )
  target_link_libraries(environmental_grib_concurrency_tests PRIVATE
    environmental_grib_engine)
  if(MSVC)
    environmental_grib_enable_windows_utf8(environmental_grib_concurrency_tests)
  endif()
  add_test(NAME environmental_grib_concurrency_tests
    COMMAND environmental_grib_concurrency_tests)
endif()

if(NOT ANDROID)
install(TARGETS environmental_grib_engine environmental-grib
  EXPORT environmental_grib_targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
