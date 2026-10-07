"""Offline preflight/estimate/job protocol and consent regressions."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    helper = str(Path(sys.argv[1]).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="xgrib-preflight-cli-") as directory:
        root = Path(directory)
        job, result, output = root / "job.json", root / "result.json", root / "forecast.grb"
        request = {"bbox": {"west": 179, "south": -1, "east": -179, "north": 1},
                   "start": "2026-10-07T21:00:00Z", "hours": 12, "stepHours": 1,
                   "weatherProvider": "ecmwf_aifs_open", "currentSource": "none",
                   "timePolicy": "review", "output": str(output)}

        def save():
            job.write_text(json.dumps({"schemaVersion": 1, "operation": "generateEnvironment", "request": request}))

        def estimate():
            save()
            return json.loads(subprocess.check_output([helper, "estimate-job", "--job", str(job)], text=True, timeout=10))

        plan = estimate()["preflight"]
        assert not plan["ready"] and not plan["availabilityChecked"]
        assert plan["issues"][0]["actions"]
        for dry in (False, True):
            request["dryRun"] = dry
            save()
            run = subprocess.run([helper, "run-job", "--job", str(job), "--result", str(result)], capture_output=True, text=True, timeout=10)
            error = json.loads(result.read_text())["error"]
            assert run.returncode == 1 and error["code"] == "preflight_required"
            assert "6-hour" in error["message"] and error["preflight"]["actions"]
            assert not output.exists()
        # Invalid duration must still provide options when a size estimate
        # cannot be computed. No follow-on provider requests are needed.
        request.update(hours=13, stepHours=6)
        plan = estimate()["preflight"]
        assert not plan["ready"] and plan["issues"][0]["actions"]
        for action in plan["issues"][0]["actions"]:
            patch = action["request"]
            assert patch["hours"] % patch["stepHours"] == 0
        request.update(hours=6, stepHours=3, weatherProvider="none", currentSource="synthetic", currentGridSpacingDeg=1, dryRun=False)
        assert estimate()["preflight"]["ready"]
        subprocess.run([helper, "run-job", "--job", str(job), "--result", str(result)], check=True, capture_output=True, text=True, timeout=10)
        completed = json.loads(result.read_text())
        assert completed["status"] == "complete" and output.exists()
        assert completed["result"]["diagnostics"]["time_coverage"]["policy"] == "review"
        assert completed["result"]["size_comparison"]["estimate"]["preflight"]["ready"]
    print("Temporal preflight CLI tests passed")


if __name__ == "__main__":
    main()
