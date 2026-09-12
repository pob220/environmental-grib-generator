#!/usr/bin/env python3
"""Read-only comparisons with an explicitly described local GRIB corpus.

Requires ecCodes grib_ls and the built environmental-grib helper. Does not
download, alter inputs, or infer the requested settings from its own estimates.
Case metadata must state whether requests are original or reconstructed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generator", required=True, type=Path)
    parser.add_argument("--cases", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    generator = args.generator.resolve(strict=True)
    cases = json.loads(args.cases.read_text())
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    rows = []
    for number, case in enumerate(cases):
        source = Path(case["file"]).resolve(strict=True)
        before = source.stat()
        request = dict(case["request"], output=str(output/"NOT-GENERATED.grb"))
        job = output/f"case-{number}.json"
        job.write_text(json.dumps({"schemaVersion": 1, "operation": "generateEnvironment",
                                   "request": request}, indent=2)+"\n")
        estimate = json.loads(subprocess.check_output(
            [str(generator), "estimate-job", "--job", str(job)], text=True, timeout=15))
        inventory = json.loads(subprocess.check_output([
            "grib_ls", "-j", "-p",
            "edition,shortName,typeOfLevel,level,endStep,numberOfPoints,Ni,Nj,packingType,bitsPerValue",
            str(source)], text=True, timeout=120))["messages"]
        if not inventory: raise ValueError(f"No GRIB records in {source}")
        actual_numeric = sum(int(item["numberOfPoints"]) * 8 for item in inventory)
        with source.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        after = source.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError(f"Input changed during comparison: {source}")
        predicted = estimate.get("decodedBytes")
        if predicted is None:
            numeric_status = "unknown"
        elif estimate["approximate"]:
            numeric_status = "conservative" if predicted >= actual_numeric else "underestimate"
        else:
            numeric_status = "exact" if predicted == actual_numeric else "mismatch"
        upper = estimate.get("fileUpperBytes")
        file_status = "unknown" if upper is None else (
            "within_upper_estimate" if before.st_size <= upper else "exceeds_upper_estimate")
        row = {"name": case["name"], "source": str(source), "sha256": digest,
               "request_basis": case["request_basis"], "actual_records": len(inventory),
               "actual_file_bytes": before.st_size, "actual_numeric_bytes": actual_numeric,
               "numeric_status": numeric_status, "file_status": file_status,
               "numeric_overestimate_percent": None if predicted is None else
                   100 * (predicted / actual_numeric - 1), "estimate": estimate}
        rows.append(row)
        (output/f"inventory-{number}.json").write_text(json.dumps(inventory, indent=2)+"\n")
        print(case["name"], numeric_status, file_status,
              f"actual numeric={actual_numeric / 1048576:.3f} MiB", flush=True)
    passed = all(r["numeric_status"] not in ("underestimate", "mismatch") and
                 r["file_status"] != "exceeds_upper_estimate" for r in rows)
    (output/"comparisons.json").write_text(json.dumps({"passed": passed, "cases": rows}, indent=2)+"\n")
    if not passed: raise SystemExit(1)


if __name__ == "__main__": main()
