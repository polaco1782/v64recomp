#!/usr/bin/env python3
"""Capture Linux thread CPU usage and optional perf call stacks from a running game."""
from __future__ import annotations

import argparse
from collections import Counter
import csv
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
TICKS = os.sysconf('SC_CLK_TCK')


def stat(path: Path) -> dict:
    text = path.read_text()
    end = text.rindex(')')
    fields = text[end + 2:].split()  # fields[0] is proc stat field 3 (state).
    return {'tid': int(text.split(' ', 1)[0]),
            'name': text[text.index('(') + 1:end], 'state': fields[0],
            'user_ticks': int(fields[11]), 'system_ticks': int(fields[12]),
            'start_ticks': int(fields[19])}


def optional_text(path: Path) -> str | None:
    try:
        return path.read_text().strip()
    except OSError:
        return None


def snapshot(pid: int) -> list[dict]:
    result = []
    for task in (Path('/proc') / str(pid) / 'task').iterdir():
        try:
            row = stat(task / 'stat')
        except (FileNotFoundError, ProcessLookupError):
            continue  # Threads can exit while the directory is being scanned.
        row['wait_channel'] = optional_text(task / 'wchan')
        status = optional_text(task / 'status') or ''
        for key in ('voluntary_ctxt_switches', 'nonvoluntary_ctxt_switches'):
            value = next((line.split(':', 1)[1].strip() for line in status.splitlines()
                          if line.startswith(key + ':')), None)
            row[key] = int(value) if value is not None else None
        result.append(row)
    return result


def target(args) -> int:
    if args.pid is not None:
        pid = args.pid
    else:
        matches = []
        for entry in Path('/proc').iterdir():
            if not entry.name.isdecimal():
                continue
            try:
                if (entry / 'exe').readlink().name == args.game:
                    matches.append(int(entry.name))
            except OSError:
                continue
        if len(matches) != 1:
            raise ValueError(f'expected one running {args.game}, found {matches}; use --pid '
                             '(a sandbox may hide host processes)')
        pid = matches[0]
    stat(Path('/proc') / str(pid) / 'stat')
    return pid


def reports(directory: Path, tid: int | None = None) -> None:
    if not shutil.which('perf'):
        raise ValueError('perf is not on PATH; install the perf package matching your kernel')
    data = directory / 'perf.data'
    if not data.is_file() or not data.stat().st_size:
        raise ValueError(f'no perf recording at {data}; see perf-record.log and summary.json')
    base = ['perf', 'report', '--stdio', '--input', str(data), '--percent-limit', '0.5',
            '--field-separator', '|']
    if tid is not None:
        base += ['--tid', str(tid), '--percentage', 'relative']
    suffix = f'-{tid}' if tid is not None else ''
    variants = {
        f'perf-self{suffix}.txt': ['--no-children', '--call-graph', 'none',
                                   '--sort', 'comm,pid,dso,symbol'],
        f'perf-calltree{suffix}.txt': ['--children', '--sort', 'comm,pid,dso,symbol'],
    }
    errors = []
    for name, flags in variants.items():
        with (directory / name).open('w') as output:
            run = subprocess.run(base + flags, stdout=output, stderr=subprocess.PIPE, text=True)
        if run.returncode:
            errors.append(f'{name}: {run.stderr.strip()}')
    if errors:
        raise ValueError('; '.join(errors))


