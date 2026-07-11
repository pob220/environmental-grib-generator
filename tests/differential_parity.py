#!/usr/bin/env python3
"""Differential offline parity checks against tidal-current-grib-generator."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path


def run(command: list[str], *, env: dict[str, str] | None = None) -> str:
    result = subprocess.run(command, check=True, text=True, capture_output=True, env=env)
    return result.stdout


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compare_grib_values(first: Path, second: Path) -> None:
    import eccodes

    def messages(path: Path):
        result = []
        with path.open("rb") as handle:
            while (gid := eccodes.codes_grib_new_from_file(handle)) is not None:
                try:
                    result.append(
                        (
                            eccodes.codes_get(gid, "edition"),
                            eccodes.codes_get(gid, "indicatorOfParameter"),
                            eccodes.codes_get(gid, "validityDate"),
                            eccodes.codes_get(gid, "validityTime"),
                            list(eccodes.codes_get_values(gid)),
                        )
                    )
                finally:
                    eccodes.codes_release(gid)
        return result

    left, right = messages(first), messages(second)
    assert len(left) == len(right), (len(left), len(right))
    for index, (a, b) in enumerate(zip(left, right)):
        assert a[:4] == b[:4], (index, a[:4], b[:4])
        assert len(a[4]) == len(b[4])
        maximum = max((abs(x - y) for x, y in zip(a[4], b[4])), default=0.0)
        assert maximum <= 1e-12, (index, maximum)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpp-cli", type=Path, required=True)
    parser.add_argument("--python-repo", type=Path, required=True)
    args = parser.parse_args()
    python_cli = args.python_repo / ".venv/bin/tidal-current-grib"
    if not python_cli.exists():
        raise SystemExit(f"Python reference CLI not found: {python_cli}")

    with tempfile.TemporaryDirectory(prefix="environmental-grib-parity-") as directory:
        root = Path(directory)
        common = [
            "--bbox", "-7", "51.5", "-6.5", "52",
            "--start", "2026-07-01T00:00:00Z",
            "--hours", "6", "--step-hours", "3",
            "--grid-spacing-deg", "0.25", "--source", "synthetic",
        ]
        python_output, cpp_output = root / "python.grb", root / "cpp.grb"
        run([str(python_cli), "generate", *common, "--output", str(python_output)])
        cpp_json = json.loads(run([str(args.cpp_cli), "generate", *common, "--output", str(cpp_output)]))
        assert cpp_json["message_count"] == 6
        compare_grib_values(python_output, cpp_output)
        assert digest(python_output) == digest(cpp_output)

        env = os.environ.copy()
        env["PYTHONPATH"] = str(args.python_repo / "src")
        fixture = root / "currents.nc"
        fixture_script = """
