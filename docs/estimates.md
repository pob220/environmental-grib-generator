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

## Current development coverage

| Source / mode | What is estimated |
| --- | --- |
| UKV, extension off | Shared output grid dimensions, actual forecast-hour selection, selected output fields including pressure levels and absent hour-zero rain; packed upper estimate |
| Direct TPXO, XTD offline, NetCDF interpolated or synthetic currents, extension off | Shared requested output grid/time sequence, two components; packed upper estimate |
| Copernicus NWS, Global, IBI and Mediterranean currents, extension off | Requested regular output grid and U/V time sequence; packed upper estimate (successful full-coverage generation assumed) |
| Copernicus Global Waves, extension off | Three fields at 3-hour steps, through 240h; uses the generator's requested weather-grid spacing, not the native source spacing; packed upper estimate |
| GFS minimal/routing, through 120 hours, extension off | Approximate quarter-degree numeric storage with conservative edge alignment; disk packing unknown |
| Other weather, GFS waves, existing GRIBs, existing TPXO cache, other remote/automatic currents | Unknown pending native grid/record/time metadata |
| Forecast extension | Unknown pending actual source-cycle/fallback/merge inventory |

The current/cache distinction matters: an existing TPXO cache may have a grid
different from the selected output spacing. Native regional GRIB streams may
cover their full domain regardless of the requested area. The estimator does
not pretend these are cropped regular grids. GFS all/marine filter requests
select variable/level combinations needing a real inventory; they are unknown.
Cross-dateline requests remain rejected consistently with the generator.
GFS Wave's indexed fallback can download full-domain records, so a small-area
estimate based only on the usual filtered quarter-degree output is unsafe.

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

## Measured totals and prospective reports

`run-job` captures an estimate before generation and includes a
`result.size_comparison` object in a successful result. It reuses the existing
final merged inspection: no extra scan, decode or model load is introduced.
The actual numeric count includes all fields, levels, times and masked cells,
even when a pre-generation estimate was partial or unknown. A missing/partial
inspection never becomes a complete zero total. Dry runs have no measured report.
Reporting failures do not prevent otherwise supported generation.

After successful GUI generation, the summary shows actual packed file size and
the complete actual numeric-data size when available. Older helpers remain
usable; their actual numeric size is shown as unknown, not as a stale estimate.
The GUI saves `<output>.size-report.json` beside the GRIB using a staged write.
Failure to save this optional report leaves the generated GRIB usable.

Reports contain an allowlisted snapshot of size-relevant settings, the original
estimate, actual counts and comparison status. Credentials, URLs and local
source/model paths are excluded. **Area/time and the output basename are
included: review before sharing.** Nothing is uploaded automatically. Reports
do not reproduce model inputs or pin the provider's forecast cycle for replay.

`tests/compare_size_estimates.py --generator PATH --reports FILE.size-report.json
--output NEW_DIRECTORY` rechecks a saved report against its neighbouring GRIB
and the current estimator. It verifies recorded size/record/numeric counts,
retains the captured estimate and hashes the current GRIB; it is not proof that
a file with identical counts has never been replaced. The `--cases` workflow
still supports explicitly labelled retrospective requests.

Additional local checks matched Copernicus-wave, UKV/NWS-current and small-area
global-current archived numeric counts exactly. A mixed UKV/NWS/GFS-wave file
remains honestly partial before generation; its complete numeric inventory can
be displayed afterward. Mocked Copernicus provider conversions and three
synthetic CLI jobs also compare estimates captured **before** generation.

The goal is a useful best estimate, not exact compression prediction. Cached
native inventories and extension handovers remain deferred; no low-memory
mode or general peak-RAM guarantee is implied.
