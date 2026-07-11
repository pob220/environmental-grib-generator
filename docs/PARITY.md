# Python-to-C++ parity contract

The Python `tidal-current-grib-generator` checkout is the behavioural oracle.
Generated fields are compared after ecCodes decoding; byte identity is also
required where both implementations use the same packing.

| Subsystem | Native status | Parity evidence |
| --- | --- | --- |
| Bounding boxes, regular grids, UTC times | Implemented | Unit and differential fixtures |
| Vector units and direction conventions | Implemented | Numerical unit tests |
| Provider registry and automatic current selection | Implemented | Registry tests |
| Credential/log redaction | Implemented | Secret and URL tests |
| Strict GRIB scan, normalisation, inspection and merge | Implemented | Corrupt/wrapped stream fixtures |
| GRIB1 current and GRIB2 weather/wave writing | Implemented | ecCodes field/value comparison |
| Synthetic, constant and local NetCDF currents | Implemented | Byte/value differential tests |
| Marine.ie current GRIB | Implemented | Wrapped-stream and component fixtures |
| Copernicus NWS currents | Implemented | Mock STAC/Blosc fixture and bounded public-ARCO smoke test; authenticated live value comparison requires a user account |
| Copernicus Global currents | Implemented | Multidimensional float Zarr fixture; authenticated live comparison requires a user account |
| NOAA RTOFS regional/global files | Implemented | Inventory and curvilinear NetCDF/Qhull fixtures |
| GFS and GFS Wave | Implemented | URL/cycle/atomic assembly fixtures and bounded live byte-identical GFS run |
| HRRR | Implemented | Inventory/range/cadence fixtures |
| ICON-EU | Implemented | URL/cadence/bzip2 fixtures |
| ECMWF IFS/AIFS | Implemented | URL/index/range/field fixtures; AIFS remains experimental upstream |
| Copernicus Global Waves | Implemented | Local NetCDF and remote packed-Zarr fixtures |
| UKMO UKV | Implemented when PROJ is available | Projected fixtures and bounded live differential run (maximum field difference 0.0028 in source units) |
| TPXO cache prediction | Implemented | 15-constituent major/minor pyTMD differential test |
| TPXO10 Atlas v2 direct model | Implemented | Complete synthetic ATLAS directory differential test |
| TPXO cache preparation | Implemented | C++ cache loaded/predicted by Python with exact result parity |
| Combined environmental orchestration | Implemented | Native-provider and existing-file assembly fixtures |

The Python inspection, discovery, reference-comparison and benchmark commands
remain useful development tools but are not runtime dependencies of generation.
NOAA OFS/S-111 is discovery-only in both implementations and is rejected as a
generation source.

## Deliberate safety constraints

- Copernicus credentials are validated and never accepted merely because an
  anonymous object request happens to work.
- UKV is unavailable when native PROJ is absent; it is never approximated.
- TPXO source data remains user-supplied and licensed. Prepared caches retain
  the redistribution warning.
- Unsupported data layouts and malformed caches fail explicitly.
- Every output is scanned as a strict GRIB stream before it is returned.

Navigation use should still be gated by a plugin-level integration test using
the actual requested providers, time range and OpenCPN GRIB reader.
