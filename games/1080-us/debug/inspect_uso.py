#!/usr/bin/env python3
"""ROM-bound USO relocation evidence; never registers discovered code automatically."""
from collections import defaultdict, Counter
from pathlib import Path
import struct
import hashlib
import json
import sys

import argparse
import tomllib

sys.path[:0] = [str(Path(__file__).parents[1]), str(Path(__file__).parents[3] / 'tools')]
from common import RecompError
from uso import sections_at, section_bytes

parser = argparse.ArgumentParser(description='Verify a 1080 USO against crash RAM and export relocation evidence.')
parser.add_argument('--rom', type=Path, required=True)
parser.add_argument('--crash', type=Path, required=True)
parser.add_argument('--module-offset', type=lambda s: int(s, 0))
parser.add_argument('--text-base', type=lambda s: int(s, 0))
parser.add_argument('--header-word', type=lambda s: int(s, 0), default=0x12345678)
parser.add_argument('--target', type=lambda s: int(s, 0), help='locate a module from a unique ROM-matching target prefix')
parser.add_argument('--prefix-offset', type=lambda s: int(s, 0), default=0,
                    help='use a later unrelocated prefix relative to --target')
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
header_word = args.header_word
if args.header_word != 0x12345678 and module is None:
    parser.error('a nonstandard header word requires --module-offset')
if args.target is not None:
    if text_base is not None:
        parser.error('--target cannot be combined with --text-base')
    if not 0 <= args.prefix_offset <= 0x1000 or args.prefix_offset & 3:
        parser.error('prefix offset must be aligned and within 0x1000 bytes of the target')
    target_offset = (args.target + args.prefix_offset) & 0x1fffffff
    if args.target & 3 or target_offset + 16 > len(ram):
        parser.error('target prefix is outside aligned RDRAM')
    prefix = b''.join(ram[p:p+4][::-1] for p in range(target_offset, target_offset + 16, 4))
    candidates, positions, position = [], [], 0
    if module is not None:
        positions = [(module, header_word)]
    else:
        while (position := rom.find(bytes.fromhex('12345678'), position)) >= 0:
            positions.append((position, 0x12345678))
            # Named filesystem records have a 16-byte filename and a 40-byte
            # header. Some ordinary USOs use a zero inner header word.
            name = rom[position + 12:position + 28].split(b'\0', 1)[0]
            if (rom[position + 8:position + 12] == b'\0\0\0\1'
                    and name.endswith(b'.uso') and all(32 <= c < 127 for c in name)
                    and rom[position + 40:position + 44] == bytes(4)):
                positions.append((position + 40, 0))
            position += 4
    for position, candidate_header in positions:
        try:
            directory = sections_at(rom, position, candidate_header)
            if 5 in directory and 2 in directory:
                text = section_bytes(rom, directory[5])
                offset = 0
                while (offset := text.find(prefix, offset)) >= 0:
                    base = args.target + args.prefix_offset - offset
                    if not base & 3 and (base & 0x1fffffff) + len(text) <= len(ram):
                        candidates.append((position, base, candidate_header))
                    offset += 1
        except RecompError:
            pass
    if len(candidates) != 1:
        details = ', '.join(f'{offset:#x}:{base:#x}:header={word:#x}' for offset, base, word in candidates)
        parser.error(f'target prefix identifies {len(candidates)} module mappings ({details}); supply --module-offset and --text-base')
    module, text_base, header_word = candidates[0]
elif module is None or text_base is None:
    parser.error('supply --target or both --module-offset and --text-base')
try:
    sections = sections_at(rom, module, header_word)
except RecompError as error:
    parser.error(str(error))
print(f'module ROM {module:#x}, Text ROM {sections.get(5, (0,))[0]:#x}, Text base {text_base:#x}')
if 5 not in sections or 2 not in sections:
    parser.error('missing Text or relocation section')
text_rom, stored_size, flags = sections[5]
try:
    raw_text = section_bytes(rom, sections[5])
except RecompError as error:
    parser.error(str(error))
text_size = len(raw_text)
reloc_rom, reloc_size, _ = sections[2]
if text_size % 4 or text_base % 4 or reloc_size % 12 or (text_base & 0x1fffffff) + text_size > len(ram):
    parser.error('invalid Text or relocation extent')
u32 = lambda p: struct.unpack_from('>I', rom, p)[0]
text_word = lambda p: struct.unpack_from('>I', raw_text, p)[0]
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
        raw, patched = text_word(p), live(text_base+p)
        from_jump = 0x80000000 | (((patched & 0x3ffffff) - (raw & 0x3ffffff)) << 2 & 0x0fffffff)
    if pair:
        hi, lo = pair
        rh, rl = text_word(hi[2]), text_word(lo[2])
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
patched = bytearray(raw_text)
def write(p, word):
    struct.pack_into('>I', patched, p, word & 0xffffffff)
pending_hi = None
mismatches = []
for sym, kind, p in entries:
    value = symbol_values[sym]
    word = text_word(p)
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
    'raw_text_sha256': hashlib.sha256(raw_text).hexdigest(),
    'reloc_rom_offset': reloc_rom,
    'reloc_count': len(entries),
    'reloc_sha256': hashlib.sha256(rom[reloc_rom:reloc_rom+reloc_size]).hexdigest(),
    'prepared_text_sha256': hashlib.sha256(patched).hexdigest(),
    'symbol_values': {str(k): v for k, v in sorted(symbol_values.items())},
}
if flags & 0x10000:
    inventory['text_rom_size'] = stored_size
    inventory['stored_text_sha256'] = hashlib.sha256(rom[text_rom:text_rom + stored_size]).hexdigest()
if header_word != 0x12345678:
    inventory['header_word'] = header_word
args.output.write_text(json.dumps(inventory, indent=2) + '\n')
print('prepared hash',inventory['prepared_text_sha256'])
