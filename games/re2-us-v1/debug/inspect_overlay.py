#!/usr/bin/env python3
"""Inspect an RE2 overlay record and optionally compare a private crash RAM image.

Reports evidence only; does not modify the profile, inventory or runtime mappings.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import tomllib
import zlib

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME.parents[1] / 'tools'))
from common import RecompError, load_rom


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--index', type=int, action='append', required=True, help='overlay record index; repeatable')
    parser.add_argument('--rom', type=Path, help='original cartridge image; defaults to the profile ROM')
    parser.add_argument('--ram', type=Path, help='word-swapped RDRAM from the shared crash reporter')
    parser.add_argument('--json', type=Path, help='optional private report path under build/')
    args = parser.parse_args()
    profile = tomllib.loads((GAME / 'profile.toml').read_text())
    game = profile['game']
    rom, _ = load_rom(args.rom or (GAME / game['rom']), game['rom_sha1'], game['rom_size'])
    if rom[0x12cd0:0x12ce0] != b'OVERLAYTABLENUMC':
        raise RecompError('overlay table marker mismatch')
    ram = args.ram.read_bytes() if args.ram else None
    if ram is not None:
        if len(ram) != game['ram_size']:
            raise RecompError('snapshot size does not match RDRAM')
        ram = b''.join(ram[i:i+4][::-1] for i in range(0, len(ram), 4))
    records = []
    for index in args.index:
        if not 1 <= index <= 38:
            raise RecompError('record index must be 1..38')
        record = 0x12cd0 + index * 16
        cart, compressed, start, end = struct.unpack_from('>4I', rom, record)
        offset, physical = cart - 0xb0000000, start & 0x1fffffff
        if (not 0 <= offset < len(rom) or not 2 < compressed <= len(rom) - offset
                or not 0 < end - start <= game['ram_size'] - physical):
            raise RecompError(f'invalid/empty overlay record {index}')
        decoder = zlib.decompressobj(-15)
        data = decoder.decompress(rom[offset+2:offset+compressed], end-start+1)
        if not decoder.eof or len(data) != end-start:
            raise RecompError(f'overlay {index} did not decode to its complete declared size')
        item = dict(index=index, record_offset=record, rom_offset=offset,
                    compressed_size=compressed, vram=start, size=len(data),
                    sha256=hashlib.sha256(data).hexdigest())
        if ram is not None:
            observed = ram[physical:physical+len(data)]
            item.update(ram_matches=observed == data, ram_sha256=hashlib.sha256(observed).hexdigest())
        records.append(item)
    result = dict(rom_sha1=game['rom_sha1'], records=records)
    text = json.dumps(result, indent=2) + '\n'
    if args.json:
        args.json.write_text(text)
    print(text, end='')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RecompError, zlib.error) as error:
        sys.exit(f'error: {error}')
