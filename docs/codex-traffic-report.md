# Codex token throughput report

Run from the repository root (Python 3.9 or newer; no packages required):

```bash
python3 scripts/codex-traffic-report.py --timezone America/Chicago
```

The default report covers the last 28 days, with hourly rows for the latest
24 hours, daily rows, and calendar weeks starting Monday. Each row separates
model, requested Fast/Standard mode, and reasoning effort. Hours include their
UTC offset so repeated hours at the end of daylight saving time stay separate.
Requests are assigned to the period in which they completed. Periods without
recorded traffic are omitted, and the first/current periods can be partial.

Examples:

```bash
# Keep a machine-readable copy alongside the terminal report.
python3 scripts/codex-traffic-report.py --timezone America/Chicago --json build/codex-traffic/latest.json

# Eight weeks of history, with hourly detail for the last four hours.
python3 scripts/codex-traffic-report.py --days 56 --hourly-hours 4

# Compare this model's Fast rows across time, keeping reasoning levels separate.
python3 scripts/codex-traffic-report.py --model gpt-6.1-sol --mode fast

# Point at another local Codex data directory.
python3 scripts/codex-traffic-report.py --codex-home /path/to/.codex
```

The script reads local Codex logs and rollouts without modifying them. It
archives token counts and timing metadata in
`build/codex-traffic/history.sqlite3` (ignored by Git). Use `--cache PATH` to
choose another archive. Keep a separate archive for each Codex home. Incremental
rollout offsets make repeat runs faster. No prompts, responses, tool arguments,
credentials, or raw log bodies are copied into the archive.

Run it periodically to retain timing history before Codex rotates its logs.
The first run cannot recover timing that was already deleted. Untimed requests
remain visible in coverage and token totals, but do not enter speed averages.
The report's earliest retained timing indicates how far the archive reaches;
it does not guarantee continuous coverage. Legacy `token_count` records are
supported for turns without structured usage records. When a turn has
structured records, those take precedence: compaction can make the legacy
cumulative counters disagree, so combining the formats would double-count.
Active, partially written records are picked up on the next run.

## Interpret the numbers

- **Input/s:** input tokens, including cached context, divided by request time.
- **Fresh/s:** uncached input tokens divided by the same request time.
- **Output/s:** all output tokens, including reasoning, divided by request time.
- **Cache% / In/req / Out/req:** workload context for comparing rows.
- **Timed/All:** requests with usable timing versus all recorded requests.

Rates use summed tokens divided by summed request durations, rather than an
unweighted average of individual rates. Concurrent requests each contribute
their duration: this measures average per-request throughput, not total
account tokens per wall-clock second. Timing begins at the logged Responses
request and ends at the structured usage record. Older clients only emit usage
after tool execution, so legacy timing can include tool time. Neither format
isolates server input processing, first-token latency, or generation-only speed.

Fast means the logged requested tier was `priority` or `fast`; Standard means
`default`. Missing/automatic settings are Unknown, and other tiers retain their
own label. These logs do not establish the actual served tier or whether a Fast
request was downgraded. Models and reasoning levels are kept separate, but
different context lengths, cache shares, output lengths, retries, network
conditions, and concurrency can still change the rates. A faster recent row
alone does not prove the underlying service accelerated.

Run the deterministic checks with:

```bash
python3 -m unittest discover -s scripts/tests -p 'test_codex_traffic_report.py'
```
