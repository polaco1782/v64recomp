#!/usr/bin/env python3
"""Summarize renderer callback timing from RECOMP_FRAME_TRACE (not GPU scanout)."""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import statistics


def distribution(values: list[float]) -> dict:
    if not values:
        return {"count": 0}
    values = sorted(values)
    return dict(count=len(values), median_ms=statistics.median(values),
                p95_ms=values[math.ceil(.95 * len(values)) - 1],
                p99_ms=values[math.ceil(.99 * len(values)) - 1], max_ms=values[-1])


def summarize(path: Path, start: float, end: float | None) -> dict:
    with path.open() as source:
        reader = csv.DictReader(source)
        if reader.fieldnames != ['timestamp_ns', 'event', 'value']:
            raise ValueError('expected a RECOMP_FRAME_TRACE CSV header')
        rows = sorted((int(r['timestamp_ns']), r['event'], int(r['value'])) for r in reader)
    if not rows:
        raise ValueError('trace has no events')
    origin = rows[0][0]
    finish = (rows[-1][0] - origin) / 1e9
    end = finish if end is None else min(end, finish)
    if not math.isfinite(start) or not math.isfinite(end) or not 0 <= start < end:
        raise ValueError('window must overlap the trace and satisfy 0 <= start < end')
    selected = [(t, e, v) for t, e, v in rows if start <= (t - origin) / 1e9 <= end]
    result = dict(start_seconds=start, end_seconds=end, timings={})
    for name, matches in (
        ('display_callback_interval', lambda e, v: e == 'display'),
        ('host_present_interval', lambda e, v: e == 'host_present'),
        ('graphics_fullsync_interval', lambda e, v: e == 'graphics_end' and v == 1),
    ):
        times = [t for t, e, v in selected if matches(e, v)]
        intervals = [(b - a) / 1e6 for a, b in zip(times, times[1:])]
        data = distribution(intervals)
        data['events'] = len(times)
        if intervals:
            data['events_per_second'] = (len(times) - 1) * 1e9 / (times[-1] - times[0])
            data['intervals_over_50_ms'] = sum(i > 50 for i in intervals)
        result['timings'][name] = data
    for name, begin, end_event in (
        ('graphics_job', 'graphics_begin', 'graphics_end'),
        ('present_job', 'present_begin', 'present_end'),
        ('graphics_receipt_delay', 'graphics_end', 'graphics_receipt'),
    ):
        pending, durations = None, []
        for t, event, _ in rows:
            if event == begin:
                pending = t
            elif event == end_event and pending is not None:
                if start <= (t - origin) / 1e9 <= end:
                    durations.append((t - pending) / 1e6)
                pending = None
        result['timings'][name] = distribution(durations)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--start', type=float, default=0, help='seconds after the first trace event')
    parser.add_argument('--end', type=float, help='seconds after the first trace event')
    parser.add_argument('--json', action='store_true', help='print machine-readable results')
    args = parser.parse_args()
    try:
        report = summarize(args.trace, args.start, args.end)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    if args.json:
        print(json.dumps(report, indent=2))
        return
    print(f"Window: {report['start_seconds']:.3f}..{report['end_seconds']:.3f}s; CPU callback timing, not GPU scanout")
    print(f"{'Timing':30} {'N':>6} {'median ms':>10} {'p95 ms':>10} {'p99 ms':>10} {'max ms':>10}")
    for name, data in report['timings'].items():
        if not data['count']:
            print(f'{name:30}      0')
            continue
        print(f"{name:30} {data['count']:6} {data['median_ms']:10.3f} {data['p95_ms']:10.3f} "
              f"{data['p99_ms']:10.3f} {data['max_ms']:10.3f}")
        if 'events_per_second' in data:
            print(f"  {data['events_per_second']:.2f} events/s; {data['intervals_over_50_ms']} intervals >50 ms")


if __name__ == '__main__':
    main()
