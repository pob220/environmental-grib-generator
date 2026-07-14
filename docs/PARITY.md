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
| Copernicus NWS currents | Implemented | Mock STAC/Blosc fixture, bounded retries and attempted-host diagnostics. The NWS catalogue currently depends on CloudFerro waw3; no silent global-model substitution is made when that regional catalogue is unavailable. |
| Copernicus Global currents | Implemented | Multidimensional float Zarr fixture plus authenticated live 2 x 2 field generation through the official waw4 fallback; ecCodes validated two GRIB1 current messages. |
| Copernicus IBI currents | Implemented | Explicit 1/36-degree hourly regional source including tides and non-tidal model processes. Its official metadata and ARCO payloads are mirrored on waw4. Coverage ends at 56.08 N, so it is not silently substituted for NWS. |
| Copernicus Mediterranean currents | Implemented | Explicit 4.2 km hourly regional source including tidal and non-tidal model processes, available from the official waw4 mirror. |
| NOAA RTOFS regional/global files | Implemented | Inventory and curvilinear NetCDF/Qhull fixtures |
| GFS and GFS Wave | Implemented | URL/cycle/atomic assembly fixtures and bounded live byte-identical GFS run |
| HRRR | Implemented | Inventory/range/cadence fixtures |
| ICON-EU | Implemented | URL/cadence/bzip2 fixtures |
| ECMWF IFS/AIFS | Implemented | URL/index/range/field fixtures; AIFS remains experimental upstream |
| Copernicus Global Waves | Implemented | Local NetCDF and remote packed-Zarr fixtures plus authenticated live 2 x 2 generation through the official waw4 fallback; ecCodes validated three GRIB2 wave messages. |
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
- Copernicus metadata roots are read from the official client configuration
  and supplemented with documented CloudFerro hosts. Retry counts and delays
  are bounded. A selected provider is never silently replaced by another
  product when one cloud host is unavailable.
- UKV is unavailable when native PROJ is absent; it is never approximated.
- TPXO source data remains user-supplied and licensed. Prepared caches retain
  the redistribution warning.
- Unsupported data layouts and malformed caches fail explicitly.
- Every output is scanned as a strict GRIB stream before it is returned.

Navigation use should still be gated by a plugin-level integration test using
the actual requested providers, time range and OpenCPN GRIB reader.
