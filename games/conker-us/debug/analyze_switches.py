#!/usr/bin/env python3
"""Report bounded Conker switch candidates for review; never edits port metadata."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
from common import load_profile, load_rom
import importlib.util


def candidates(image, metadata):
    def read(address, size):
        regions = [s for s in metadata['sections'] if s['vram'] <= address and
                   address + size <= s['vram'] + s['size']]
        if len(regions) != 1:
            raise ValueError(f'unmapped/ambiguous switch span: {address:#x}')
        s = regions[0]
        offset = s['rom_offset'] + address - s['vram']
        return image[offset:offset+size]

    found = []
    for f in metadata['functions']:
        owned = set(f['instruction_addresses'])
        for pc in sorted(owned):
            if pc - 12 not in owned:
                continue
            hi, add, load, jump = struct.unpack('>4I', read(pc-12, 16))
            reg = jump >> 21 & 31
            if jump != reg << 21 | 8 or reg == 31:
                continue
            # lui at,hi; addu at,at,index; lw target,lo(at); jr target
            if hi >> 16 != 0x3c01 or add != 0x00200821 | reg << 16 or load >> 16 != 0x8c20 | reg:
                continue
            bounds = []
            for earlier in range(max(f['vram'], pc-96), pc-12, 4):
                if earlier not in owned:
                    continue
                w = int.from_bytes(read(earlier, 4), 'big')
                # sltiu at,index,count followed by beqz at,default;
                # index is shifted by two before the address addition.
                if w >> 16 != 0x2c01 | reg << 5:
                    continue
                count = w & 65535
                branch = int.from_bytes(read(earlier+4, 4), 'big')
                between = [int.from_bytes(read(p, 4), 'big') for p in range(earlier+8, pc-12, 4)]
                shift = reg << 16 | reg << 11 | 2 << 6
                if not 0 < count < 1024 or branch >> 16 != 0x1020 or shift not in between:
                    continue
                bounds.append((earlier, count))
            if len(bounds) != 1:
                continue
            bound_pc, count = bounds[0]
            lo = load & 65535
            table = ((hi & 65535) << 16) + (lo if lo < 32768 else lo-65536)
            raw = read(table, count*4)
            targets = struct.unpack(f'>{count}I', raw)
            if any(t % 4 or not any(s['name'] in ('.init', '.game') and
                       s['vram'] <= t < s['vram'] + s['size'] for s in metadata['sections']) for t in targets):
                continue
            found.append(dict(pc=pc, expected_word=jump, table_vram=table, count=count,
                              sha256=hashlib.sha256(raw).hexdigest(), bound_pc=bound_pc,
                              function=f['vram'], targets=sorted(set(targets))))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('symbols', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    profile = load_profile(ROOT / 'games/conker-us/profile.toml')
    rom, _ = load_rom(profile['game']['rom'], profile['game']['rom_sha1'], profile['game']['rom_size'])
    spec = importlib.util.spec_from_file_location('conker_generate', ROOT / 'games/conker-us/generate.py')
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    image, _ = generator.prepare(rom, profile, args.output.parent)
    metadata = json.loads(args.symbols.read_text())
    if metadata['rom_sha1'] != hashlib.sha1(image).hexdigest():
        raise ValueError('metadata belongs to another prepared image')
    found = candidates(image, metadata)
    args.output.write_text(json.dumps(found, indent=2)+'\n')
    print(f'{len(found)} bounded switch candidates; review {args.output}')


if __name__ == '__main__':
    main()
