#!/usr/bin/env python3
"""ROM-bound USO relocation evidence; never registers discovered code automatically."""
from collections import defaultdict, Counter
from pathlib import Path
import struct
import hashlib
import json

import argparse
import tomllib

parser = argparse.ArgumentParser(description='Verify a 1080 USO against crash RAM and export relocation evidence.')
parser.add_argument('--rom', type=Path, required=True)
parser.add_argument('--crash', type=Path, required=True)
parser.add_argument('--module-offset', type=lambda s: int(s, 0), required=True)
parser.add_argument('--text-base', type=lambda s: int(s, 0), required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
profile = tomllib.loads(Path(__file__).parents[1].joinpath('profile.toml').read_text())
rom = args.rom.read_bytes()
crash = json.loads(args.crash.read_text())
if hashlib.sha1(rom).hexdigest() != profile['game']['rom_sha1'] or crash['rom_sha1'] != profile['game']['rom_sha1']:
    parser.error('ROM/crash identity mismatch')
# The shared reporter writes its internal word-swapped RDRAM immediately beside JSON.
if crash['rdram']['storage'] != 'word-swapped':
    parser.error('expected word-swapped crash RDRAM')
ram = Path(str(args.crash) + '.rdram.bin').read_bytes()
module, text_base = args.module_offset, args.text_base
position, sections = module + 4, {}
if rom[module:module + 4] != bytes.fromhex('12345678'):
    parser.error('USO marker mismatch')
for _ in range(16):
    if position + 12 > len(rom):
        parser.error('truncated USO directory')
    kind, size, flags = struct.unpack_from('>3I', rom, position)
    if kind == 11:
        break
    if kind in sections or kind > 13 or (kind != 8 and position + 12 + size > len(rom)):
        parser.error('invalid USO directory')
    sections[kind] = (position + 12, size)
    position += 12 + (0 if kind == 8 else (size + 3) & ~3)
else:
    parser.error('unterminated USO directory')
if 5 not in sections or 2 not in sections:
    parser.error('missing Text or relocation section')
text_rom, text_size = sections[5]
reloc_rom, reloc_size = sections[2]
if text_size % 4 or text_base % 4 or reloc_size % 12 or (text_base & 0x1fffffff) + text_size > len(ram):
    parser.error('invalid Text or relocation extent')
u32 = lambda p: struct.unpack_from('>I', rom, p)[0]
live = lambda p: int.from_bytes(ram[(p & 0x1fffffff):(p & 0x1fffffff)+4][::-1], 'big')
sx16 = lambda w: (w & 0xffff) - (0x10000 if w & 0x8000 else 0)
entries = []
by_symbol = defaultdict(list)
for r in range(reloc_rom, reloc_rom + reloc_size, 12):
    # Record fields: encoded symbol/type, Text offset, saved-word storage.
    f = u32(r)
    e = (f >> 4, f & 7, u32(r+4))
    if e[2] % 4 or e[2] + 4 > text_size:
        parser.error('relocation outside Text')
    entries.append(e)
    by_symbol[e[0]].append(e)

symbol_values = {}
source = Counter()
for sym, sites in by_symbol.items():
    jump = next((e for e in sites if e[1] == 1), None)
    pair = next(((hi, lo) for hi, lo in zip(sites, sites[1:]) if hi[1] == 3 and lo[1] == 2), None)
    from_jump = None
    from_pair = None
    if jump:
        _, _, p = jump
        raw, patched = u32(text_rom+p), live(text_base+p)
        from_jump = 0x80000000 | (((patched & 0x3ffffff) - (raw & 0x3ffffff)) << 2 & 0x0fffffff)
    if pair:
        hi, lo = pair
        rh, rl = u32(text_rom+hi[2]), u32(text_rom+lo[2])
        ph, pl = live(text_base+hi[2]), live(text_base+lo[2])
        from_pair = (((ph & 0xffff) << 16) + sx16(pl) - ((rh & 0xffff) << 16) - sx16(rl)) & 0xffffffff
    if from_jump is not None and from_pair is not None and (from_jump & 0x0fffffff) != (from_pair & 0x0fffffff):
        parser.error(f'symbol {sym} has conflicting relocation evidence')
    if from_pair is not None:
        symbol_values[sym] = from_pair
        source['pair'] += 1
    elif from_jump is not None:
        symbol_values[sym] = from_jump
        source['jump'] += 1
    else:
        parser.error(f'symbol {sym} has no complete address evidence')

print('symbol source', source, 'symbols', len(symbol_values))
print('value categories', Counter((v >> 28) for v in symbol_values.values()))

# Emulate the exact immediate operations of the original relocation routine.
patched = bytearray(rom[text_rom:text_rom+text_size])
def write(p, word):
    struct.pack_into('>I', patched, p, word & 0xffffffff)
pending_hi = None
mismatches = []
for sym, kind, p in entries:
    value = symbol_values[sym]
    word = u32(text_rom+p)
    if kind == 1:
        result = (word & 0xfc000000) | (((word & 0x3ffffff) + (value >> 2)) & 0x3ffffff)
        write(p, result)
    elif kind == 3:
        if pending_hi is not None:
            parser.error('overlapping HI relocations')
        pending_hi = (sym, p, word)
        continue
    elif kind == 2:
        result = (word & 0xffff0000) | ((word + value) & 0xffff)
        write(p, result)
        if pending_hi:
            hs, hp, hw = pending_hi
            if hs != sym:
                parser.error('HI/LO symbol mismatch')
            full = ((hw & 0xffff) << 16) + sx16(word) + value
            high = (full + 0x8000) >> 16
            write(hp, (hw & 0xffff0000) | (high & 0xffff))
            pending_hi = None
    elif kind == 4:
        write(p, word + value)
    else:
        parser.error(f'unknown relocation kind {kind}')

for p in range(0, len(patched), 4):
    actual = live(text_base+p)
    expected = struct.unpack_from('>I', patched, p)[0]
    if expected != actual:
        mismatches.append((p, expected, actual))
print('text mismatches', len(mismatches), 'of', len(patched)//4)
print('first mismatches', [(hex(p),hex(x),hex(y)) for p,x,y in mismatches[:35]])
print('pending_hi',pending_hi)
if mismatches or pending_hi:
    parser.error('relocated Text does not match the complete guest image')
inventory = {
    'format_version': 1,
    'rom_sha1': hashlib.sha1(rom).hexdigest(),
    'module_offset': module,
    'text_rom_offset': text_rom,
    'text_base': text_base,
    'text_size': len(patched),
    'raw_text_sha256': hashlib.sha256(rom[text_rom:text_rom+len(patched)]).hexdigest(),
    'reloc_rom_offset': reloc_rom,
    'reloc_count': len(entries),
    'reloc_sha256': hashlib.sha256(rom[reloc_rom:reloc_rom+reloc_size]).hexdigest(),
    'prepared_text_sha256': hashlib.sha256(patched).hexdigest(),
    'symbol_values': {str(k): v for k, v in sorted(symbol_values.items())},
}
args.output.write_text(json.dumps(inventory, indent=2) + '\n')
print('prepared hash',inventory['prepared_text_sha256'])