import numpy as np, xarray as xr, sys
times=np.array(['2026-07-01T00:00:00','2026-07-01T01:00:00'],dtype='datetime64[ns]')
lats=np.array([51.5,52.0,52.5],dtype=np.float32)
lons=np.array([-7.0,-6.5,-6.0],dtype=np.float32)
xr.Dataset(data_vars={
'eastward_sea_water_velocity':(('time','latitude','longitude'),np.ones((2,3,3)),{'units':'cm/s'}),
'northward_sea_water_velocity':(('time','latitude','longitude'),np.full((2,3,3),2.0),{'units':'cm/s'})},
coords={'time':times,'latitude':lats,'longitude':lons}).to_netcdf(sys.argv[1])
"""
        run([str(args.python_repo / ".venv/bin/python"), "-c", fixture_script, str(fixture)], env=env)
        netcdf_common = [
            "--bbox", "-7", "51.5", "-6", "52.5",
            "--start", "2026-07-01T00:00:00Z", "--hours", "1",
            "--step-hours", "1", "--grid-spacing-deg", "0.5",
            "--source", "netcdf", "--input-netcdf", str(fixture),
        ]
        python_nc, cpp_nc = root / "python-nc.grb", root / "cpp-nc.grb"
        run([str(python_cli), "generate", *netcdf_common, "--output", str(python_nc)])
        run([str(args.cpp_cli), "generate", *netcdf_common, "--output", str(cpp_nc)])
        compare_grib_values(python_nc, cpp_nc)

        tpxo_cache = root / "fixture.tpxocache"
        tpxo_fixture_script = r'''
from datetime import datetime, timezone
from pathlib import Path
import numpy as np
import xarray as xr
from tidal_current_grib_generator.geo import BoundingBox, build_regular_grid
from tidal_current_grib_generator.sources.tpxo_cache import TPXOCacheMetadata, _write_cache_atomic
bbox=BoundingBox(-1.0,50.0,-0.9,50.1)
grid=build_regular_grid(bbox,0.1)
lon,lat=np.meshgrid(grid.longitudes,grid.latitudes)
lp,ap=lon.ravel(),lat.ravel()
constituents=['2n2','k1','k2','m2','m4','mf','mm','mn4','ms4','n2','o1','p1','q1','s1','s2']
rng=np.random.default_rng(42)
def component(group):
    variables={c:(('point',),(rng.normal(size=lp.size)+1j*rng.normal(size=lp.size))*10,{'units':'cm/s'}) for c in constituents}
    return xr.Dataset(variables,coords={'x':(('point',),lp),'y':(('point',),ap)},attrs={'group':group})
metadata=TPXOCacheMetadata(bbox,0.1,'fixture-tpxo','test',constituents,'ATLAS',[],[],datetime.now(timezone.utc).isoformat())
_write_cache_atomic(Path(sys.argv[1]),metadata,grid,lp,ap,{'u':component('u'),'v':component('v')})
'''
        # Keep sys imported separately so the fixture body remains close to the
        # production cache construction sequence.
        run([str(args.python_repo / ".venv/bin/python"), "-c",
             "import sys\n" + tpxo_fixture_script, str(tpxo_cache)], env=env)
        tpxo_common = [
            "--source", "tpxo-cache", "--input-cache", str(tpxo_cache),
            "--start", "2026-01-01T00:00:00Z", "--hours", "6",
            "--step-hours", "1",
        ]
        python_tpxo, cpp_tpxo = root / "python-tpxo.grb", root / "cpp-tpxo.grb"
        run([str(python_cli), "generate", *tpxo_common, "--output", str(python_tpxo)])
        run([str(args.cpp_cli), "generate", *tpxo_common, "--output", str(cpp_tpxo)])
        compare_grib_values(python_tpxo, cpp_tpxo)

        model_directory = root / "tpxo-model"
        run([str(args.python_repo / ".venv/bin/python"),
             str(Path(__file__).with_name("make_tpxo_fixture.py")),
             str(model_directory)], env=env)
        direct_common = [
            "--source", "tpxo", "--model-dir", str(model_directory),
            "--bbox", "-0.9", "50.1", "-0.7", "50.3",
            "--start", "2026-01-01T00:00:00Z", "--hours", "2",
            "--step-hours", "1", "--grid-spacing-deg", "0.1",
        ]
        python_direct = root / "python-direct.grb"
        cpp_direct = root / "cpp-direct.grb"
        run([str(python_cli), "generate", *direct_common,
             "--output", str(python_direct)])
        run([str(args.cpp_cli), "generate", *direct_common,
             "--output", str(cpp_direct)])
        compare_grib_values(python_direct, cpp_direct)

        cpp_prepared_cache = root / "cpp-prepared.tpxocache"
        run([str(args.cpp_cli), "prepare-tpxo-cache",
             "--model-dir", str(model_directory),
             "--bbox", "-0.9", "50.1", "-0.7", "50.3",
             "--grid-spacing-deg", "0.1", "--output", str(cpp_prepared_cache)])
        python_from_cpp_cache = root / "python-from-cpp-cache.grb"
        run([str(python_cli), "generate", "--source", "tpxo-cache",
             "--input-cache", str(cpp_prepared_cache),
             "--start", "2026-01-01T00:00:00Z", "--hours", "2",
             "--step-hours", "1", "--output", str(python_from_cpp_cache)])
        compare_grib_values(python_direct, python_from_cpp_cache)

        print(json.dumps({
            "status": "pass",
            "synthetic_sha256": digest(cpp_output),
            "synthetic_byte_identical": True,
            "netcdf_value_parity": True,
            "tpxo_cache_value_parity": True,
            "tpxo_direct_value_parity": True,
            "cpp_prepared_cache_python_compatible": True,
        }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
