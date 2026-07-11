# Native generator architecture

The engine is independent of OpenCPN UI code. Provider adapters produce typed
weather, wave or current grids/streams; GRIB writers and the environment
orchestrator validate and merge them atomically. The future plugin should link
this library and expose progress/cancellation through adapters rather than
starting a Python subprocess.

Remote providers use injectable HTTP functions. This keeps credentials and
network failures testable without live services. Copernicus ARCO readers decode
only required Zarr chunks. RTOFS uses a reusable triangulation and spatial bin
index. UKV performs native PROJ transformation and interpolates vector
components, not direction angles.

TPXO has two paths sharing one predictor:

1. A regional hyperslab reader converts user-supplied TPXO10 transports to
   currents, then interpolates harmonic constants.
2. A compressed NPZ cache stores those constants for repeated use.

The ATLAS astronomical/nodal and minor-constituent equations are native C++ and
differentially tested against pyTMD. Licensed TPXO data is never bundled.

All generated files pass the strict GRIB scanner. Unsupported layouts,
unavailable projection support, malformed archives, missing credentials and
incomplete streams are errors rather than approximate output.
