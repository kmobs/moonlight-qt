#!/usr/bin/env python3
"""Report local Codex request throughput. Python 3.9+, standard library only."""

import argparse
import bisect
import collections
import datetime as dt
import json
import os
from pathlib import Path
import re
import sqlite3
import sys
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

UTC = dt.timezone.utc
FIELDS = ("input_tokens", "cached_input_tokens", "output_tokens", "reasoning_output_tokens")
THREAD = re.compile(r"thread_id=([^ }]+)")
TURN = re.compile(r"turn.id=([^ }]+)")
MODEL = re.compile(r" model=([^ }]+)")
EFFORT = re.compile(r"codex.turn.reasoning_effort=([^ }]+)")
TIER = re.compile(r'"service_tier":"([^"]+)"')


def read_db(path):
    return sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True, timeout=15)


def open_cache(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(path, timeout=30)
    db.executescript("""
        CREATE TABLE IF NOT EXISTS sources (
            path TEXT PRIMARY KEY, offset INTEGER, thread TEXT, turn TEXT);
        CREATE TABLE IF NOT EXISTS events (
            thread TEXT, turn TEXT, time REAL, kind TEXT,
            model TEXT, tier TEXT, effort TEXT,
            PRIMARY KEY(thread, turn, time, kind));
        CREATE TABLE IF NOT EXISTS usage (
            usage_key TEXT PRIMARY KEY, thread TEXT, turn TEXT, end REAL,
            input_tokens INTEGER, cached_input_tokens INTEGER,
            output_tokens INTEGER, reasoning_output_tokens INTEGER,
            structured INTEGER);
        CREATE INDEX IF NOT EXISTS usage_turn_format ON usage(thread, turn, structured);
    """)
    return db


def ingest_logs(home, cache):
    paths = sorted(home.glob("logs_*.sqlite"))
    if not paths:
        return ["No Codex timing log database found; rates require retained timing logs."]
    warnings = []
    for path in paths:
        try:
            with read_db(path) as db:
                query = """SELECT ts, ts_nanos, feedback_log_body FROM logs
                    WHERE target='feedback_tags' AND
                    (feedback_log_body LIKE '%tags_json=%' OR
                     feedback_log_body LIKE '%endpoint="/responses"%')
                    ORDER BY ts, ts_nanos"""
                for seconds, nanos, body in db.execute(query):
                    thread, turn = THREAD.search(body), TURN.search(body)
                    if not thread or not turn:
                        continue
                    kind = "metadata" if "tags_json=" in body else "start"
                    model, tier, effort = MODEL.search(body), TIER.search(body), EFFORT.search(body)
                    cache.execute("INSERT OR IGNORE INTO events VALUES (?,?,?,?,?,?,?)",
                                  (thread[1], turn[1], seconds + nanos / 1e9, kind,
                                   model[1] if model else "unknown",
                                   tier[1] if tier else "unknown",
                                   effort[1] if effort else "unknown"))
        except sqlite3.Error as exc:
            warnings.append(f"Could not read {path.name}: {exc}")
    return warnings


def rollout_paths(home):
    paths = set((home / "sessions").rglob("*.jsonl"))
    # Include registered archived/subagent rollouts outside the standard tree.
    for path in home.glob("state_*.sqlite"):
        try:
            with read_db(path) as db:
                paths.update(Path(row[0]) for row in db.execute("SELECT rollout_path FROM threads")
                             if row[0] and Path(row[0]).is_file())
        except sqlite3.Error:
            pass
    return sorted(paths)


def ingest_rollouts(home, cache):
    malformed = 0
    markers = (b'"session_meta"', b'"task_started"', b'"turn_context"',
               b'"token_usage_record"', b'"token_count"')
    for path in rollout_paths(home):
        old = cache.execute("SELECT offset,thread,turn FROM sources WHERE path=?",
                            (str(path),)).fetchone()
        offset, thread, turn = old or (0, "", "")
        if path.stat().st_size < offset:
            offset, thread, turn = 0, "", ""
        with path.open("rb") as file:
            file.seek(offset)
            while True:
                start = file.tell()
                line = file.readline()
                if not line or not line.endswith(b"\n"):
                    # An active writer may still be appending the final record.
                    file.seek(start)
                    break
                if not any(marker in line for marker in markers):
                    continue
                try:
                    record = json.loads(line)
                    payload = record.get("payload", {})
                    kind = record.get("type")
                    if kind == "session_meta":
                        thread = payload.get("id", thread)
                    elif kind == "turn_context":
                        turn = payload.get("turn_id", turn)
                    elif kind == "event_msg" and payload.get("type") == "task_started":
                        turn = payload.get("turn_id", turn)
                    else:
                        structured = kind == "token_usage_record"
                        info = payload.get("info") or {}
                        if structured:
                            counts = payload.get("usage")
                            total = payload.get("thread_token_usage")
                            current_thread = payload.get("thread_id", thread)
                            current_turn = payload.get("turn_id", turn)
                        elif kind == "event_msg" and payload.get("type") == "token_count":
                            counts = info.get("last_token_usage")
                            total = info.get("total_token_usage")
                            current_thread, current_turn = thread, turn
                        else:
                            continue
                        if not counts or not total or not current_thread or not current_turn:
                            continue
                        # Both record formats describe the same request. Repeated
                        # token_count updates (e.g. rate-limit updates) deduplicate.
                        key = json.dumps([current_thread, current_turn,
                                          total.get("input_tokens"), total.get("output_tokens")])
                        end = dt.datetime.fromisoformat(record["timestamp"].replace("Z", "+00:00")).timestamp()
                        values = tuple(int(counts.get(name, 0)) for name in FIELDS)
                        cache.execute("""INSERT INTO usage VALUES (?,?,?,?,?,?,?,?,?)
                            ON CONFLICT(usage_key) DO UPDATE SET
                            end=excluded.end, structured=excluded.structured
                            WHERE excluded.structured > usage.structured""",
                                      (key, current_thread, current_turn, end, *values, int(structured)))
                except (ValueError, TypeError, KeyError):
                    malformed += 1
            offset = file.tell()
        cache.execute("INSERT OR REPLACE INTO sources VALUES (?,?,?,?)",
                      (str(path), offset, thread, turn))
    return malformed


def mode(tier):
    if tier in ("fast", "priority"):
        return "Fast"
    if tier == "default":
        return "Standard"
    return "Unknown" if tier in ("unknown", "unset", "auto") else tier


def requests(cache, since, until):
    events = collections.defaultdict(lambda: collections.defaultdict(list))
    for thread, turn, time, kind, model, tier, effort in cache.execute(
            "SELECT * FROM events ORDER BY time"):
        events[(thread, turn)][kind].append((time, model, tier, effort))
    index = {key: {kind: [event[0] for event in values] for kind, values in groups.items()}
             for key, groups in events.items()}
    rows = []
    reused = set()
    for key, thread, turn, end, *tail in cache.execute(
            """SELECT * FROM usage AS u WHERE end>=? AND end<? AND
               (structured=1 OR NOT EXISTS (
                   SELECT 1 FROM usage AS s WHERE s.thread=u.thread
                   AND s.turn=u.turn AND s.structured=1)) ORDER BY end""", (since, until)):
        counts, structured = tail[:4], tail[4]
        row = dict(zip(FIELDS, counts), end=end, seconds=None, model="unknown",
                   mode="Unknown", requested_tier="unknown", effort="unknown",
                   timing_status="missing", record_format="structured" if structured else "legacy")
        groups = events.get((thread, turn), {})
        times = index.get((thread, turn), {})
        pos = bisect.bisect_right(times.get("start", []), end) - 1
        if pos >= 0:
            start, model, _, effort = groups["start"][pos]
            row.update(model=model, effort=effort)
            meta_pos = bisect.bisect_right(times.get("metadata", []), start) - 1
            if meta_pos >= 0:
                _, _, tier, effort = groups["metadata"][meta_pos]
                row.update(requested_tier=tier, mode=mode(tier), effort=effort)
            event_key = (thread, turn, start)
            if event_key in reused:
                row["timing_status"] = "ambiguous"
            elif end > start:
                row.update(seconds=end - start, timing_status="matched")
                reused.add(event_key)
        rows.append(row)
    return rows


def summarize(rows):
    timed = [row for row in rows if row["seconds"] is not None]
    seconds = sum(row["seconds"] for row in timed)
    tokens = {name: sum(row[name] for row in timed) for name in FIELDS}
    totals = {name: sum(row[name] for row in rows) for name in FIELDS}
    rate = lambda count: count / seconds if seconds else None
    return dict(requests=len(rows), timed_requests=len(timed),
                untimed_requests=len(rows) - len(timed), request_seconds=seconds,
                timed_tokens=tokens, all_recorded_tokens=totals,
                input_tps=rate(tokens["input_tokens"]),
                uncached_input_tps=rate(tokens["input_tokens"] - tokens["cached_input_tokens"]),
                output_tps=rate(tokens["output_tokens"]),
                cache_percent=(100 * tokens["cached_input_tokens"] / tokens["input_tokens"]
                               if tokens["input_tokens"] else None),
                mean_input_tokens=(tokens["input_tokens"] / len(timed) if timed else None),
                mean_output_tokens=(tokens["output_tokens"] / len(timed) if timed else None))


def bucket(timestamp, period, timezone):
    date = dt.datetime.fromtimestamp(timestamp, timezone)
    if period == "hour":
        return date.strftime("%Y-%m-%d %H:00 %z")
    if period == "day":
        return date.date().isoformat()
    monday = date.date() - dt.timedelta(days=date.weekday())
    return monday.isoformat()


def grouped(rows, period, timezone):
    groups = collections.defaultdict(list)
    for row in rows:
        label = "All" if period == "total" else bucket(row["end"], period, timezone)
        groups[(label, row["model"], row["mode"], row["effort"])].append(row)
    return [dict(period=label, model=model, mode=mode_name, effort=effort, **summarize(values))
            for (label, model, mode_name, effort), values in sorted(groups.items())]


def local_timezone():
    name = os.environ.get("TZ")
    if name:
        return ZoneInfo(name)
    try:
        with Path("/etc/localtime").open("rb") as file:
            return ZoneInfo.from_file(file, key="local")
    except OSError:
        return UTC


def print_report(report):
    print(f"Codex token throughput — {report['timezone']}")
    print(f"Window: {report['since']} to {report['until']}")
    print("Rates = summed tokens / summed request seconds; includes request latency/reasoning.")
    print("Input includes cache; Fresh input excludes cache. Output includes reasoning tokens.")
    print("Fast/Standard = logged requested tier; actual served tier is not available.")
    print("Models and reasoning efforts are separated. Workload differences can affect rates.")
    coverage = report["coverage"]
    print(f"Coverage: {coverage['timed_requests']}/{coverage['requests']} requests timed; "
          f"{coverage['untimed_requests']} excluded from speed averages.")
    if report["earliest_timing"]:
        print(f"Earliest retained request timing: {report['earliest_timing']}")
    for warning in report["warnings"]:
        print(f"Warning: {warning}")
    for name, entries in report["tables"].items():
        print(f"\n{name} (only periods with recorded traffic; edge periods may be partial)")
        print(f"{'Period':<23} {'Model':<19} {'Mode':<9} {'Effort':<8} "
              f"{'Timed/All':>10} {'Input/s':>10} {'Fresh/s':>10} {'Output/s':>10} "
              f"{'Cache%':>7} {'In/req':>10} {'Out/req':>9}")
        for entry in entries:
            def fmt(key, decimals=1):
                return f"{entry[key]:,.{decimals}f}" if entry[key] is not None else "—"
            count = f"{entry['timed_requests']}/{entry['requests']}"
            print(f"{entry['period']:<23} {entry['model']:<19} {entry['mode']:<9} "
                  f"{entry['effort']:<8} {count:>10} {fmt('input_tps'):>10} "
                  f"{fmt('uncached_input_tps'):>10} {fmt('output_tps'):>10} "
                  f"{fmt('cache_percent'):>7} {fmt('mean_input_tokens', 0):>10} "
                  f"{fmt('mean_output_tokens', 0):>9}")
        if not entries:
            print("No recorded traffic in this period.")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codex-home", type=Path, default=Path(os.environ.get("CODEX_HOME", Path.home() / ".codex")))
    parser.add_argument("--cache", type=Path, default=Path(__file__).resolve().parents[1] / "build/codex-traffic/history.sqlite3",
                        help="Persistent metadata archive (default: build/codex-traffic/history.sqlite3)")
    parser.add_argument("--days", type=int, default=28, help="Rolling report window (default: 28 days)")
    parser.add_argument("--hourly-hours", type=int, default=24, help="Show hourly rows for the latest N hours")
    parser.add_argument("--timezone", help="IANA timezone, e.g. America/Chicago (default: system timezone)")
    parser.add_argument("--model", help="Filter to one exact model")
    parser.add_argument("--mode", choices=["fast", "standard", "unknown"], help="Filter by requested mode")
    parser.add_argument("--json", type=Path, help="Also save the report as JSON")
    args = parser.parse_args(argv)
    if args.days <= 0 or args.hourly_hours <= 0:
        parser.error("--days and --hourly-hours must be positive")
    home = args.codex_home.expanduser().resolve()
    if not home.is_dir():
        parser.error(f"Codex home does not exist: {home}")
    try:
        timezone = ZoneInfo(args.timezone) if args.timezone else local_timezone()
    except ZoneInfoNotFoundError as exc:
        parser.error(str(exc))
    until = dt.datetime.now(UTC).timestamp()
    since = until - args.days * 86400
    with open_cache(args.cache.expanduser()) as cache:
        warnings = ingest_logs(home, cache)
        malformed = ingest_rollouts(home, cache)
        if malformed:
            warnings.append(f"Skipped {malformed} malformed records.")
        rows = requests(cache, since, until)
        earliest = cache.execute("SELECT MIN(time) FROM events WHERE kind='start'").fetchone()[0]
    if args.model:
        rows = [row for row in rows if row["model"] == args.model]
    if args.mode:
        rows = [row for row in rows if row["mode"].lower() == args.mode]
    stamp = lambda value: dt.datetime.fromtimestamp(value, timezone).isoformat(timespec="seconds")
    report = dict(schema_version=1, timezone=str(timezone), since=stamp(since), until=stamp(until),
                  cache=str(args.cache.resolve()), earliest_timing=stamp(earliest) if earliest else None,
                  mode_basis="requested_tier_not_confirmed_served_tier",
                  rate_basis="sum_tokens_divided_by_sum_request_seconds",
                  coverage=summarize(rows), warnings=warnings,
                  tables={"Overall": grouped(rows, "total", timezone),
                          "Hourly": grouped([row for row in rows if row["end"] >= until - args.hourly_hours * 3600], "hour", timezone),
                          "Daily": grouped(rows, "day", timezone),
                          "Weekly (Monday start)": grouped(rows, "week", timezone)})
    print_report(report)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, sqlite3.Error, ValueError) as exc:
        sys.exit(f"codex-traffic-report: {exc}")
