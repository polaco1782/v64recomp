#!/usr/bin/env python3
"""Report callback candidates and checked-shape monster animation tables.

Read-only: review producers, consumers and function boundaries before changing
startup-entries.json. The animation layout comes from 0x80036ee8: mmove_t is
{first, last, frames, endfunc}; each mframe_t is {ai, distance, think}.
"""
import argparse
import hashlib
import math
from pathlib import Path
import struct
import sys

PROJECT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(PROJECT / 'tools'))
from common import load_metadata, load_profile, load_rom, write_json
from metadata_candidates import materialized_entries, pointer_entries


def analyze(rom, metadata, profile):
    section = metadata['sections'][0]
    text = [dict(section, size=0x51580)]
    data = [dict(section, vram=0x80064320, rom_offset=0x64f20, size=0x7780)]

    def raw(address, size):
        offset = section['rom_offset'] + address - section['vram']
        return rom[offset:offset + size]

    def callback(address):
        return address == 0 or 0x80000400 <= address < 0x80051980 and address % 4 == 0

    moves = []
    for address in range(0x80064320, 0x8006baa0 - 15, 4):
        first, last, frames, end = struct.unpack('>4I', raw(address, 16))
        count = last - first + 1
        if not (first <= last < 1000 and frames % 4 == 0
                and 0x80067110 <= frames and frames + count * 12 <= 0x8006baa0
                and callback(end)):
            continue
        callbacks = []
        for index in range(count):
            site = frames + index * 12
            ai, distance, think = struct.unpack('>IfI', raw(site, 12))
            if not (callback(ai) and callback(think) and math.isfinite(distance)
                    and abs(distance) <= 1024):
                break
            callbacks.extend(dict(vram=value, pointer_source=site + offset)
                             for offset, value in ((0, ai), (8, think)) if value)
        else:
            if end:
                callbacks.append(dict(vram=end, pointer_source=address + 12))
            moves.append(dict(vram=address, first=first, last=last, frames=frames,
                endfunc=end, sha256=hashlib.sha256(raw(address, 16)).hexdigest(),
                frames_sha256=hashlib.sha256(raw(frames, count * 12)).hexdigest(),
                callbacks=callbacks))
    return dict(format_version=1, rom_sha1=metadata['rom_sha1'], complete=False,
        warning='Candidates require pointer-use and full function-boundary review.',
        materialized=materialized_entries(rom, metadata, text),
        pointers=pointer_entries(rom, metadata, text, data,
                                profile['discovery']['jump_tables']),
        animation_moves=moves)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--metadata', type=Path,
        default=PROJECT / 'build/quake2-us-debug/generated/quake2-us/symbols.json')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    profile = load_profile(PROJECT / 'games/quake2-us/profile.toml')
    rom, _ = load_rom(profile['game']['rom'], profile['game']['rom_sha1'],
                      profile['game']['rom_size'])
    report = analyze(rom, load_metadata(args.metadata, rom), profile)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_json(args.output, report)
    print(f"{len(report['materialized'])} materialized and {len(report['pointers'])} "
          f"pointer candidates; {len(report['animation_moves'])} animation descriptors")


if __name__ == '__main__':
    main()
