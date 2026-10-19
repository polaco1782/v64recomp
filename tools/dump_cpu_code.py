#!/usr/bin/env python3
"""Dump verified N64 CPU text as binary, disassembly and an address manifest."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import sys
import tempfile

from code_inventory import load_inventory
from common import RecompError, load_profile, load_rom, write_json
from emit import decode
from export_analysis_rom import load_analysis_rom
from generate_game import GAMES, PROJECT


def cpu_ranges(image: bytes, metadata: dict, inventory: Path | None,
               allow_partial: bool) -> tuple[list[dict], bool]:
    """Select classified text, or explicitly owned instructions in partial mode."""
    sections = {s['id']: s for s in metadata['sections']}
    result = []

    def add(section, address, size, kind):
        offset = section['rom_offset'] + address - section['vram']
        if (address % 4 or size <= 0 or size % 4 or address < section['vram']
                or address+size > section['vram']+section['size']
                or offset < 0 or offset+size > len(image)):
            raise RecompError('invalid CPU dump range')
        result.append(dict(section=section['id'], section_name=section['name'],
                           vram=address, image_offset=offset, size=size, kind=kind))

    if inventory:
        classified, _ = load_inventory(inventory, image, list(sections.values()))
        by_name = {s['name']: s for s in sections.values()}
        for entry in classified:
            if entry['kind'] in ('code', 'native_os'):
                add(by_name[entry['section']], entry['vram'], entry['size'], entry['kind'])
    else:
        if not allow_partial:
            raise RecompError('no complete checked code/data inventory for this game; '
                              'use --allow-partial to dump discovered instructions only')
        for function in metadata['functions']:
            words = function.get('instruction_addresses')
            if not words:
                raise RecompError('partial dumping requires explicit instruction ownership')
            start = previous = words[0]
            for pc in words[1:]:
                if pc != previous+4:
                    add(sections[function['section']], start, previous+4-start, 'discovered')
                    start = pc
                previous = pc
            add(sections[function['section']], start, previous+4-start, 'discovered')
    if not result:
        raise RecompError('no verified CPU text found')
    return sorted(result, key=lambda r: (r['image_offset'], r['section'], r['vram'])), bool(inventory)


def write_listing(path: Path, image: bytes, ranges: list[dict], functions: list[dict],
                  heading: str) -> None:
    labels = {(f['section'], f['vram']): f['name'] for f in functions}
    with path.open('w') as stream:
        stream.write(f'# {heading}\n# PC        Image offset  Word      MIPS\n')
        for entry in ranges:
            stream.write(f"\n# Section {entry['section_name']} ({entry['kind']}), "
                         f"{entry['size']:#x} bytes\n")
            offset = entry['image_offset']
            # Bound decoding memory even for large contiguous text inventories.
            for chunk in range(0, entry['size'], 0x10000):
                raw = image[offset+chunk:offset+min(chunk+0x10000, entry['size'])]
                for instruction in decode(raw, entry['vram']+chunk):
                    label = labels.get((entry['section'], instruction.pc))
                    if label:
                        stream.write(f'\n{label}:\n')
                    source = offset + instruction.pc - entry['vram']
                    stream.write(f'{instruction.pc:08x}  {source:08x}      '
                                 f'{instruction.word:08x}  {instruction.assembly}\n')


def publish(staging: Path, output: Path) -> None:
    """Refuse differing existing files, including before publishing any outputs."""
    if output.exists() and not output.is_dir():
        raise RecompError('--output must be a directory')
    files = sorted(staging.iterdir())
    for source in files:
        target = output/source.name
        if target.exists() and (not target.is_file() or target.read_bytes() != source.read_bytes()):
            raise RecompError(f'output contains different content: {target}; choose a new directory')
    output.mkdir(parents=True, exist_ok=True)
    for source in files:
        target = output/source.name
        if not target.exists():
            with source.open('rb') as reader, target.open('xb') as writer:
                shutil.copyfileobj(reader, writer)


def dump_cpu_code(game: str, metadata_path: Path, output: Path, allow_partial: bool = False) -> dict:
    profile = load_profile(PROJECT/'games'/game/'profile.toml')
    identity = profile['game']
    original, _ = load_rom(identity['rom'], identity['rom_sha1'], identity['rom_size'])
    image, metadata = load_analysis_rom(game, metadata_path)
    ranges, complete = cpu_ranges(image, metadata, profile['discovery'].get('inventory'), allow_partial)
    sections = {s['id']: s for s in metadata['sections']}
    direct = {s['id']: image[s['rom_offset']:s['rom_offset']+s['size']] ==
              original[s['rom_offset']:s['rom_offset']+s['size']] for s in sections.values()}
    payload = bytearray()
    for entry in ranges:
        offset, size = entry['image_offset'], entry['size']
        raw = image[offset:offset+size]
        entry.update(dump_offset=len(payload), sha256=hashlib.sha256(raw).hexdigest(),
                     original_rom_offset=offset if direct[entry['section']] else None)
        payload.extend(raw)
    if len(original) < 0x1000:
        raise RecompError('original cartridge has no complete IPL3 boot block')
    boot = original[0x40:0x1000]
    manifest = dict(format_version=1, game=game, original_rom_sha1=identity['rom_sha1'],
                    analysis_sha1=metadata['rom_sha1'], metadata=str(metadata_path.resolve()),
                    complete_mapped_inventory=complete, cpu_bytes=len(payload),
                    cpu_words=len(payload)//4, cpu_sha256=hashlib.sha256(payload).hexdigest(),
                    byte_order='big-endian', ranges=ranges,
                    scope='Complete configured CPU text inventory' if complete else 'Partial discovered CPU instructions only',
                    limitations='Does not classify CPU code outside configured sections; excludes RSP and data. '
                                'IPL3 is exported separately as a boot block, which can include data.',
                    ipl3=dict(file='ipl3.bin', original_rom_offset=0x40, vram=0xa4000040,
                              size=len(boot), sha256=hashlib.sha256(boot).hexdigest()))
    with tempfile.TemporaryDirectory(prefix='v64-cpu-dump-') as temp:
        staging = Path(temp)
        (staging/'cpu.bin').write_bytes(payload)
        (staging/'ipl3.bin').write_bytes(boot)
        write_listing(staging/'cpu.asm', image, ranges, metadata['functions'], manifest['scope'])
        write_listing(staging/'ipl3.asm', original,
                      [dict(section=-1, section_name='IPL3', kind='boot block; may include data',
                            image_offset=0x40, vram=0xa4000040, size=len(boot))], [],
                      'Raw IPL3 boot-block disassembly; words are not individually classified as instructions')
        write_json(staging/'cpu.json', manifest)
        publish(staging, output)
    print(f"{game}: {manifest['scope']}; {len(payload):,} CPU bytes in {len(ranges)} ranges")
    print(f'Binary: {output / "cpu.bin"}\nDisassembly: {output / "cpu.asm"}\n'
          f'Manifest: {output / "cpu.json"}\nIPL3: {output / "ipl3.bin"} and ipl3.asm')
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, choices=GAMES)
    parser.add_argument('--metadata', required=True, type=Path, help='matching generated symbols.json')
    parser.add_argument('--output', required=True, type=Path, help='directory for cpu.bin, cpu.asm and cpu.json')
    parser.add_argument('--allow-partial', action='store_true', help='allow discovered instructions without a complete inventory')
    args = parser.parse_args()
    try:
        dump_cpu_code(args.game, args.metadata, args.output, args.allow_partial)
    except (RecompError, OSError, ValueError, KeyError, TypeError, ImportError) as error:
        print(f'CPU dump failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
