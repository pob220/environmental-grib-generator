# Native helper job protocol

The OpenCPN plugin invokes `environmental-grib` as a separate native helper.
This keeps provider and format dependencies outside the OpenCPN process. The
protocol is versioned independently of the command-line convenience commands.

## Invocation

```sh
environmental-grib run-job --job request.json --result result.json
```

The result file is written atomically. It is first created with `status` set to
`running`, then replaced with a `complete` or `failed` result. Standard output
contains one compact JSON object per line. Event types are `started`,
`progress`, `complete`, and `failed`.

## Request schema version 1

```json
{
  "schemaVersion": 1,
  "operation": "generateEnvironment",
  "request": {
    "bbox": {"west": -8.5, "south": 50.5, "east": -2.5, "north": 56.5},
    "start": "2026-07-12T00:00:00Z",
    "hours": 72,
    "stepHours": 3,
    "weatherProvider": "gfs",
    "weatherPreset": "routing",
    "includeWaves": true,
    "waveProvider": "gfs_wave",
    "currentSource": "tpxo-cache",
    "inputCache": "/path/to/cache.tpxocache",
    "tpxoModelDirectory": "/path/to/tide-models",
    "autoPrepareTpxoCache": true,
    "output": "/path/to/environment.grb",
    "overwrite": true
  },
  "credentials": {
    "copernicusPasswordEnvironment": "ENVIRONMENTAL_GRIB_COPERNICUS_PASSWORD"
  }
}
```

Passwords must never be placed in a job file. The named environment variable
is read by the helper after validating the schema.

## Discovery and compatibility

`environmental-grib capabilities` reports the job schema, helper version,
progress protocol, and implemented providers. Unknown schema versions,
operations, options, and malformed values are rejected before generation.
