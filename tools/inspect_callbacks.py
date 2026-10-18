#!/usr/bin/env python3
"""Inspect callback references and report unowned candidates in a verified analysis image."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

from common import RecompError, load_metadata
from emit import decode
from export_analysis_rom import load_analysis_rom
from generate_game import GAMES
from metadata_candidates import materialized_entries, pointer_entries


def number(value: str) -> int:
    return int(value, 0)


def select_section(metadata: dict, selector: str) -> dict:
    matches = [s for s in metadata['sections'] if selector in (str(s['id']), s['name'])]
    if len(matches) != 1:
        raise RecompError(f'unknown or ambiguous section: {selector}')
    return matches[0]


def mapped_range(metadata: dict, value: str) -> dict:
    try:
        selector, low, high = value.split(':')
        low, high = number(low), number(high)
    except ValueError as error:
        raise RecompError('ranges use SECTION:START:END with an exclusive end') from error
    section = select_section(metadata, selector)
    if (low % 4 or high % 4 or not section['vram'] <= low < high <= section['vram'] + section['size']):
        raise RecompError(f'unaligned or unmapped range: {value}')
    return dict(section=section['id'], vram=low, size=high-low,
                rom_offset=section['rom_offset']+low-section['vram'])


def owned_ranges(function: dict) -> list[dict]:
    """Scan contiguous owned runs, without treating gaps in a function span as code."""
    runs = []
    for pc in function['instruction_addresses']:
        if runs and runs[-1]['vram'] + runs[-1]['size'] == pc:
            runs[-1]['size'] += 4
        else:
            runs.append(dict(vram=pc, size=4,
                             rom_offset=function['rom_offset']+pc-function['vram']))
    return runs


def inspect_callbacks(rom: bytes, metadata: dict, text: list[dict], data: list[dict],
                      targets: set[int], section_filter: str | None = None,
                      include_owned: bool = False) -> dict:
    # The candidate helpers resolve addresses within nonoverlapping text ranges.
    # Require explicit overlay selection instead of merging equal guest addresses.
    ordered = sorted(text, key=lambda r: r['vram'])
    if any(a['vram']+a['size'] > b['vram'] for a, b in zip(ordered, ordered[1:])):
        raise RecompError('text ranges overlap; select one overlay/section at a time')
    selected = select_section(metadata, section_filter)['id'] if section_filter else None
    entries = {(f['section'], f['vram']): f for f in metadata['functions']}
    owners = {(f['section'], pc): f for f in metadata['functions'] for pc in f['instruction_addresses']}
    records = {}

    def record(address: int) -> dict | None:
        mapped = [r for r in text if r['vram'] <= address and address+16 <= r['vram']+r['size']
                  and (selected is None or r['section'] == selected)]
        if len(mapped) != 1:
            return None
        region = mapped[0]
        key = (region['section'], address)
        if key not in records:
            owner = owners.get(key)
            offset = region['rom_offset']+address-region['vram']
            records[key] = dict(vram=address, section=region['section'], rom_offset=offset,
                status='entry' if key in entries else 'owned-interior' if owner else 'unowned',
                owner=owner['name'] if owner else None,
                sha256=hashlib.sha256(rom[offset:offset+16]).hexdigest(), references=[])
        return records[key]

    for address in targets:
        if address % 4 or record(address) is None:
            raise RecompError(f'target {address:#010x} is unaligned or outside the selected text ranges')

    def reference(address: int, kind: str, pc: int, source: dict, function: dict | None = None):
        item = record(address)
        if item is None:
            return
        item['references'].append(dict(kind=kind, pc=pc, section=source['section'],
            rom_offset=source['rom_offset']+pc-source['vram'],
            function=function['name'] if function else None))

    # Empty ownership lets the helpers report known targets as well as candidates.
    # Ownership is classified above using section IDs, including shared-tail gaps.
    empty = {'functions': []}
    for function in metadata['functions']:
        source = dict(section=function['section'], vram=function['vram'], rom_offset=function['rom_offset'])
        candidates = materialized_entries(rom, empty, text, source_ranges=owned_ranges(function))
        for candidate in candidates:
            address = candidate['vram']
            if not targets or address in targets:
                for pc in candidate['materialized_sources']:
                    reference(address, 'materialized-address', pc, source, function)
        for pc in function['instruction_addresses']:
            offset = function['rom_offset']+pc-function['vram']
            word = int.from_bytes(rom[offset:offset+4], 'big')
            if word >> 26 in (2, 3):
                address = ((pc+4) & 0xf0000000) | ((word & 0x3ffffff) << 2)
                if address in targets:
                    reference(address, 'jal' if word >> 26 == 3 else 'j', pc, source, function)
        for transfer in function.get('indirect_transfers', []):
            for address in targets.intersection(transfer.get('targets', [])):
                reference(address, 'checked-'+transfer['kind'], transfer['pc'], source, function)
    for region in data:
        for candidate in pointer_entries(rom, empty, text, [region], []):
            address = candidate['vram']
            if not targets or address in targets:
                for pc in candidate['pointer_sources']:
                    reference(address, 'data-pointer', pc, region)
    results = [item for _, item in sorted(records.items())
               if targets or include_owned or item['status'] == 'unowned']
    data_ranges = [{**region, 'sha256': hashlib.sha256(
        rom[region['rom_offset']:region['rom_offset']+region['size']]).hexdigest()} for region in data]
    return dict(format_version=1, game=metadata['game'], rom_sha1=metadata['rom_sha1'],
                candidates_only=not bool(targets), data_ranges=data_ranges, targets=results)


def print_report(report: dict, rom: bytes, metadata: dict, before: int, after: int) -> None:
    print(f"{report['game']}; verified analysis SHA-1 {report['rom_sha1']}")
    print('Address materializations and data pointers are candidates, not proof of callbacks.')
    for region in report['data_ranges']:
        print(f"Data range s{region['section']} {region['vram']:#010x}.."
              f"{region['vram']+region['size']:#010x} ({region['size']//4} words); "
              f"SHA-256 {region['sha256']}")
    sections = {s['id']: s for s in metadata['sections']}
    for target in report['targets']:
        print(f"\n{target['vram']:#010x} s{target['section']} {target['status']}; "
              f"analysis offset {target['rom_offset']:#x}; owner {target['owner'] or '-'}")
        print(f"  First-16-byte SHA-256: {target['sha256']}")
        for ref in target['references']:
            print(f"  {ref['kind']} s{ref['section']} {ref['pc']:#010x} "
                  f"({ref['function'] or 'data'}; analysis offset {ref['rom_offset']:#x})")
            if report['candidates_only']:
                continue
            section = sections[ref['section']]
            low = max(section['vram'], ref['pc']-before*4)
            high = min(section['vram']+section['size'], ref['pc']+(after+1)*4)
            offset = section['rom_offset']+low-section['vram']
            if ref['kind'] == 'data-pointer':
                print('    data word: ' + rom[ref['rom_offset']:ref['rom_offset']+4].hex())
                continue
            print('    MIPS context (a window may include unowned bytes; verify control flow):')
            for instruction in decode(rom[offset:offset+high-low], low):
                marker = '>' if instruction.pc == ref['pc'] else ' '
                print(f'    {marker} {instruction.pc:08x} {instruction.word:08x} {instruction.assembly}')
        if not target['references']:
            print('  No references found in owned instructions or the supplied data ranges.')
    print(f"\n{len(report['targets'])} targets; no discovery metadata was changed.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--metadata', required=True, type=Path, help='matching symbols.json')
    image = parser.add_mutually_exclusive_group(required=True)
    image.add_argument('--game', choices=GAMES, help='prepare and verify the pinned ROM automatically')
    image.add_argument('--rom', type=Path, help='exact normalized/prepared analysis image')
    parser.add_argument('--target', action='append', type=number, default=[], help='inspect references to this address; repeatable')
    parser.add_argument('--text', action='append', default=[], metavar='SECTION:START:END', help='candidate target text range; end exclusive; repeatable')
    parser.add_argument('--data', action='append', default=[], metavar='SECTION:START:END', help='scan this data range for pointers; end exclusive; repeatable')
    parser.add_argument('--section', help='target section ID/name when guest mappings overlap')
    parser.add_argument('--include-owned', action='store_true', help='also list already owned targets during candidate discovery')
    parser.add_argument('--before', type=int, default=12, help='context words before each reference')
    parser.add_argument('--after', type=int, default=2, help='context words after each reference, including the delay slot')
    parser.add_argument('--json', type=Path, help='also write the report as JSON')
    args = parser.parse_args()
    if not 0 <= args.before <= 256 or not 1 <= args.after <= 256:
        raise RecompError('context requires before=0..256 and after=1..256')
    if not args.target and not args.text:
        raise RecompError('supply --target, or explicit --text bounds for candidate discovery')
    if args.rom:
        rom = args.rom.read_bytes()
        metadata = load_metadata(args.metadata, rom)
    else:
        rom, metadata = load_analysis_rom(args.game, args.metadata)
    text = [mapped_range(metadata, value) for value in args.text]
    if not text:
        selected = select_section(metadata, args.section)['id'] if args.section else None
        for address in args.target:
            matches = [s for s in metadata['sections'] if s['vram'] <= address < s['vram']+s['size']
                       and (selected is None or s['id'] == selected)]
            if len(matches) != 1:
                raise RecompError(f'target {address:#010x} has no unique mapping; supply --section')
            section = matches[0]
            if not any(r['section'] == section['id'] for r in text):
                text.append(dict(section=section['id'], vram=section['vram'],
                                 rom_offset=section['rom_offset'], size=section['size']))
    data = [mapped_range(metadata, value) for value in args.data]
    report = inspect_callbacks(rom, metadata, text, data, set(args.target), args.section, args.include_owned)
    print_report(report, rom, metadata, args.before, args.after)
    if args.json:
        args.json.write_text(json.dumps(report, indent=2)+'\n')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RecompError, OSError, ValueError, KeyError, TypeError, ImportError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
