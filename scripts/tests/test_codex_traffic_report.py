import datetime as dt
import importlib.util
import json
from pathlib import Path
import tempfile
import sys
import unittest
from zoneinfo import ZoneInfo

spec = importlib.util.spec_from_file_location(
    "traffic", Path(__file__).resolve().parents[1] / "codex-traffic-report.py")
traffic = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
spec.loader.exec_module(traffic)


class TrafficReportTests(unittest.TestCase):
    def test_weighted_rates_exclude_missing_timing(self):
        rows = [dict(seconds=1, input_tokens=100, cached_input_tokens=80,
                     output_tokens=10, reasoning_output_tokens=3),
                dict(seconds=9, input_tokens=200, cached_input_tokens=100,
                     output_tokens=90, reasoning_output_tokens=20),
                dict(seconds=None, input_tokens=9999, cached_input_tokens=0,
                     output_tokens=9999, reasoning_output_tokens=0)]
        summary = traffic.summarize(rows)
        self.assertEqual(summary["output_tps"], 10)
        self.assertEqual(summary["input_tps"], 30)
        self.assertEqual(summary["uncached_input_tps"], 12)
        self.assertEqual(summary["untimed_requests"], 1)
        self.assertEqual(summary["all_recorded_tokens"]["output_tokens"], 10099)

    def test_calendar_buckets_and_dst_repeated_hour(self):
        timezone = ZoneInfo("America/Chicago")
        first = dt.datetime(2026, 11, 1, 6, 30, tzinfo=dt.timezone.utc).timestamp()
        second = first + 3600
        self.assertNotEqual(traffic.bucket(first, "hour", timezone),
                            traffic.bucket(second, "hour", timezone))
        self.assertEqual(traffic.bucket(first, "day", timezone), "2026-11-01")
        self.assertEqual(traffic.bucket(first, "week", timezone), "2026-10-26")

    def test_incremental_dedup_and_partial_record(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            sessions = home / "sessions"
            sessions.mkdir()
            path = sessions / "sample.jsonl"
            counts = dict(input_tokens=100, cached_input_tokens=80,
                          output_tokens=10, reasoning_output_tokens=2)
            timestamp = "2026-09-30T12:00:10Z"
            records = [dict(type="session_meta", payload=dict(id="thread")),
                       dict(type="event_msg", payload=dict(type="task_started", turn_id="turn")),
                       dict(type="event_msg", timestamp=timestamp,
                            payload=dict(type="token_count", info=dict(
                                total_token_usage=counts, last_token_usage=counts))),
                       dict(type="token_usage_record", timestamp="2026-09-30T12:00:09Z",
                            payload=dict(thread_id="thread", turn_id="turn",
                                         usage=counts, thread_token_usage=counts))]
            path.write_text("\n".join(json.dumps(record) for record in records) + "\n")
            with traffic.open_cache(home / "cache.sqlite") as cache:
                traffic.ingest_rollouts(home, cache)
                self.assertEqual(cache.execute("SELECT count(*) FROM usage").fetchone()[0], 1)
                self.assertEqual(cache.execute("SELECT structured FROM usage").fetchone()[0], 1)
                traffic.ingest_rollouts(home, cache)
                self.assertEqual(cache.execute("SELECT count(*) FROM usage").fetchone()[0], 1)
                next_counts = dict(counts, input_tokens=200, output_tokens=20)
                record = dict(type="token_usage_record", timestamp="2026-09-30T12:00:20Z",
                              payload=dict(thread_id="thread", turn_id="turn", usage=counts,
                                           thread_token_usage=next_counts))
                with path.open("a") as file:
                    file.write(json.dumps(record))
                traffic.ingest_rollouts(home, cache)
                self.assertEqual(cache.execute("SELECT count(*) FROM usage").fetchone()[0], 1)
                with path.open("a") as file:
                    file.write("\n")
                traffic.ingest_rollouts(home, cache)
                self.assertEqual(cache.execute("SELECT count(*) FROM usage").fetchone()[0], 2)

    def test_mode_at_request_start_and_ambiguous_timing(self):
        with tempfile.TemporaryDirectory() as directory:
            with traffic.open_cache(Path(directory) / "cache.sqlite") as cache:
                cache.executemany("INSERT INTO events VALUES (?,?,?,?,?,?,?)", [
                    ("t", "u", 99, "metadata", "model", "priority", "high"),
                    ("t", "u", 100, "start", "model", "unknown", "high"),
                    ("t", "u", 105, "metadata", "model", "default", "medium")])
                cache.executemany("INSERT INTO usage VALUES (?,?,?,?,?,?,?,?,?)", [
                    ("one", "t", "u", 110, 100, 80, 10, 2, 1),
                    ("two", "t", "u", 120, 100, 80, 10, 2, 1)])
                rows = traffic.requests(cache, 0, 200)
                self.assertEqual(rows[0]["mode"], "Fast")
                self.assertEqual(rows[0]["effort"], "high")
                self.assertEqual(rows[0]["seconds"], 10)
                self.assertIsNone(rows[1]["seconds"])
                self.assertEqual(rows[1]["timing_status"], "ambiguous")

    def test_models_modes_and_efforts_never_merge(self):
        rows = [dict(end=100, model="m", mode=mode, effort=effort,
                     seconds=1, input_tokens=100, cached_input_tokens=80,
                     output_tokens=10, reasoning_output_tokens=2)
                for mode, effort in [("Fast", "high"), ("Standard", "high"), ("Fast", "max")]]
        self.assertEqual(len(traffic.grouped(rows, "total", dt.timezone.utc)), 3)
        self.assertEqual(traffic.mode("unset"), "Unknown")
        self.assertEqual(traffic.mode("priority"), "Fast")

    def test_structured_turn_suppresses_legacy_counter_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            with traffic.open_cache(Path(directory) / "cache.sqlite") as cache:
                cache.executemany("INSERT INTO usage VALUES (?,?,?,?,?,?,?,?,?)", [
                    ("one", "t", "u", 110, 100, 80, 10, 2, 1),
                    ("two", "t", "u", 120, 100, 80, 10, 2, 0),
                    ("three", "t", "old", 80, 100, 80, 10, 2, 0)])
                rows = traffic.requests(cache, 0, 200)
                self.assertEqual(len(rows), 2)
                self.assertEqual(sum(row["record_format"] == "legacy" for row in rows), 1)


if __name__ == "__main__":
    unittest.main()
