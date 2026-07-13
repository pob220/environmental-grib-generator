#!/usr/bin/env python3
"""Create a tiny format-faithful TPXO10 Atlas directory for C++ parity tests."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pyTMD
import xarray as xr


def main() -> int:
    root = Path(sys.argv[1]) / "TPXO10_atlas_v2"
    root.mkdir(parents=True, exist_ok=True)
    x = np.array([-1.0, -0.8, -0.6])
    y = np.array([50.0, 50.2, 50.4])
    depth = np.full((3, 3), 100.0)
    xr.Dataset(
        {
            "lon_u": (("nx",), x), "lat_u": (("ny",), y),
            "lon_v": (("nx",), x), "lat_v": (("ny",), y),
            "lon_z": (("nx",), x), "lat_z": (("ny",), y),
            "hu": (("nx", "ny"), depth, {"units": "meter"}),
            "hv": (("nx", "ny"), depth, {"units": "meter"}),
            "hz": (("nx", "ny"), depth, {"units": "meter"}),
        }
    ).to_netcdf(root / "grid_tpxo10atlas_v2.nc")

    database = Path(pyTMD.__file__).parent / "data/database.json"
    model = json.loads(database.read_text())["TPXO10-atlas-v2-nc"]
    for index, relative in enumerate(model["u"]["model_file"]):
        name = Path(relative).name
        constituent = name.split("_")[1]
        base = np.add.outer(np.arange(3), np.arange(3)).astype(float) + index + 1
        con = np.frombuffer(constituent.ljust(4).encode(), dtype="S1")
        xr.Dataset(
            {
                "con": (("nchar",), con),
                "lon_u": (("nx",), x), "lat_u": (("ny",), y),
                "lon_v": (("nx",), x), "lat_v": (("ny",), y),
                "uRe": (("nx", "ny"), depth * 100.0 * base, {"units": "cm^2/s"}),
                "uIm": (("nx", "ny"), depth * 100.0 * base * 0.1, {"units": "cm^2/s"}),
                "vRe": (("nx", "ny"), depth * 100.0 * base * 0.5, {"units": "cm^2/s"}),
                "vIm": (("nx", "ny"), depth * 100.0 * base * 0.05, {"units": "cm^2/s"}),
            }
        ).to_netcdf(root / name)
    for index, relative in enumerate(model["z"]["model_file"]):
        name = Path(relative).name
        constituent = name.split("_")[1]
        base = np.add.outer(np.arange(3), np.arange(3)).astype(float) + index + 1
        con = np.frombuffer(constituent.ljust(4).encode(), dtype="S1")
        xr.Dataset(
            {
                "con": (("nchar",), con),
                "lon_z": (("nx",), x), "lat_z": (("ny",), y),
                "hRe": (("nx", "ny"), base, {"units": "mm"}),
                "hIm": (("nx", "ny"), base * 0.1, {"units": "mm"}),
            }
        ).to_netcdf(root / name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
