# Environmental GRIB Generator (C++)

A standalone native engine reproducing the production generation paths of
`tidal-current-grib-generator`. It has no Python or wxWidgets runtime
dependency, allowing the eventual OpenCPN catalogue plugin, CLI and tests to
call one implementation.

The native executable also provides a versioned job-file interface intended
for the OpenCPN plugin. See [docs/JOB_PROTOCOL.md](docs/JOB_PROTOCOL.md).

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
