#!/usr/bin/env python3
"""Review missing-function tables and roots once per change in checked evidence.

The game profile opts in to this bounded Codex pass. Accepted proposals are
validated and saved by review_candidates.py; a marker avoids repeating the same
review on every ordinary build.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

from common import RecompError, load_profile, write_json
from generate_game import GAMES, PROJECT
from review_candidates import run


def pending_requests(game: str, rom_sha1: str) -> list[str]:
    """Treat a new runtime miss as new evidence, even without a live module."""
    patch_dir = PROJECT / 'local' / 'live-patches' / game
    addresses = set()
    for path in patch_dir.glob('request-*.json'):
        try:
            request = json.loads(path.read_text())
            if request.get('rom_sha1') == rom_sha1 and request.get('kind') == 'missing_function':
                value = request['address']
                addresses.add(f'{int(value, 0) if isinstance(value, str) else int(value):08x}')
        except (OSError, ValueError, KeyError, TypeError):
            continue
    return sorted(addresses)


def evidence_key(game: str) -> dict:
    path = PROJECT / 'games' / game / 'profile.toml'
    profile = load_profile(path)
    rom_sha1 = profile['game']['rom_sha1']
    checked_inputs = hashlib.sha256()
    for field in ('entry_inventory', 'inventory'):
        relative = profile['discovery'].get(field)
        if relative:
            source = path.parent / relative
            checked_inputs.update(field.encode())
            checked_inputs.update(source.read_bytes())
    scanner = hashlib.sha256()
    for source in ('inspect_function_candidates.py', 'metadata_candidates.py',
                   'probe_function_roots.py', 'review_candidates.py',
                   'review_discovery.py'):
        scanner.update((PROJECT / 'tools' / source).read_bytes())
    scanner.update((path.parent / 'generate.py').read_bytes())
    return {'rom_sha1': rom_sha1,
            'profile_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'checked_inputs_sha256': checked_inputs.hexdigest(),
            'scanner_sha256': scanner.hexdigest(),
            'runtime_requests': pending_requests(game, rom_sha1)}


def review(game: str, build_dir: Path, max_tables: int, max_roots: int) -> None:
    profile = load_profile(PROJECT / 'games' / game / 'profile.toml')
    if not profile['discovery'].get('auto_candidate_review', False):
        print(f'{game}: automatic candidate review is disabled')
        return
    marker = PROJECT / 'local' / 'candidate-review' / game / 'preflight.json'
    current = evidence_key(game)
    try:
        previous = json.loads(marker.read_text())
    except (OSError, ValueError):
        previous = {}
    if all(previous.get(key) == value for key, value in current.items()):
        print(f'{game}: candidate review already completed for this evidence')
        return
    print(f'{game}: reviewing checked function tables and independent roots', flush=True)
    run(game, build_dir, True, max_tables)
    run(game, build_dir, True, max_roots, roots_only=True)
    marker.parent.mkdir(parents=True, exist_ok=True)
    write_json(marker, {'game': game, **evidence_key(game)})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', choices=GAMES, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--max-tables', type=int, default=3)
    parser.add_argument('--max-roots', type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.max_tables <= 10 or not 1 <= args.max_roots <= 10:
        parser.error('review limits must be between 1 and 10')
    try:
        review(args.game, args.build_dir.resolve(), args.max_tables, args.max_roots)
    except (RecompError, OSError, ValueError, KeyError, ImportError) as error:
        print(f'discovery review stopped: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
