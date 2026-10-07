# Environmental GRIB Generator (C++)

A standalone native engine reproducing the production generation paths of
`tidal-current-grib-generator`. It has no Python or wxWidgets runtime
dependency, allowing the eventual OpenCPN catalogue plugin, CLI and tests to
call one implementation.

The native executable also provides a versioned job-file interface intended
for the OpenCPN plugin. See [docs/JOB_PROTOCOL.md](docs/JOB_PROTOCOL.md).

Version 0.3.3 adds local forecast-time preflight to `estimate-job` and structured
`preflight_required` errors to `run-job`. GUI requests use `timePolicy: review`:
unsupported settings and components without a usable common UTC period require
a decision, while usable partial field coverage retains every record and adds
an advisory. Preferred/fallback timelines are assessed together, including
equivalent UKV/GFS mean sea-level pressure encodings. Explicit `shared-period`
and `common-times` policies filter stored records without interpolation or
extrapolation; omitted `timePolicy` retains legacy `keep-all` behaviour.

Version 0.3.2 adds date-line-crossing request boxes and continuous output grids
for all geographically viable provider combinations. Bounds remain in
`[-180, 180]`; west greater than east selects the eastward crossing interval.
GFS requests use an unwrapped east longitude, while ECMWF global downloads
are cropped locally. Cyclic NetCDF, Copernicus ARCO and TPXO sampling wraps
neighbour columns; wave directions interpolate around the short angular arc.
Imported regular latitude/longitude fragments are stitched into one record per
field/time, preserving masks and rejecting gaps, mismatched cycles and
conflicting overlaps. Framing validation and field assembly avoid loading an
entire forecast into memory. These changes ship in xGRIB 0.3.7.

Version 0.3.1 adds automatic regional hosted failover for NOAA GFS outages and
rate limits. It preserves completed timesteps, validates the exact forecast,
fields, levels, native grid and SHA-256 digest, obeys service `Retry-After`, and
keeps cancellation responsive. Paired GFS atmosphere/wave jobs share an outage
flag so they stop starting new NOAA requests after failure. Other providers
retain their existing retries. The hosted cache currently covers minimal/routing
weather and the three GFS wave fields; unsupported presets fail explicitly.

Version 0.3.0 combines the global/wrapped-grid correction and Ubuntu 22.04
compatibility with Android in-process generation, cooperative cancellation,
isolated TLS linkage and the corrected NOAA GFS mean-sea-level pressure field.
Desktop command-line behaviour remains available unchanged.

Version 0.1.11 retains the 0.1.10 global/wrapped-grid fix and makes its
provider-style regression fixture compatible with the older ecCodes shipped
by Ubuntu 22.04.

Version 0.1.10 fixes weather/current merge coverage for global and wrapped
regular longitude grids, including ECMWF IFS/AIFS near Tonga. Coverage uses
the column count, longitude increment and scan direction rather than sorting
the first/last longitude. The inspection's `coverage.regions` array preserves
individual rectangles with an unwrapped east longitude (possibly above 180),
while the existing coverage bounds remain a conventional envelope. Merge
validation compares these regions modulo 360, preserving gaps and latitude
restrictions. GRIB data and coordinates are not rewritten. This does not add
support for antimeridian-crossing generation request boxes until 0.3.2.

Version 0.1.8 added offline `estimate-job` planning for supported weather, wave
and current outputs, plus pre-generation versus measured-size reports in
completed jobs. See [coverage and reporting limits](docs/estimates.md).

Weather generation supports NOAA GFS and HRRR, Met Office UKV, MET Norway's
Nordic forecast, DWD ICON-EU, and ECMWF IFS/AIFS Open Data. Presets are
`minimal`, `routing`, `marine`, and provider-aware `all`. The latter selects
all fields which the target xGRIB reader can display; it is intentionally not
the upstream provider's unrestricted field inventory.

## Dependencies

The engine uses maintained distribution libraries: ecCodes, JsonCpp, NetCDF-C,
libcurl, Qhull, bzip2, Blosc, libzip and (for UKV) PROJ.

Arch Linux development packages:

```sh
sudo pacman -S cmake gcc eccodes jsoncpp netcdf curl qhull bzip2 blosc libzip proj
```

UKV is disabled explicitly if PROJ is unavailable. Other providers continue to
build and run.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

python3 tests/differential_parity.py \
  --cpp-cli build/environmental-grib \
  --python-repo /path/to/tidal-current-grib-generator
```

## Examples

```sh
./build/environmental-grib generate \
  --bbox -7 51.5 -4 55.5 \
  --start 2026-07-01T00:00:00Z --hours 6 --step-hours 3 \
  --grid-spacing-deg 0.25 --source synthetic --output /tmp/current.grb

./build/environmental-grib prepare-tpxo-cache \
  --model-dir /path/to/licensed/tpxo \
  --bbox -8 49 -3 56 --grid-spacing-deg 0.05 \
  --output /path/to/local-current.tpxocache

./build/environmental-grib generate \
  --source tpxo-cache --input-cache /path/to/local-current.tpxocache \
  --start 2026-07-01T00:00:00Z --hours 72 --step-hours 1 \
  --output /tmp/tidal-current.grb

./build/environmental-grib inspect-grib /tmp/tidal-current.grb
```

Copernicus passwords are accepted through an environment-variable name, never
as a command-line value. See `docs/PARITY.md` for the exact evidence and safety
gates for each provider.

Copernicus metadata endpoints are discovered from the official Marine client
configuration. Downloads use bounded retries and try each advertised or
documented CloudFerro catalogue host. The provider selected by the caller is
never silently replaced with a different model: if a regional catalogue is not
available on a fallback host, the request fails with the attempted-host
diagnostics instead of substituting lower-resolution global data.

The provider list also exposes the Copernicus IBI 1/36-degree and
Mediterranean 4.2 km hourly surface-current products explicitly. Both contain
tidal and non-tidal model processes. IBI is a high-resolution choice for the
Irish Sea and North Channel, but its published northern boundary is 56.08 N;
it is not equivalent to the wider 1.5 km NWS product.
