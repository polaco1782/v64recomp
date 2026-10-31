#!/usr/bin/env python3
"""Embed an offset-preserving ROM image with verified CPU instructions erased."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys

from code_inventory import load_inventory
from common import RecompError, load_profile, load_rom, write_json
from export_analysis_rom import load_analysis_rom
from generate_game import GAMES, PROJECT


def ranges_from_mask(mask: bytearray) -> list[dict]:
    ranges = []
    start = None
    for offset, marked in enumerate(mask):
        if marked and start is None:
            start = offset
        elif not marked and start is not None:
            ranges.append(dict(rom_offset=start, size=offset-start))
            start = None
    if start is not None:
        ranges.append(dict(rom_offset=start, size=len(mask)-start))
    return ranges


def strip_rom(rom: bytes, analysis: bytes, metadata: dict, profile: dict) -> tuple[bytes, dict]:
    if profile.get('packaging', {}).get('allow_embedding', True) is False:
        raise RecompError('this demo uses an external ROM; embedding is disabled by its profile')
    # Appended/repacked analysis offsets cannot be applied to the cartridge.
    # In particular, zeroing compressed code can also destroy initialized data.
    if analysis != rom:
        raise RecompError('embedded ROM stripping requires an unchanged analysis image; '
                          'this compressed/repacked game needs a verified packaging adapter')
    identity = profile['game']
    if (len(rom) != identity['rom_size'] or hashlib.sha1(rom).hexdigest() != identity['rom_sha1']
            or metadata['rom_sha1'] != identity['rom_sha1'] or metadata['rom_size'] != len(rom)):
        raise RecompError('packaging inputs belong to different ROMs')
    mask = bytearray(len(rom))
    sections = {s['id']: s for s in metadata['sections']}
    protected = []

    def mark(offset, size, minimum=0x1000):
        if (type(offset) is not int or type(size) is not int or offset < minimum or offset % 4
                or size <= 0 or size % 4 or offset+size > len(rom)):
            raise RecompError('CPU removal range outside aligned cartridge code')
        mask[offset:offset+size] = b'\1' * size

    inventory_path = profile['discovery'].get('inventory')
    if inventory_path:
        inventory, _ = load_inventory(inventory_path, rom, list(sections.values()))
        by_name = {s['name']: s for s in sections.values()}
        for entry in inventory:
            section = by_name[entry['section']]
            offset = section['rom_offset'] + entry['vram'] - section['vram']
            if entry['kind'] in ('code', 'native_os'):
                mark(offset, entry['size'])
            else:
                protected.append((offset, entry['size']))
    else:
        # Function spans can contain literal pools and switch tables. Erase only
        # individually owned instruction words, including checked hook prefixes.
        for function in metadata['functions']:
            if 'instruction_addresses' not in function:
                raise RecompError('stripping needs explicit instruction ownership metadata')
            for pc in function['instruction_addresses']:
                mark(function['rom_offset'] + pc - function['vram'], 4)

    def checked_range(entry):
        offset, size = entry['rom_offset'], entry['size']
        if (type(offset) is not int or type(size) is not int or offset < 0 or size <= 0
                or offset+size > len(rom) or not isinstance(entry.get('reason'), str) or not entry['reason']
                or hashlib.sha256(rom[offset:offset+size]).hexdigest() != entry.get('sha256')):
            raise RecompError('invalid packaging range/fingerprint/reason')
        return offset, size

    # Explicit discarded ranges cover audited unused content outside discovery,
    # e.g. an IPL3 block bypassed by the native bootstrap. Keep the N64 header.
    discarded = profile.get('packaging', {}).get('discard', [])
    for entry in discarded:
        offset, size = checked_range(entry)
        mark(offset, size, minimum=64)
    for offset, size in protected:
        if any(mask[offset:offset+size]):
            raise RecompError('CPU removal overlaps retained data or RSP microcode')
    # Exceptions are for instructions read as data, checksums, etc.
    preserved = profile.get('packaging', {}).get('preserve', [])
    for entry in preserved:
        offset, size = checked_range(entry)
        mask[offset:offset+size] = bytes(size)
    removed = ranges_from_mask(mask)
    if not removed:
        raise RecompError('no verified CPU instructions to strip')
    payload = bytearray(rom)
    for entry in removed:
        offset, size = entry['rom_offset'], entry['size']
        payload[offset:offset+size] = bytes(size)
    report = dict(format_version=1, original_sha1=identity['rom_sha1'],
                  payload_sha1=hashlib.sha1(payload).hexdigest(), rom_size=len(rom),
                  removed_bytes=sum(e['size'] for e in removed), removed_ranges=removed,
                  preserved_ranges=preserved, discarded_ranges=discarded,
                  complete_mapped_inventory=bool(inventory_path),
                  scope='Verified CPU and explicit discard ranges only; header, unclassified bytes and RSP programs retained')
    return bytes(payload), report


def write_if_changed(path: Path, content: bytes) -> None:
    if not path.exists() or path.read_bytes() != content:
        path.write_bytes(content)


def package(game: str, metadata_path: Path, output: Path) -> None:
    profile = load_profile(PROJECT/'games'/game/'profile.toml')
    identity = profile['game']
    rom, _ = load_rom(identity['rom'], identity['rom_sha1'], identity['rom_size'])
    analysis, metadata = load_analysis_rom(game, metadata_path)
    payload, report = strip_rom(rom, analysis, metadata, profile)
    output.mkdir(parents=True, exist_ok=True)
    write_if_changed(output/'rom-assets.bin', payload)
    # Octal C string literals avoid target-specific assembler/linker formats.
    # Keep the implicit trailing NUL outside the logical cartridge size.
    lines = ['/* Private generated ROM content; do not distribute or commit. */',
             '#include "embedded_rom.h"',
             'const uint8_t recomp_embedded_rom[] =']
    for offset in range(0, len(payload), 64):
        lines.append('"'+''.join(f'\\{b:03o}' for b in payload[offset:offset+64])+'"')
    lines.extend([';', f'const size_t recomp_embedded_rom_size = {len(payload)};'])
    for name, digest in (('original_sha1', report['original_sha1']), ('sha1', report['payload_sha1'])):
        lines.append(f'const uint8_t recomp_embedded_{name}[20] = {{'+
                     ','.join(f'0x{b:02x}' for b in bytes.fromhex(digest))+'};')
    write_if_changed(output/'embedded_rom.c', ('\n'.join(lines)+'\n').encode())
    write_json(output/'rom-assets.json', report)
    print(f"{game}: embedded {len(payload):,} bytes; erased {report['removed_bytes']:,} verified CPU bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, choices=GAMES)
    parser.add_argument('--metadata', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        package(args.game, args.metadata, args.output)
    except (RecompError, OSError, ValueError, KeyError) as error:
        print(f'ROM packaging failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
