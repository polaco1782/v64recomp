#!/usr/bin/env python3
"""Map a guest address to discovery/coverage evidence, optionally with verified ROM bytes."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

from common import RecompError, load_metadata


def read_inputs(metadata_path: Path, rom_path: Path | None, coverage_path: Path | None):
    metadata = json.loads(metadata_path.read_text())
    if metadata.get('format_version') != 1:
        raise RecompError('unsupported function metadata version')
    rom = None
    if rom_path is not None:
        rom = rom_path.read_bytes()
        # Prepared images are inspected as exact bytes. Never silently reinterpret
        # a raw-ROM offset as an offset into a decompressed analysis image.
        metadata = load_metadata(metadata_path, rom)
    candidate = coverage_path or metadata_path.with_name('coverage.json')
    coverage = None
    if coverage_path is not None or candidate.is_file():
        coverage = json.loads(candidate.read_text())
        if (coverage.get('format_version') != 1
                or coverage.get('rom_sha1') != metadata['rom_sha1']):
            raise RecompError('coverage belongs to a different analysis image')
    return metadata, rom, coverage


def inspect_address(metadata: dict, address: int, section: str | None = None,
                    rom: bytes | None = None, coverage: dict | None = None,
                    before: int = 4, words: int = 16) -> None:
    if not 0 <= address <= 0xffffffff or before < 0 or not 1 <= words <= 4096:
        raise RecompError('invalid guest address or disassembly window (1..4096 words)')
    sections = [s for s in metadata['sections']
                if s['vram'] <= address < s['vram'] + s['size']
                and (section is None or section in (str(s['id']), s['name']))]
    if not sections:
        raise RecompError('address is outside the selected mapped sections; check loading/relocation')
    if len(sections) != 1:
        raise RecompError('address maps to multiple sections; select --section by ID or name')
    mapped = sections[0]
    sid = mapped['id']
    offset = mapped['rom_offset'] + address - mapped['vram']
    print(f"Guest {address:#010x}: section {sid} {mapped['name']}, analysis ROM offset {offset:#x}")
    print(f"Analysis SHA-1: {metadata['rom_sha1']}")
    if rom is None:
        print('Metadata only: ROM bytes and fingerprints have not been verified.')
    print(f"Loaded at boot: {mapped.get('loaded_at_boot', False)} (current runtime state is not recorded here)")
    owners = [f for f in metadata['functions'] if f['section'] == sid
              and address in f.get('instruction_addresses', range(f['vram'], f['vram'] + f['size'], 4))]
    spans = [f for f in metadata['functions'] if f['section'] == sid
             and f['vram'] <= address < f['vram'] + f['size']]
    for function in spans:
        owned = function in owners
        relation = 'entry' if function['vram'] == address else 'owned instruction' if owned else 'unowned byte in span'
        print(f"Function: {function['name']} + {address - function['vram']:#x} ({relation})")
        print(f"  Entry {function['vram']:#010x}; size {function['size']:#x}; origin {function.get('origin', '?')}")
        print(f"  Generated name: recomp_s{sid}_{function['vram']:08x} (hooks use their native handler)")
        if function.get('aliases'):
            print('  Aliases: ' + ', '.join(function['aliases']))
        for error in function.get('discovery_errors', []):
            print('  Discovery: ' + json.dumps(error, sort_keys=True))
        for result in (coverage or {}).get('functions', []):
            if (result['section'], result['vram']) == (sid, function['vram']):
                print(f"  Translation: {result['status']}; selected: {result.get('selected')}")
                for error in result.get('errors', []):
                    print('  Translation error: ' + json.dumps(error, sort_keys=True))
    if not owners:
        print('No discovered instruction owns this address. A containing span alone is not evidence of code.')
    for gap in metadata.get('unvisited_ranges', []):
        if gap['section'] == sid and gap['vram'] <= address < gap['vram'] + gap['size']:
            print(f"Unvisited range: {gap['vram']:#010x}..{gap['vram'] + gap['size']:#010x} (end exclusive)")
    print('Known references to this exact address (not an exhaustive call graph):')
    references = 0
    for function in metadata['functions']:
        for transfer in function.get('indirect_transfers', []):
            if address in transfer.get('targets', []):
                print(f"  {function['name']} s{function['section']} at {transfer['pc']:#010x}: checked {transfer['kind']}")
                references += 1
        if rom is None:
            continue
        # Only inspect owned words, so data in gaps cannot masquerade as JALs.
        for pc in function.get('instruction_addresses', range(function['vram'], function['vram'] + function['size'], 4)):
            pos = function['rom_offset'] + pc - function['vram']
            word = int.from_bytes(rom[pos:pos + 4], 'big')
            if word >> 26 not in (2, 3):
                continue
            destination = (((pc + 4) & 0xf0000000) | ((word & 0x03ffffff) << 2))
            if destination == address:
                kind = 'jal' if word >> 26 == 3 else 'j'
                print(f"  {function['name']} s{function['section']} at {pc:#010x}: {kind}")
                references += 1
    if not references:
        print('  None found.' + (' Supply --rom to scan direct J/JAL references.' if rom is None else ''))
    if rom is None:
        return
    from emit import decode
    start = max(mapped['vram'], (address & ~3) - min(before, words - 1) * 4)
    end = min(mapped['vram'] + mapped['size'], start + words * 4)
    pos = mapped['rom_offset'] + start - mapped['vram']
    print('Verified analysis-image disassembly (may include data):')
    for instruction in decode(rom[pos:pos + end - start], start):
        marker = '>' if instruction.pc == (address & ~3) else ' '
        print(f"{marker} {instruction.pc:08x}  {instruction.word:08x}  {instruction.assembly}")
    if address % 4 == 0 and offset + 16 <= mapped['rom_offset'] + mapped['size']:
        print('First-16-byte SHA-256 (entry candidate evidence, not proof of a function): ' +
              hashlib.sha256(rom[offset:offset + 16]).hexdigest())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--metadata', required=True, type=Path, help='symbols.json from this build/report')
    parser.add_argument('--address', required=True, type=lambda value: int(value, 0), help='32-bit guest address')
    parser.add_argument('--section', help='section ID or name; required for ambiguous mappings')
    parser.add_argument('--rom', type=Path, help='exact normalized/prepared image used for discovery')
    parser.add_argument('--coverage', type=Path, help='default: coverage.json beside metadata, if present')
    parser.add_argument('--before', type=int, default=4, help='instructions before address, default 4')
    parser.add_argument('--words', type=int, default=16, help='total disassembly words, default 16')
    args = parser.parse_args()
    metadata, rom, coverage = read_inputs(args.metadata, args.rom, args.coverage)
    inspect_address(metadata, args.address, args.section, rom, coverage, args.before, args.words)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RecompError, OSError, ValueError, KeyError, TypeError, ImportError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
