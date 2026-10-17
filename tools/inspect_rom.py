#!/usr/bin/env python3
"""Identify an N64 ROM; optionally disassemble and fingerprint a mapped byte range."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys

from common import RecompError, load_rom


def inspect_rom(path: Path, offset: int | None = None, vram: int | None = None,
                size: int = 64) -> None:
    rom, identity = load_rom(path)
    print(f"Input order: {identity['input_order']}")
    print(f"Size: {len(rom):#x} ({len(rom)} bytes)")
    print(f"Normalized SHA-1: {identity['sha1']}")
    print(f"Normalized SHA-256: {hashlib.sha256(rom).hexdigest()}")
    print(f"Title: {rom[0x20:0x34].decode('ascii', errors='replace').rstrip(chr(0) + ' ')}")
    print(f"Header entry: {int.from_bytes(rom[8:12], 'big'):#010x}")
    print(f"Country byte: {rom[0x3e]:#04x}; revision byte: {rom[0x3f]}")
    if offset is None:
        if vram is not None:
            raise RecompError('--vram requires --offset')
        return
    if offset < 0 or size <= 0 or offset + size > len(rom):
        raise RecompError('requested range lies outside the ROM')
    raw = rom[offset:offset + size]
    print(f"Range ROM {offset:#x}..{offset + size:#x} (end exclusive)")
    print(f"Range SHA-256: {hashlib.sha256(raw).hexdigest()}")
    if vram is None:
        for i in range(0, size, 16):
            print(f"  ROM {offset + i:08x}: {raw[i:i + 16].hex(' ')}")
        return
    if offset % 4 or vram < 0 or vram % 4 or size % 4 or vram + size > 0x100000000:
        raise RecompError('disassembly needs aligned offsets, VRAM and size in the 32-bit address space')
    # Identity and byte inspection do not need Rabbitizer.
    from emit import decode
    print('Disassembly assumes CPU code and the supplied mapping; it does not discover either.')
    for instruction in decode(raw, vram):
        print(f"  {instruction.pc:08x}  ROM {offset + instruction.pc - vram:08x}"
              f"  {instruction.word:08x}  {instruction.assembly}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rom', type=Path)
    parser.add_argument('--offset', type=lambda value: int(value, 0), help='normalized ROM offset')
    parser.add_argument('--vram', type=lambda value: int(value, 0), help='guest address at --offset')
    parser.add_argument('--size', type=lambda value: int(value, 0), default=64, help='bytes, default 64')
    args = parser.parse_args()
    inspect_rom(args.rom, args.offset, args.vram, args.size)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RecompError, OSError, ValueError, ImportError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
