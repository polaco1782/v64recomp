#!/usr/bin/env python3
"""Try a checked live repair, optionally asking Codex to investigate ambiguity.

The game launches this helper after pausing. A failed or uncertain attempt leaves
the game waiting; only a successfully published module lets it resume.
"""
from __future__ import annotations

import argparse
from contextlib import redirect_stderr, redirect_stdout
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from build_live_function import build
from common import RecompError
from generate_game import GAMES, PROJECT


def handoff(game: str, address: int, site: int, section: int, kind: str, build_dir: Path,
            patch_dir: Path, reason: str, report: Path) -> bool:
    codex = shutil.which('codex')
    if not codex:
        print('live repair: Codex CLI is unavailable; manual review needed', file=sys.stderr)
        return False
    command = (f'.venv/bin/python tools/build_live_function.py --game {game} '
               f'--address 0x{address:08x} --build-dir {build_dir} --patch-dir {patch_dir}')
    if kind != 'missing_function':
        command += f' --site 0x{site:08x} --site-section {section}'
    prompt = (f'Follow AGENTS.md in {PROJECT}. A game is paused at guest target '
              f'0x{address:08x} (kind {kind}, site 0x{site:08x}). The safe automatic '
              f'live builder stopped: {reason}. Inspect the pinned ROM, current '
              f'generated metadata, the review report at {report}, and the caller or checked '
              f'table. If the target is verified guest code, make the smallest '
              f'checked profile or entry-inventory change, then run exactly the '
              f'appropriate live builder command, based on: {command}. Publish only '
              f'a module that passes the builder checks. The running game will '
              f'resume on its own. If evidence is ambiguous, leave it paused and '
              f'explain what needs review. Do not modify the original ROM.')
    control = patch_dir / f'function-{address:08x}.path'
    before = control.read_text() if control.exists() else None
    transcript = patch_dir / f'ai-{address:08x}.log'
    answer = patch_dir / f'ai-{address:08x}.md'
    print(f'live repair: asking Codex to investigate; log {transcript}', file=sys.stderr)
    with transcript.open('w') as log:
        result = subprocess.run([codex, 'exec', '--approve-for-me',
                                 '-C', str(PROJECT),
                                 '--output-last-message', str(answer), prompt],
                                stdout=log, stderr=subprocess.STDOUT, check=False)
    if result.returncode:
        print(f'live repair: Codex exited {result.returncode}; game remains paused', file=sys.stderr)
        return False
    return control.exists() and control.read_text() != before


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', choices=GAMES, required=True)
    parser.add_argument('--address', type=lambda value: int(value, 0), required=True)
    parser.add_argument('--site', type=lambda value: int(value, 0), default=0)
    parser.add_argument('--section', type=int, default=0)
    parser.add_argument('--kind', choices=('missing_function', 'call', 'jump'), default='missing_function')
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--patch-dir', type=Path)
    parser.add_argument('--ai', action='store_true', help='ask Codex to investigate if safe automation stops')
    args = parser.parse_args()
    if args.address < 0 or args.address > 0xffffffff or args.address % 4:
        parser.error('address must be an aligned 32-bit guest address')
    build_dir = args.build_dir.resolve()
    patch_dir = (args.patch_dir or PROJECT / 'local' / 'live-patches' / args.game).resolve()
    patch_dir.mkdir(parents=True, exist_ok=True)
    log_path = patch_dir / f'auto-{args.address:08x}.log'
    try:
        with log_path.open('w') as log, redirect_stdout(log), redirect_stderr(log):
            module = build(args.game, args.address, build_dir, patch_dir,
                           site=(args.site or None) if args.kind != 'missing_function' else None,
                           site_section=args.section if args.site else None,
                           auto_candidate=args.kind == 'missing_function')
    except (RecompError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        reason = str(error)
        report = patch_dir / f'review-{args.address:08x}.json'
        report.write_text(json.dumps({'game': args.game, 'address': f'0x{args.address:08x}',
                                      'site': f'0x{args.site:08x}', 'kind': args.kind,
                                      'reason': reason, 'build_log': str(log_path)}, indent=2) + '\n')
        print(f'live repair: automatic build stopped: {reason}; review {report}', file=sys.stderr)
        if args.ai or os.environ.get('RECOMP_LIVE_AI') == '1':
            return 0 if handoff(args.game, args.address, args.site, args.section, args.kind,
                                build_dir, patch_dir, reason, report) else 1
        return 1
    print(f'live repair: published {module}', file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
