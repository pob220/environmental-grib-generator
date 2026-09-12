"""Offline, prospective CLI estimate/generate/compare protocol regression."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    generator = str(Path(sys.argv[1]).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="xgrib-size-report-") as directory:
        root = Path(directory)
        for index, (hours, step, spacing) in enumerate(((0, 1, .1), (6, 3, .05), (12, 1, .025))):
            output = root / f"forecast-{index}.grb"
            job = root / f"request-{index}.json"
            result_path = root / f"result-{index}.json"
            request = {"bbox": {"west": -6, "south": 53, "east": -5.9, "north": 53.1},
                       "start": "2026-09-12T12:00:00Z", "hours": hours, "stepHours": step,
                       "weatherProvider": "none", "currentSource": "synthetic",
                       "currentGridSpacingDeg": spacing, "output": str(output),
                       "copernicusUsername": "private-sentinel-user",
                       "offlineTidalFile": "/private-sentinel-model.xtd"}
            job.write_text(json.dumps({"schemaVersion": 1, "operation": "generateEnvironment",
                                       "request": request}))
            before = json.loads(subprocess.check_output(
                [generator, "estimate-job", "--job", str(job)], text=True, timeout=15))
            assert not output.exists(), "estimating must not generate"
            subprocess.run([generator, "run-job", "--job", str(job), "--result", str(result_path)],
                           check=True, capture_output=True, text=True, timeout=60)
            completed = json.loads(result_path.read_text())
            assert completed["status"] == "complete"
            report = completed["result"]["size_comparison"]
            assert report["estimate"] == before, "captured estimate must precede generation"
            assert report["numericStatus"] == "exact"
            assert report["fileStatus"] == "within_upper_estimate"
            assert report["actual"]["fileBytes"] == output.stat().st_size
            assert report["actual"]["records"] == 2 * (hours // step + 1)
            assert report["actual"]["decodedBytes"] == before["decodedBytes"]
            assert report["outputFile"] == output.name
            assert "private-sentinel" not in json.dumps(report)
        # Old/unavailable estimates are diagnostic only, never a generation
        # prerequisite. A dry run must not claim a measured final inventory.
        request["dryRun"] = True
        request["output"] = str(root / "dry-run.grb")
        job.write_text(json.dumps({"schemaVersion": 1, "operation": "generateEnvironment",
                                   "request": request}))
        subprocess.run([generator, "run-job", "--job", str(job), "--result", str(result_path)],
                       check=True, capture_output=True, text=True, timeout=60)
        completed = json.loads(result_path.read_text())
        assert "size_comparison" not in completed["result"]
        assert not Path(request["output"]).exists()
    print("Prospective size-report CLI tests passed")


if __name__ == "__main__":
    main()
