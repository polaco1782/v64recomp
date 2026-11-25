#!/usr/bin/env python3
"""Make published live functions permanent, then clear their temporary files.

Run after closing the game. The normal generator must accept every promoted
function before any live patch file is removed.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
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
from observed_transfers import promote_observations


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


def compiled_without_snapshot(snapshot: Path, coverage: dict, address: int) -> bool:
    """An orphaned module needs no promotion if its sole entry already builds.
    Coverage cannot verify a body fingerprint. Keep all repair files in this case.
    """
    old_path = snapshot.with_name('coverage.json')
    if not old_path.is_file():
        return False
    old = json.loads(old_path.read_text())
    if (not old.get('generated') or not coverage.get('generated')
            or old.get('rom_sha1') != coverage['rom_sha1']):
        return False
    selected = [f for f in old['functions'] if f['selected']]
    current = [f for f in coverage['functions'] if f['vram'] == address]
    return (len(selected) == len(current) == 1 and selected[0]['vram'] == address
            and selected[0]['status'] == 'ready' and current[0]['selected'] and current[0]['status'] == 'ready'
            and all(selected[0][key] == current[0][key] for key in ('section', 'size')))


def read_published(patch_dir: Path, current: dict, coverage_path: Path) -> tuple[list[dict], list[Path]]:
    published, retained = [], []
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
            coverage = json.loads(coverage_path.read_text())
            if coverage['rom_sha1'] == current['rom_sha1'] and compiled_without_snapshot(snapshot, coverage, address):
                retained.append(control)
                continue
            raise RecompError(f'live build metadata is missing: {snapshot}')
        metadata = json.loads(snapshot.read_text())
        same_rom = ((metadata['rom_sha1'], metadata['rom_size'])
                    == (current['rom_sha1'], current['rom_size']))
        report = json.loads(snapshot.with_name('coverage.json').read_text())
        selected = [f for f in report['functions'] if f['selected']]
        roots = [f for f in selected if f['vram'] == address]
        if (len(roots) != 1 or not report.get('generated')
                or any(f['status'] != 'ready' or f['section'] != roots[0]['section'] for f in selected)
                or report['rom_sha1'] != metadata['rom_sha1']):
            raise RecompError(f'cannot identify selected live function: {control.name}')
        matches = [f for f in metadata['functions'] if f['vram'] == address
                   and f['section'] == roots[0]['section'] and f['size'] == roots[0]['size']]
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
        callees = []
        for entry in selected:
            if entry['vram'] == address:
                continue
            callee = function_at(metadata, entry['vram'], old_section['id'])
            if not callee or callee['origin'] != 'direct-call' or callee['size'] != entry['size']:
                raise RecompError(f'invalid bundled live callee: {control.name}')
            compiled = function_at(current, entry['vram'], section)
            if compiled and (compiled['size'], compiled['sha256']) != (callee['size'], callee['sha256']):
                raise RecompError(f'current callee differs from live build: {control.name}')
            if not same_rom and not compiled:
                raise RecompError(f'bundled callee still needs its original prepared ROM: {control.name}')
            callees.append(dict(address=entry['vram'], section=section, size=callee['size'],
                                sha256=callee['sha256'], registered=compiled is not None))
        published.append({'address': address, 'section': section,
                          'section_name': current_section['name'],
                          'sha256': function['sha256'], 'size': function['size'],
                          'registered': existing is not None, 'control': control, 'module': module,
                          'snapshot': snapshot.parent, 'callees': callees})
    return published, retained


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
    published, retained = read_published(patch_dir, current, current_path.with_name('coverage.json'))
    if retained:
        for control in retained:
            print(f'Already compiled {control.name}; missing live snapshot, preserving repair files')
        if all(item['registered'] and all(c['registered'] for c in item['callees']) for item in published):
            return
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
        coverage = json.loads(current_path.with_name('coverage.json').read_text())
        ready = {(f['section'], f['vram']) for f in coverage['functions'] if f['selected'] and f['status'] == 'ready'}
        for module in published:
            for item in (module, *module['callees']):
                function = function_at(updated, item['address'], item['section'])
                if ((item['section'], item['address']) not in ready or function is None
                        or (function['sha256'], function['size']) != (item['sha256'], item['size'])):
                    raise RecompError(f'promoted function changed or unavailable: 0x{item["address"]:08x}')
    except Exception:
        if replacement != original:
            write_atomic(profile_path, original)
        raise
    if retained:
        print(f'Promoted {len(additions)} function roots; preserved {patch_dir} for review')
        return
    for item in published:
        control, module = item['control'], item['module']
        if ((patch_dir / f'request-{item["address"]:08x}.json').exists() or not control.is_file()
                or control.read_text().strip() != str(module)):
            continue
        control.unlink()
        module.unlink(missing_ok=True)
        shutil.rmtree(item['snapshot'])
    entries = {(entry['section'], entry['address']) for module in published for entry in (module, *module['callees'])}
    print(f'Promoted {len(additions)} function roots; {len(entries) - len(additions)} '
          f'resolved through existing entries or discovered calls.')


@contextmanager
def promotion_lock(game):
    # Debug and Release builds can finish configure and start preparing the same
    # profile concurrently. Keep the lock outside directories cleaned by repair.
    path = PROJECT / 'local' / f'.{game}-promotion.lock'
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('a+b') as stream:
        if os.name == 'nt':
            import msvcrt
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        yield


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, choices=GAMES)
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        build_dir = args.build_dir.resolve()
        with promotion_lock(args.game):
            promote(args.game, build_dir, PROJECT / 'local' / 'live-patches' / args.game)
            directory = PROJECT / 'local' / 'cpu-observations' / args.game
            if directory.is_dir():
                profile_path = PROJECT / 'games' / args.game / 'profile.toml'
                profile = load_profile(profile_path)
                game = profile['game']
                rom, _ = load_rom(game['rom'], game['rom_sha1'], game['rom_size'])
                promote_observations(args.game, profile_path, profile, rom, build_dir, directory, write_atomic)
    except (RecompError, OSError, ValueError, KeyError) as error:
        print(f'promotion stopped: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
