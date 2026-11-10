#!/usr/bin/env python3
"""Validate every mapped USO's declared Text symbols in one static pass.

Writes an inventory proposal and all unresolved control flow before gameplay.
--apply installs evidence only after discovery and instruction analysis pass.
No game is launched, no heuristic candidate roots are promoted, and unresolved
switches are never replaced with success stubs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import tempfile

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from common import RecompError, load_metadata, load_profile, load_rom, write_json
from discover import discover
from emit import FunctionRegistry, analyze, decode_function
import generate as game_generator
from uso_symbols import symbol_inventory, merge_entries


def inspect(output: Path) -> tuple[dict, dict, list[dict]]:
    profile = load_profile(BASE / 'profile.toml')
    profile['discovery'].pop('uso_entry_inventory', None)
    original, _ = load_rom(profile['game']['rom'], profile['game']['rom_sha1'], profile['game']['rom_size'])
    with tempfile.TemporaryDirectory(prefix='1080-symbols-') as temporary:
        rom, profile = game_generator.prepare(original, profile, Path(temporary))
        inventory = symbol_inventory(original, rom, profile)
        entries_path = profile['discovery']['entry_inventory']
        entries = merge_entries(json.loads(entries_path.read_text()), inventory)
        entries['rom_sha1'] = hashlib.sha1(rom).hexdigest()
        write_json(entries_path, entries)
        existing = {(s['section'], s['pc']) for s in profile['discovery'].get('registered_direct_calls', [])}
        imports = []
        for name, sites in profile['uso_relocation_pcs'].items():
            section = next(s for s in profile['discovery']['regions'] if s['name'] == name)
            for pc in sorted(sites):
                word = struct.unpack_from('>I', rom, section['rom_offset'] + pc - section['vram'])[0]
                if word >> 26 == 3 and word & 0x03ffffff == 0 and (name, pc) not in existing:
                    imports.append(dict(section=name, pc=pc, expected_word=word, target=0x80000000))
        profile['discovery']['registered_direct_calls'] = [
            *profile['discovery'].get('registered_direct_calls', []), *imports]
        metadata = discover(rom, profile)
        game_generator.finalize_metadata(rom, metadata, profile)
        checked_metadata = Path(temporary) / 'review-symbols.json'
        write_json(checked_metadata, metadata)
        metadata = load_metadata(checked_metadata, rom)
        registry = FunctionRegistry(metadata['functions'])
        issues = []
        for function in metadata['functions']:
            if function['origin'] in ('native-replacement', 'unsupported-boundary'):
                continue
            problems = [*function['discovery_errors'],
                        *analyze(function, decode_function(rom, function), registry)]
            for problem in problems:
                issues.append(dict(section=metadata['sections'][function['section']]['name'],
                                   function=function['vram'], name=function['name'], **problem))
        report = dict(rom_sha1=inventory['rom_sha1'],
                      summary=dict(declared_text_entries=len(inventory['entries']),
                                   excluded_non_cpu_symbols=len(inventory['excluded_symbols']),
                                   discovered_functions=len(metadata['functions']),
                                   native_hooks=sum(f['origin'] == 'native-replacement' for f in metadata['functions']),
                                   unsupported_boundaries=sum(f['origin'] == 'unsupported-boundary' for f in metadata['functions']),
                                   new_dynamic_imports=len(imports), issues=len(issues)),
                      modules=inventory['modules'], dynamic_imports=imports, issues=issues)
        output.mkdir(parents=True, exist_ok=True)
        write_json(output / 'inventory.json', inventory)
        write_json(output / 'report.json', report)
        write_json(output / 'symbols.json', metadata)
        print(json.dumps(report['summary']))
        for issue in issues[:24]:
            print(f'{issue["section"]} {issue["name"]} {issue.get("pc", 0):#010x}: {issue["reason"]}')
        print(f'Report: {output / "report.json"}')
        return inventory, report, imports


def apply(source: bytes, inventory: dict, imports: list[dict]) -> None:
    text = source.decode()
    if 'uso_entry_inventory' not in text:
        text = text.replace('[discovery]\n', '[discovery]\nuso_entry_inventory = "uso-entries.json"\n', 1)
    if imports:
        marker = re.search(r'(?m)^registered_direct_calls = \[\n', text)
        if marker is None:
            raise RecompError('profile has no registered_direct_calls list')
        lines = ['  # Unbound J26 imports checked from USO relocation records by discover_symbols.py.']
        lines += [f'  {{section = "{s["section"]}", pc = 0x{s["pc"]:08x}, '
                  f'expected_word = 0x{s["expected_word"]:08x}, target = 0x80000000}},' for s in imports]
        text = text[:marker.end()] + '\n'.join(lines) + '\n' + text[marker.end():]
    path = BASE / 'profile.toml'
    if path.read_bytes() != source:
        raise RecompError('profile changed during review; preserving concurrent changes')
    write_json(BASE / 'uso-entries.json', inventory)
    path.write_text(text)
    print(f'Installed {len(inventory["entries"])} declared Text entries; run the normal build.')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/1080-us-debug/uso-symbol-review')
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    try:
        source = (BASE / 'profile.toml').read_bytes()
        inventory, report, imports = inspect(args.output.resolve())
        if report['issues']:
            print('Unresolved static issues remain; no permanent inventory was changed.')
            return 1
        if args.apply:
            apply(source, inventory, imports)
        return 0
    except (RecompError, KeyError, ValueError, OSError) as error:
        print(f'USO symbol review stopped: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