def capture(args) -> int:
    pid = target(args)
    process = Path('/proc') / str(pid)
    identity = stat(process / 'stat')['start_ticks']
    if args.tid is not None and not (process / 'task' / str(args.tid)).is_dir():
        raise ValueError(f'TID {args.tid} does not belong to PID {pid}')
    directory = args.output or ROOT / 'build' / 'profiles' / (
        datetime.now().strftime('%Y%m%d-%H%M%S-%f') + f'-{pid}')
    directory = directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    try:
        executable = str((process / 'exe').readlink())
    except OSError:
        executable = None
    metadata = {'format_version': 1, 'pid': pid, 'start_ticks': identity,
                'executable': executable, 'started_utc': datetime.now(timezone.utc).isoformat(),
                'clock_ticks_per_second': TICKS, 'requested_seconds': args.duration,
                'interval_seconds': args.interval, 'requested_backend': args.backend,
                'perf_tid': args.tid, 'perf_event': args.event,
                'perf_frequency': args.frequency, 'perf_call_graph': args.call_graph,
                'perf_event_paranoid': optional_text(Path('/proc/sys/kernel/perf_event_paranoid'))}
    if executable:
        cache = Path(executable).parent.parent / 'CMakeCache.txt'
        metadata['build_settings'] = [line for line in (optional_text(cache) or '').splitlines()
                                      if line.startswith(('CMAKE_BUILD_TYPE:', 'RECOMP_OPT_LEVEL:',
                                                          'RECOMP_RT64_BUILD_TYPE:'))]
    maps = optional_text(process / 'maps')
    if maps:
        (directory / 'maps.txt').write_text(maps + '\n')
    # Save identification even if the capture is interrupted or perf fails to start.
    (directory / 'summary.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(f'PID {pid}: {executable or "unknown executable"}\nCapture: {directory}', flush=True)
    if any(line == 'CMAKE_BUILD_TYPE:STRING=Debug' for line in metadata.get('build_settings', [])):
        print('Debug build: generated game code is unoptimized; compare with Release.', flush=True)
    recorder = None
    perf_log = None
    perf_error = None
    command = None
    first = {}
    last = {}
    states = {}
    waits = {}
    observations = Counter()
    start = time.monotonic()
    reason = 'duration'
    try:
        if args.backend != 'threads':
            if not shutil.which('perf'):
                perf_error = 'perf is not on PATH'
            else:
                command = ['perf', 'record', '--event', args.event,
                           '--freq', str(args.frequency), '--call-graph', args.call_graph,
                           '--output', str(directory / 'perf.data'),
                           '--no-buildid-cache', '--timestamp']
                command += (['--tid', str(args.tid)] if args.tid is not None
                            else ['--pid', str(pid)])
                command += ['--', 'sleep', str(args.duration)]
                perf_log = (directory / 'perf-record.log').open('w')
                try:
                    recorder = subprocess.Popen(command, stdout=perf_log, stderr=perf_log,
                                                start_new_session=True)
                except OSError as error:
                    perf_error = str(error)
            if perf_error:
                print(f'{perf_error}; collecting thread CPU usage only.', flush=True)
        with (directory / 'threads.jsonl').open('w') as timeline:
            while True:
                try:
                    current = stat(process / 'stat')
                    if current['start_ticks'] != identity or current['state'] == 'Z':
                        reason = 'process exited or PID reused'
                        break
                    rows = snapshot(pid)
                except (FileNotFoundError, ProcessLookupError):
                    reason = 'process exited'
                    break
                elapsed = time.monotonic() - start
                for row in rows:
                    key = (row['tid'], row['start_ticks'])
                    first.setdefault(key, row.copy())
                    last[key] = row
                    observations[key] += 1
                    states.setdefault(key, Counter())[row['state']] += 1
                    waits.setdefault(key, Counter())[row['wait_channel'] or 'unavailable'] += 1
                timeline.write(json.dumps({'elapsed_seconds': elapsed, 'threads': rows}) + '\n')
                timeline.flush()
                if recorder is not None and recorder.poll() not in (None, 0) and not perf_error:
                    perf_error = f'perf exited with status {recorder.returncode}; see perf-record.log'
                    print(f'{perf_error}; continuing thread CPU accounting.', flush=True)
                if elapsed >= args.duration:
                    break
                time.sleep(min(args.interval, args.duration - elapsed))
    except KeyboardInterrupt:
        reason = 'interrupted'
    finally:
        elapsed = time.monotonic() - start
        if recorder is not None:
            # Let perf's duration command exit normally at the deadline; sending
            # SIGINT during its teardown can turn a valid capture into SIGTERM.
            stopped_by_us = recorder.poll() is None and reason != 'duration'
            if stopped_by_us:
                recorder.send_signal(signal.SIGINT)
            try:
                recorder.wait(timeout=10)
            except subprocess.TimeoutExpired:
                recorder.kill()
                recorder.wait()
                perf_error = 'perf did not finish within 10 seconds'
            if recorder.returncode and not perf_error and not (
                    stopped_by_us and recorder.returncode in (-signal.SIGINT, -signal.SIGTERM)):
                perf_error = f'perf exited with status {recorder.returncode}; see perf-record.log'
        if perf_log:
            perf_log.close()
        totals = []
        for key, row in last.items():
            initial = first[key]
            user = max(0, row['user_ticks'] - initial['user_ticks']) / TICKS
            system = max(0, row['system_ticks'] - initial['system_ticks']) / TICKS
            item = {'tid': row['tid'], 'name': row['name'], 'start_ticks': row['start_ticks'],
                    'user_seconds': user, 'system_seconds': system,
                    'cpu_percent': 100 * (user + system) / elapsed,
                    'observations': observations[key], 'state_observations': dict(states[key]),
                    'wait_channel_observations': dict(waits[key])}
            for field in ('voluntary_ctxt_switches', 'nonvoluntary_ctxt_switches'):
                item[field] = (row[field] - initial[field]
                               if row[field] is not None and initial[field] is not None else None)
            totals.append(item)
        totals.sort(key=lambda row: row['cpu_percent'], reverse=True)
        metadata.update(elapsed_seconds=elapsed, stop_reason=reason, threads=totals,
                        total_cpu_percent=sum(row['cpu_percent'] for row in totals),
                        perf_command=command, perf_error=perf_error,
                        perf_returncode=recorder.returncode if recorder else None,
                        perf_available=recorder is not None and perf_error is None)
        (directory / 'summary.json').write_text(json.dumps(metadata, indent=2) + '\n')
        columns = ['tid', 'name', 'cpu_percent', 'user_seconds', 'system_seconds',
                   'voluntary_ctxt_switches', 'nonvoluntary_ctxt_switches']
        with (directory / 'threads.csv').open('w', newline='') as output:
            writer = csv.DictWriter(output, fieldnames=columns, extrasaction='ignore')
            writer.writeheader()
            writer.writerows(totals)
        print(f'\n{elapsed:.2f}s; CPU percentages use one logical CPU = 100%.')
        print('     TID  CPU %   user s  system s  voluntary/s  involuntary/s  name')
        for row in totals[:args.top]:
            voluntary = row['voluntary_ctxt_switches']
            involuntary = row['nonvoluntary_ctxt_switches']
            vs = f'{voluntary / elapsed:.1f}' if voluntary is not None else '?'
            invs = f'{involuntary / elapsed:.1f}' if involuntary is not None else '?'
            print(f"{row['tid']:8d} {row['cpu_percent']:6.1f} {row['user_seconds']:8.2f} "
                  f"{row['system_seconds']:9.2f} {vs:>12} {invs:>14}  {row['name']}")
        print(f"Total observed thread CPU: {metadata['total_cpu_percent']:.1f}%")
    if metadata['perf_available']:
        reports(directory)
        print('Function reports: perf-self.txt, perf-calltree.txt')
    elif args.backend != 'threads':
        print('No function profile captured. See summary.json and perf-record.log; '
              'thread accounting is still available.')
    return 1 if args.backend == 'perf' and not metadata['perf_available'] else 0


def positive(value: str) -> float:
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError('must be a finite positive number')
    return number


def positive_int(value: str) -> int:
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError('must be a positive integer')
    return number


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    for action in ('threads', 'record'):
        command = commands.add_parser(action, help='thread CPU accounting' if action == 'threads'
                                      else 'thread CPU accounting plus perf function sampling')
        select = command.add_mutually_exclusive_group(required=True)
        select.add_argument('--pid', type=positive_int)
        select.add_argument('--game', help='exact executable basename; requires one matching process')
        command.add_argument('--duration', type=positive, default=30)
        command.add_argument('--interval', type=positive, default=1)
        command.add_argument('--output', type=Path, help='new output directory (must not exist)')
        command.add_argument('--top', type=positive_int, default=15)
        command.set_defaults(backend='threads', tid=None, event='cpu-clock:u', frequency=199,
                             call_graph='dwarf,16384')
        if action == 'record':
            command.add_argument('--backend', choices=('auto', 'perf', 'threads'), default='auto',
                                 help='auto keeps thread accounting if perf fails; perf fails explicitly')
            command.add_argument('--tid', type=positive_int, help='sample only this native TID with perf')
            command.add_argument('--event', default='cpu-clock:u', help='perf event (default: userspace CPU time)')
            command.add_argument('--frequency', type=positive_int, default=199)
            command.add_argument('--call-graph', default='dwarf,16384', help='perf unwind mode, e.g. fp or dwarf,16384')
    report = commands.add_parser('report', help='write Self/call-tree reports from an existing capture')
    report.add_argument('directory', type=Path)
    report.add_argument('--tid', type=positive_int, help='filter reports to one native thread')
    args = parser.parse_args()
    try:
        if args.action == 'report':
            reports(args.directory, args.tid)
            return 0
        return capture(args)
    except (OSError, ValueError) as error:
        print(f'profile_execution: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
