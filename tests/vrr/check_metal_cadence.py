#!/usr/bin/env python3
"""Gate OS presentation cadence on a constant-rate native Metal smoke trace.

Submission throughput alone does not establish adaptive display scheduling.
This checks matched drawable presentedTime events, not physical panel scanout.
"""

import argparse
import csv
import importlib.util
import io
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "decode_vrr_trace", Path(__file__).resolve().parents[2] / "scripts/decode-vrr-trace.py")
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--fps", type=float, required=True)
    parser.add_argument("--tolerance-us", type=float, default=500)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 0 < args.fps <= 1000 or not 0 < args.tolerance_us < 1000000 / args.fps:
        parser.error("FPS and tolerance must be positive, with tolerance below one frame period")
    decoded = io.BytesIO()
    with args.trace.open("rb") as trace:
        decoder.decode(trace, decoded)
    rows = list(csv.DictReader(io.StringIO(decoded.getvalue().decode("utf-8"))))
    presented = [r for r in rows if r.get("presented") == "1"]
    if any(r.get("native_backend") != "4" for r in presented):
        parser.error("trace contains non-Metal presentation")
    # Discard initial warmup; callbacks may include the pre-worker smoke draws.
    ids = {int(r["submission_id"]) for r in presented[32:]}
    events = {int(r["latch_submission_id"]): int(r["latch_time_us"])
              for r in presented if r.get("latch_valid") == "1" and
              int(r["latch_submission_id"]) in ids and r.get("latch_time_kind") == "2"}
    intervals = [events[k] - events[k - 1] for k in sorted(events) if k - 1 in events]
    errors = sorted(abs(interval - 1000000 / args.fps) for interval in intervals)
    within = sum(error <= args.tolerance_us for error in errors)
    result = {
        "trace": str(args.trace.resolve()), "fps": args.fps,
        "scope": "OS drawable presentation events; constant-rate smoke only, no optical measurement",
        "submitted_frames": len(presented), "steady_submissions": len(ids),
        "matched_display_events": len(events), "consecutive_display_intervals": len(intervals),
        "display_event_coverage_percent": 100 * len(events) / len(ids) if ids else 0,
        "intervals_within_tolerance_percent": 100 * within / len(errors) if errors else 0,
        "interval_error_p95_us": errors[int((len(errors) - 1) * .95)] if errors else None,
        "tolerance_us": args.tolerance_us,
    }
    result["passed"] = (len(intervals) >= 32 and
                        result["display_event_coverage_percent"] >= 90 and
                        result["intervals_within_tolerance_percent"] >= 95)
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text, end="")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
