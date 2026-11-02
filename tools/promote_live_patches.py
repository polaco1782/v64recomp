#!/usr/bin/env python3
"""Make published live functions permanent, then clear their temporary files.

Run after closing the game. The normal generator must accept every promoted
function before any live patch file is removed.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import json
import os
from pathlib import Path
import re
import shutil
import sys
import textwrap

from common import RecompError, load_profile, load_rom
from generate_game import GAMES, PROJECT, generate


CONTROL = re.compile(r'function-([0-9a-f]{8})\.path')


def write_atomic(path: Path, content: bytes) -> None:
    temporary = path.with_name(f'.{path.name}.promote-{os.getpid()}')
    try:
        temporary.write_bytes(content)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def function_at(metadata: dict, address: int, section: int) -> dict | None:
    matches = [f for f in metadata['functions']
               if f['vram'] == address and f['section'] == section]
    if len(matches) > 1:
        raise RecompError(f'ambiguous function 0x{address:08x} in section {section}')
    return matches[0] if matches else None


def read_published(patch_dir: Path, current: dict) -> list[dict]:
    published = []
    for control in sorted(patch_dir.glob('function-*.path')):
        match = CONTROL.fullmatch(control.name)
        if not match or control.is_symlink():
            raise RecompError(f'invalid live patch control file: {control}')
        address = int(match[1], 16)
        module = Path(control.read_text().strip())
        if not module.is_absolute() or module.parent.resolve() != patch_dir or not module.is_file():
            raise RecompError(f'published module is missing or outside {patch_dir}: {control}')
        match = re.fullmatch(rf'module-{address:08x}-([0-9a-f]{{32}})\.so', module.name)
        if not match:
            raise RecompError(f'published module does not match {control.name}')
        snapshot = patch_dir / '.build' / f'{address:08x}-{match[1]}' / 'symbols.json'
        if not snapshot.is_file():
            raise RecompError(f'live build metadata is missing: {snapshot}')
        metadata = json.loads(snapshot.read_text())
        same_rom = ((metadata['rom_sha1'], metadata['rom_size'])
                    == (current['rom_sha1'], current['rom_size']))
        matches = [f for f in metadata['functions'] if f['vram'] == address]
        if len(matches) != 1:
            raise RecompError(f'live function is missing or ambiguous: {control.name}')
        function = matches[0]
        old_section = metadata['sections'][function['section']]
        sections = [s for s in current['sections'] if s['name'] == old_section['name']]
        if len(sections) != 1:
            raise RecompError(f'live section is missing or ambiguous: {control.name}')
        current_section = sections[0]
        section = current_section['id']
        existing = function_at(current, address, section)
        if not same_rom:
            # Adding checked analysis sections can change the prepared ROM and
            # section IDs. Only retire an already permanent, identical function
            # in an identical complete Text image in that case. New roots still
            # require the exact prepared ROM used by their live build.
            identity = ('name', 'vram', 'size', 'sha256')
            if (existing is None or not old_section.get('sha256')
                    or any(old_section[key] != current_section.get(key) for key in identity)):
                raise RecompError(f'live build used different prepared ROM bytes: {control.name}')
        if existing and (existing['sha256'], existing['size']) != (function['sha256'], function['size']):
            raise RecompError(f'current function differs from live build: {control.name}')
        if not existing and function.get('origin') != 'profile-root':
            raise RecompError(f'live function needs a checked table or inventory entry: {control.name}')
        published.append({'address': address, 'section': section,
                          'section_name': current_section['name'],
                          'sha256': function['sha256'], 'size': function['size'],
                          'registered': existing is not None})
    return published


def add_roots(source: bytes, additions: list[dict], name_prefix: str = 'live') -> bytes:
    if not additions:
        return source
    text = source.decode()
    discovery = re.search(r'(?m)^\[discovery\]\s*$', text)
    if discovery is None:
        raise RecompError('profile has no discovery section')
    next_section = re.search(r'(?m)^\[', text[discovery.end():])
    section_end = discovery.end() + next_section.start() if next_section else len(text)
    section = text[discovery.end():section_end]
    multiline = list(re.finditer(r'(?m)^roots = \[\n', section))
    inline = list(re.finditer(r'(?m)^roots = \[([^\n]*)\]$', section))
    if len(multiline) + len(inline) > 1:
        raise RecompError('profile must have at most one discovery roots list')
    lines = ['  # ROM-verified function roots.']
    for item in sorted(additions, key=lambda entry: entry['address']):
        address = item['address']
        evidence = ' '.join(item.get('evidence', '').split())
        for part in textwrap.wrap(evidence, width=88):
            lines.append(f'  # {part}')
        lines.append(f'  {{name = "{name_prefix}_{address:08x}", vram = 0x{address:08x}, '
                     f'section = "{item["section_name"]}"}},')
    if not multiline and not inline:
        replacement = 'roots = [\n' + '\n'.join(lines) + '\n]\n\n'
        if section_end and text[section_end - 1] != '\n':
            replacement = '\n' + replacement
        return (text[:section_end] + replacement + text[section_end:]).encode()
    if inline:
        match = inline[0]
        existing = match[1].strip().rstrip(',')
        existing_lines = [f'  {existing},'] if existing else []
        replacement = 'roots = [\n' + '\n'.join(existing_lines + lines) + '\n]'
        start = discovery.end() + match.start()
        end = discovery.end() + match.end()
        return (text[:start] + replacement + text[end:]).encode()
    start = discovery.end() + multiline[0].end()
    end = text.find('\n]\n', start, section_end)
    if end < 0 or '\n[' in text[start:end]:
        raise RecompError('cannot locate the end of the discovery roots list')
    return (text[:end] + '\n' + '\n'.join(lines) + text[end:]).encode()


def needed_roots(published: list[dict], candidates: dict, current: dict) -> list[dict]:
    """Seed patched callers; discovery will follow their direct calls."""
    if candidates.get('rom_sha1') != current['rom_sha1']:
        raise RecompError('candidate report and generated metadata use different ROMs')
    by_key = {(item['section'], item['vram']): item
              for item in candidates.get('candidates', [])}
    entries = {(item['section'], item['address']): item for item in published}
    reached = {key for key, item in entries.items() if item['registered']}
    successors: dict[tuple[int, int], set[tuple[int, int]]] = defaultdict(set)
    predecessors: dict[tuple[int, int], set[tuple[int, int]]] = defaultdict(set)
    for key in entries:
        candidate = by_key.get(key)
        if candidate is None:
            continue
        for ref in candidate.get('references', []):
            if ref.get('kind') != 'candidate-direct-call':
                continue
            caller = (ref.get('section'), ref.get('candidate'))
            if caller in entries and caller != key:
                successors[caller].add(key)
                predecessors[key].add(caller)

    def follow_calls() -> None:
        pending = list(reached)
        while pending:
            for target in successors[pending.pop()]:
                if target not in reached:
                    reached.add(target)
                    pending.append(target)

    follow_calls()
    roots = []
    while len(reached) < len(entries):
        missing = set(entries) - reached
        # Prefer a caller with no unpublished predecessor. A cycle still gets
        # one deterministic root, and the validation pass checks every body.
        sources = [key for key in missing if not predecessors[key] & missing]
        key = min(sources or missing)
        roots.append(entries[key])
        reached.add(key)
        follow_calls()
    return roots


def promote(game: str, build_dir: Path, patch_dir: Path) -> None:
    if patch_dir.is_symlink():
        raise RecompError('live patch directory must not be a symlink')
    if not patch_dir.exists():
        print(f'No live patches in {patch_dir}')
        return
    patch_dir = patch_dir.resolve()
    expected = (PROJECT / 'local' / 'live-patches' / game).resolve()
    if patch_dir != expected:
        raise RecompError(f'cleanup is restricted to {expected}')
    profile_path = PROJECT / 'games' / game / 'profile.toml'
    profile = load_profile(profile_path)
    load_rom(profile['game']['rom'], profile['game']['rom_sha1'], profile['game']['rom_size'])
    current_path = build_dir / 'generated' / game / 'symbols.json'
    if not current_path.is_file():
        raise RecompError(f'generate the game first: {current_path}')
    current = json.loads(current_path.read_text())
    published = read_published(patch_dir, current)
    if not published:
        if any(patch_dir.iterdir()):
            print(f'No published live functions in {patch_dir}; diagnostic files remain')
            return
        print('No live patches to promote')
        return
    report_path = current_path.with_name('function-candidates.json')
    if not report_path.is_file():
        raise RecompError(f'generate the game first: {report_path}')
    candidates = json.loads(report_path.read_text())
    checked_roots = profile['discovery'].get('roots', [])
    additions = [entry for entry in needed_roots(published, candidates, current)
                 if not any(root['vram'] == entry['address'] and
                            root.get('section') in (None, entry['section_name'])
                            for root in checked_roots)]
    original = profile_path.read_bytes()
    replacement = add_roots(original, additions)
    if replacement != original:
        write_atomic(profile_path, replacement)
    try:
        # This checks discovery, ROM-bound function bodies, and translation.
        generate(game, build_dir / 'generated' / game, False)
        updated = json.loads(current_path.read_text())
        for item in published:
            function = function_at(updated, item['address'], item['section'])
            if (function is None or (function['sha256'], function['size'])
                    != (item['sha256'], item['size'])):
                raise RecompError(f'promoted function changed: 0x{item["address"]:08x}')
    except Exception:
        if replacement != original:
            write_atomic(profile_path, original)
        raise
    shutil.rmtree(patch_dir)
    patch_dir.mkdir(parents=True)
    print(f'Promoted {len(additions)} function roots; {len(published) - len(additions)} '
          f'resolved through existing entries or discovered calls. Cleared {patch_dir}')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, choices=GAMES)
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        promote(args.game, args.build_dir.resolve(),
                PROJECT / 'local' / 'live-patches' / args.game)
    except (RecompError, OSError, ValueError, KeyError) as error:
        print(f'promotion stopped: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
