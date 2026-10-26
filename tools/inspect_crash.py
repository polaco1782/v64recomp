#!/usr/bin/env python3
"""Show a native game crash or warning report and resolve its compiled C source locations."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


def source_location(frame: dict) -> tuple[str,str]:
    module=frame.get('module')
    if not module or not Path(module).is_file():return frame.get('symbol') or '?','?'
    with Path(module).open('rb') as stream:header=stream.read(18)
    if header[:4]!=b'\x7fELF':return frame.get('symbol') or '?','?'
    kind=int.from_bytes(header[16:18],'little' if header[5]==1 else 'big')
    address=int(frame['address'],16)-1
    if kind==3:address-=int(frame['module_base'],16)
    result=subprocess.run(['addr2line','-f','-C','-e',module,hex(address)],capture_output=True,text=True,check=True)
    lines=result.stdout.splitlines()
    return (lines[0],lines[1]) if len(lines)>=2 else (frame.get('symbol') or '?','?')


def guest_location(location: str) -> str | None:
    match=re.match(r'(.+(?:functions|cpu-[^/]+)\.c):(\d+)',location)
    if not match:return None
    path,line=Path(match[1]),int(match[2])
    if not path.is_file():return None
    last=None
    with path.open() as stream:
        for number,text in enumerate(stream,1):
            if number>line:break
            instruction=re.search(r'/\* ([0-9a-f]{8}): (.*?) \*/',text)
            if instruction:last=f'0x{instruction[1]}: {instruction[2]}'
            elif text.startswith('void recomp_'):last=None
    return last


def inspect_crash(path: Path, resolve: bool = True) -> dict:
    report=json.loads(path.read_text())
    if report.get('format_version')!=1:raise ValueError('unsupported crash report format')
    failure=report['failure'];game=report['game']
    width=f"; requested {failure['requested_bytes']} bytes" if failure['ram_range'] else ''
    severity=failure.get('severity','fatal')
    print(f"{severity.capitalize()}: {failure['message']}: {failure['address']}{width}")
    if 'level' in game:
        print(f"Level {game['level']}, area {game['area']}, action {game['action']}; {game['play_frames']} playable frames")
    else:print(game.get('name','Native game'))
    for thread in report['threads']:
        if thread['current']:
            print(f"Guest thread {thread['id']} ({thread['state']}, priority {thread['priority']}); last tracked call/FP location {thread['last_tracked_pc']}")
            print(f"Guest stack {thread['gpr'][29]}, return address {thread['gpr'][31]}")
    task=report.get('task')
    if task:print(f"SP task {task['token']} at {task['object']}: type {task['descriptor'][0]}, list {task['descriptor'][12]}, bytes {int(task['descriptor'][13],16)}")
    elif len(report.get('task_words',[]))==16:
        words=report['task_words'];print(f"SP task: type {words[0]}, list 0x{words[12]:08x}, bytes {words[13]}")
    if report.get('pager'):
        p=report['pager'];print(f"ROM pages: {p['loads']} loads, {p['evictions']} evictions, {p['calls']} paged calls")
    audio=report.get('audio')
    if audio and audio['history']:
        command=audio['history'][-1]
        print(f"Last audio command #{audio['command_count']} at {command['address']}: {command['w1']} {command['w2']}")
    print('Native C stack:')
    for frame in report['native_stack']:
        if not resolve:
            print(f"  {frame.get('symbol') or '?'} at {frame['address']} ({frame.get('module') or '?'})")
            continue
        symbol,location=source_location(frame)
        print(f"  {symbol} at {location}")
        guest=guest_location(location)
        if guest:print(f'    ROM instruction near this compiled location: {guest}')
    print(f"RAM snapshot ({report['rdram']['storage']}): {report['rdram']['path']}")
    return report


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('report',type=Path,nargs='?')
    parser.add_argument('--no-resolve', action='store_true', help='print recorded frames without using possibly rebuilt binaries or sources')
    args=parser.parse_args();path=args.report
    if path is None:
        directory=Path(__file__).resolve().parents[1]
        reports=[p for base in (directory/'local/crashes',directory/'build') for pattern in ('crash-*.json','warning-*.json','*.crash.json') for p in base.rglob(pattern)]
        if not reports:raise ValueError(f'no crash reports in {directory}')
        path=max(reports,key=lambda p:p.stat().st_mtime_ns)
    inspect_crash(path, resolve=not args.no_resolve);return 0


if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,KeyError,subprocess.SubprocessError) as error:
        print(f'error: {error}',file=sys.stderr);sys.exit(1)
