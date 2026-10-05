#!/usr/bin/env python3
"""Check exported Metal feedback, exact replay, and native telemetry rejection.

Usage: check_metal_trace_audit.py /path/to/vrrreplay /path/to/metal-feedback.csv
Set MOONLIGHT_VRR_TEST_EXPORT_METAL_TRACE while running tst_vrrpacingworker to
create the fixture. Mutations repair the footer hash before semantic auditing.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("replay", type=Path)
    parser.add_argument("fixture", type=Path)
    args = parser.parse_args()
    original = args.fixture.read_bytes().splitlines(keepends=True)
    columns = original[0].decode().strip().split(",")
    selected = next(i for i, line in enumerate(original[1:], 1)
                    if not line.startswith(b"#") and
                    line.decode().strip().split(",")[columns.index("latch_valid")] == "1")
    semantic = "row_semantic_integrity"
    timestamp = "timestamp_integrity"
    cases = [
        ({"native_backend": 5}, semantic, "native_outcome_relationship_mismatch_rows"),
        ({"native_present_result": 1}, semantic, "native_outcome_relationship_mismatch_rows"),
        ({"submission_id_valid": 0, "submission_id": 0}, semantic,
         "native_outcome_relationship_mismatch_rows"),
        ({"latch_time_kind": 1}, semantic, "native_outcome_relationship_mismatch_rows"),
        ({"submission_id_query_result_valid": 1}, semantic,
         "native_outcome_relationship_mismatch_rows"),
        ({"frame_stats_query_result_valid": 1}, semantic,
         "native_outcome_relationship_mismatch_rows"),
        ({"native_present_parameters_valid": 1}, semantic,
         "native_present_parameter_mismatch_rows"),
        ({"native_vrr_state_valid": 1}, semantic, "native_vrr_state_mismatch_rows"),
        ({"latch_raw_sync_qpc_valid": 1, "latch_raw_sync_qpc_ticks": 100,
          "latch_raw_sync_qpc_frequency_hz": 1000000}, semantic,
         "native_outcome_relationship_mismatch_rows"),
        ({"gpu_ready_signal_result_valid": 1}, semantic,
         "gpu_ready_native_result_relationship_mismatch_rows"),
        ({"gpu_ready_completion_lower_bound_us": 1}, timestamp,
         "gpu_ready_completion_bounds_derivation_mismatch_rows"),
    ]
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "result.json"

        def replay(path):
            output.unlink(missing_ok=True)
            result = subprocess.run([str(args.replay.resolve()), str(path),
                                     "--require-exact-baseline", "--output", str(output)],
                                    capture_output=True)
            assert output.exists(), result.stderr.decode()
            return result, json.loads(output.read_text())

        result, baseline = replay(args.fixture.resolve())
        assert result.returncode == 0, result.stderr.decode()
        assert baseline["fidelity"]["baseline_exact"]
        assert baseline["capture"]["recorded_sequence_integrity_valid"]
        outcomes = baseline["capture"]["telemetry_coverage"]["native_outcome_and_qpc_integrity"]
        assert outcomes["metal_present_attempt_rows"] == 40
        assert outcomes["native_backend_counts_decimal"] == {"4": 40}
        assert outcomes["relationship_mismatch_rows"] == 0
        # The Windows raster gate still requires DXGI evidence. A valid Metal
        # controller baseline must not manufacture that physical-scanout proof.
        assert not outcomes["valid"]

        failed_fixture = Path(str(args.fixture.resolve()) + ".failed")
        result, failed = replay(failed_fixture)
        assert result.returncode == 0, result.stderr.decode()
        assert failed["fidelity"]["baseline_exact"]
        assert failed["capture"]["recorded_sequence_integrity_valid"]
        assert failed["capture"]["presented_frames"] == 0
        failure_ops = failed["capture"]["telemetry_coverage"]["gpu_ready_native_operations"]
        assert failure_ops["wait_result_counts_decimal"] == {"2": 1}
        assert failure_ops["relationship_mismatch_rows"] == 0

        for changes, group, counter in cases:
            lines = list(original)
            fields = lines[selected].decode().strip().split(",")
            for key, value in changes.items():
                fields[columns.index(key)] = str(value)
            lines[selected] = (",".join(fields) + "\n").encode()
            footer_at = next(i for i, line in enumerate(lines)
                             if line.startswith(b"#vrr_trace_footer,"))
            body = b"".join(lines[:footer_at])
            footer = lines[footer_at].decode().strip().split(",")
            footer = [f"decoded_sha256={hashlib.sha256(body).hexdigest()}"
                      if part.startswith("decoded_sha256=") else part for part in footer]
            modified = Path(directory) / "modified.csv"
            modified.write_bytes(body + (",".join(footer) + "\n").encode())
            result, summary = replay(modified)
            assert result.returncode == 3 and not summary["fidelity"]["baseline_exact"], (
                changes, result.returncode, result.stderr.decode())
            assert summary["capture"][group][counter] > 0, (changes, group, counter)
    print(f"Exact Metal success/failure baselines and all {len(cases)} native-telemetry rejection checks passed")


if __name__ == "__main__":
    main()
