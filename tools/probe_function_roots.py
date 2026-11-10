#!/usr/bin/env python3
"""Dry-run discovery from promising missing-function roots and their call trees.

This checks the pinned prepared ROM and reports what one proposed root would
register. It never changes a game profile or treats a pointer as a checked root.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

from common import RecompError, load_profile, load_rom, write_json
from discover import discover
PROJECT = Path(__file__).resolve().parents[1]


def prepare(game: str) -> tuple[bytes, dict]:
    base = PROJECT / 'games' / game
    profile = load_profile(base / 'profile.toml')
    rom, _ = load_rom(profile['game']['rom'], profile['game']['rom_sha1'],
                      profile['game']['rom_size'])
    sys.path.insert(0, str(base))
    spec = importlib.util.spec_from_file_location('game_probe', base / 'generate.py')
    if spec is None or spec.loader is None:
        raise RecompError('game preparation module is unavailable')
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    with tempfile.TemporaryDirectory(prefix='v64-root-probe-') as directory:
        if hasattr(generator, 'prepare'):
            rom, profile = generator.prepare(rom, profile, Path(directory))
    return rom, profile


def call_edges(candidates: list[dict]) -> dict[int, set[int]]:
    edges: dict[int, set[int]] = defaultdict(set)
    for candidate in candidates:
        for ref in candidate.get('references', []):
            if ref.get('kind') == 'candidate-direct-call' and type(ref.get('candidate')) is int:
                edges[ref['candidate']].add(candidate['vram'])
    return edges


def descendants(root: int, edges: dict[int, set[int]]) -> set[int]:
    reached = {root}
    pending = [root]
    while pending:
        for target in edges[pending.pop()]:
            if target not in reached:
                reached.add(target)
                pending.append(target)
    return reached


def ranked_roots(candidates: list[dict], edges: dict[int, set[int]]) -> list[dict]:
    """Prioritize independent ROM pointers whose code probe found a call tree."""
    ranked = []
    for candidate in candidates:
        refs = candidate.get('references', [])
        body = candidate.get('body', {})
        if (candidate.get('registered') or candidate.get('confidence') != 'likely'
                or body.get('status') != 'closed' or not body.get('stack_prologue')
                or not any(ref.get('kind') == 'image-pointer' for ref in refs)
                or any(ref.get('kind') == 'candidate-direct-call' for ref in refs)):
            continue
        family = descendants(candidate['vram'], edges)
        ranked.append({'vram': candidate['vram'], 'section': candidate['section'],
                       'candidate_descendants': len(family),
                       'candidate_addresses': sorted(family)})
    ranked.sort(key=lambda item: (-item['candidate_descendants'], item['section'], item['vram']))
    return ranked


def probe(game: str, metadata_path: Path, addresses: list[int], limit: int) -> dict:
    metadata = json.loads(metadata_path.read_text())
    report = json.loads(metadata_path.with_name('function-candidates.json').read_text())
    rom, profile = prepare(game)
    digest = hashlib.sha1(rom).hexdigest()
    if metadata['rom_sha1'] != digest or report['rom_sha1'] != digest:
        raise RecompError('generated metadata or candidate report differs from the prepared ROM')
    candidates = report['candidates']
    by_address: dict[int, list[dict]] = defaultdict(list)
    for candidate in candidates:
        by_address[candidate['vram']].append(candidate)
    edges = call_edges(candidates)
    ranked = ranked_roots(candidates, edges)
    selected = ([{'vram': address} for address in addresses] if addresses else ranked[:limit])
    existing = {(item['section'], item['vram']) for item in metadata['functions']}
    results = []
    for item in selected:
        address = item['vram']
        matches = by_address.get(address, [])
        if len(matches) != 1:
            results.append({'vram': address, 'status': 'unresolved',
                            'reason': 'candidate is missing or ambiguous'})
            continue
        candidate = matches[0]
        section = metadata['sections'][candidate['section']]
        offset = section['rom_offset'] + address - section['vram']
        family = descendants(address, edges)
        result = {'vram': address, 'section': candidate['section'],
                  'confidence': candidate['confidence'],
                  'references': [ref['kind'] for ref in candidate['references']],
                  'candidate_descendants': len(family)}
        if (candidate.get('registered') or offset != candidate['rom_offset']
                or hashlib.sha256(rom[offset:offset + 16]).hexdigest() != candidate['sha256']):
            result.update(status='unresolved', reason='candidate mapping or ROM bytes changed')
            results.append(result)
            continue
        root = {'name': f'probe_{address:08x}', 'vram': address, 'section': section['name']}
        discovery = {**profile['discovery'],
                     'roots': [*profile['discovery'].get('roots', []), root]}
        try:
            updated = discover(rom, {**profile, 'discovery': discovery})
            gained = {(function['section'], function['vram'])
                      for function in updated['functions']} - existing
            result.update(status='discovered', new_functions=len(gained),
                          candidate_descendants_discovered=sum(
                              (candidate['section'], value) in gained for value in family),
                          gained_addresses=[f'0x{value:08x}' for _, value in sorted(gained)[:64]])
        except (RecompError, ValueError, KeyError) as error:
            result.update(status='unresolved', reason=str(error))
        results.append(result)
    return {'game': game, 'rom_sha1': digest, 'ranked_roots': ranked,
            'probes': results,
            'note': 'A successful dry run checks discovery, not callback provenance or gameplay.'}


def main() -> int:
    from generate_game import GAMES
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', choices=GAMES, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--address', action='append', type=lambda value: int(value, 0), default=[])
    parser.add_argument('--max-roots', type=int, default=3)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not 1 <= args.max_roots <= 20:
        parser.error('--max-roots must be between 1 and 20')
    metadata = args.build_dir.resolve() / 'generated' / args.game / 'symbols.json'
    output = args.output or args.build_dir.resolve() / 'candidate-review' / args.game / 'root-probes.json'
    try:
        result = probe(args.game, metadata, args.address, args.max_roots)
        write_json(output, result)
        for item in result['probes']:
            print(f"0x{item['vram']:08x}: {item['status']}; "
                  f"{item.get('new_functions', 0)} new functions, "
                  f"{item.get('candidate_descendants_discovered', 0)}/"
                  f"{item.get('candidate_descendants', 0)} candidate family; "
                  f"{item.get('reason', '')}")
        print(f'Report: {output}')
    except (RecompError, OSError, ValueError, KeyError, ImportError) as error:
        print(f'root probe stopped: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
