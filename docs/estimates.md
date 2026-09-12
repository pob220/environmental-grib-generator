# Offline size estimates (development)

`environmental-grib estimate-job --job request.json` accepts the same version-1
`generateEnvironment` job envelope as `run-job`. It only parses the small JSON
request and computes an estimate. It never downloads, loads source/model files,
reads passwords, generates GRIBs or writes the requested output. Capabilities
advertise it as `offlineEstimateCommand`. Invalid settings produce a nonzero exit.

The result uses schemaVersion 1:

- `components`: planned/approximate/unknown entries with reasons.
- `knownRecords` and `knownDecodedBytes`: subtotal of supported components.
- `decodedBytes`: present only when all selected components are supported.
- `fileUpperBytes`: present only when all selected components use known packing.
- `complete`, `approximate`, `notes`: interpretation, not success/coverage guarantees.

Numeric storage counts each U/V, time and level separately at eight bytes per
cell. It excludes masks, metadata, temporary copies, display/interpolation data,
model caches and routing/search state. It is **not total application RAM**.
`fileUpperBytes` is a conservative planning estimate for our simple-packed
writers: 16-bit current / 24-bit weather payload plus a full bitmap and 512 bytes
of framing per record. Constants and missing cells can make files much smaller.
It is not an expected compression ratio or a bound on downloads/intermediates.

## First-pass coverage

| Source / mode | What is estimated |
| --- | --- |
| UKV, extension off | Shared output grid dimensions, actual forecast-hour selection, selected output fields including pressure levels and absent hour-zero rain; packed upper estimate |
| Direct TPXO, XTD offline, NetCDF interpolated or synthetic currents, extension off | Shared requested output grid/time sequence, two components; packed upper estimate |
| GFS minimal/routing, through 120 hours, extension off | Approximate quarter-degree numeric storage with conservative edge alignment; disk packing unknown |
| Other weather, waves, existing GRIBs, existing TPXO cache, remote/automatic currents | Unknown pending native grid/record/time metadata |
| Forecast extension | Unknown pending actual source-cycle/fallback/merge inventory |

The current/cache distinction matters: an existing TPXO cache may have a grid
different from the selected output spacing. Native regional GRIB streams may
cover their full domain regardless of the requested area. The estimator does
not pretend these are cropped regular grids. GFS all/marine filter requests
select variable/level combinations needing a real inventory; they are unknown.
Cross-dateline requests remain rejected consistently with the generator.

No live-provider accuracy or Windows/macOS GUI qualification is claimed here.
Further work: cached header inventories, extension handover plans and pinned
provider-output comparisons, before describing coverage as complete.

## Comparing archived provider files

`tests/compare_size_estimates.py --generator PATH --cases cases.json --output NEW_DIRECTORY`
compares the actual helper result with ecCodes record inventories and file sizes.
Each case supplies `name`, `file`, `request_basis` and `request` (the normal job
request object). Inputs are read-only, hashed, and checked for changes during
the scan. There is no download and no generation. Preserve original request
metadata for new captures; explicitly label retrospective reconstructions.

The first local corpus (12 September 2026) matched decoded numeric storage
exactly for a 54-hour UKV routing file, a mixed-resolution 24-hour UKV/TPXO file
and a small-area 24-hour TPXO file. Packed bytes stayed below all three upper
planning estimates. A 24-hour GFS routing file had an 8.16% conservative numeric
overestimate from edge alignment; its packed size correctly remained unknown.
These were reconstructed requests, not independent prospective request tests.
