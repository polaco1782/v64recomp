#!/usr/bin/env python3
"""Inspect an OSTask and its bytes in a shared crash reporter's RDRAM snapshot."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys

from common import RecompError

FIELDS = ('type', 'flags', 'boot', 'boot_size', 'ucode', 'ucode_size',
          'ucode_data', 'ucode_data_size', 'dram_stack', 'dram_stack_size',
          'output', 'output_size_ptr', 'display_list', 'display_list_size',
          'yield_data', 'yield_data_size')


def integer(value):
    return int(value, 0) if isinstance(value, str) else int(value)


class Snapshot:
    def __init__(self, path: Path):
        self.report = json.loads(path.read_text())
        if self.report.get('format_version') != 1:
            raise RecompError('unsupported crash report version')
        info = self.report['rdram']
        if not info.get('saved'):
            raise RecompError('crash report has no saved RDRAM')
        memory_path = Path(info['path'])
        if not memory_path.is_absolute() and not memory_path.is_file():
            memory_path = path.parent / memory_path
        raw = memory_path.read_bytes()
        if len(raw) != info['size'] or len(raw) % 4:
            raise RecompError('snapshot size does not match the report')
        if info['storage'] == 'word-swapped':
            ram = bytearray(len(raw))
            for i in range(4):
                ram[i::4] = raw[3-i::4]
            self.ram = bytes(ram)
        elif info['storage'] == 'big-endian':
            self.ram = raw
        else:
            raise RecompError('unsupported RDRAM storage order')

    def read(self, address: int, size: int) -> bytes:
        physical = address & 0x1fffffff if 0x80000000 <= address < 0xc0000000 else address
        if size < 0 or physical < 0 or physical+size > len(self.ram):
            raise RecompError(f'unmapped snapshot range {address:#x}+{size:#x}; TLB aliases need explicit translation')
        return self.ram[physical:physical+size]

    def task(self) -> list[int]:
        task = self.report.get('task')
        words = task['descriptor'] if task else self.report.get('task_words', [])
        if len(words) != 16:
            raise RecompError('report does not contain a complete OSTask')
        words = [integer(w) for w in words]
        if any(not 0 <= w <= 0xffffffff for w in words):
            raise RecompError('OSTask has a non-word value')
        return words


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--rom', type=Path, help='normalized original ROM; show exact byte matches')
    parser.add_argument('--list-scale', type=lambda x: int(x, 0), default=1,
                        help='bytes per task list unit: 1 for bytes, 8 for commands, 0xa10 for MusyX v1 subframes')
    parser.add_argument('--structured', action='store_true',
                        help='list contains descriptors; omit command/opcode interpretation')
    parser.add_argument('--commands', type=int, default=32, help='maximum flat command rows to print')
    parser.add_argument('--opcode', type=lambda x: int(x, 0), help='print only this opcode')
    parser.add_argument('--extract', type=Path, help='write microcode and graphics/audio command bytes here')
    args = parser.parse_args()
    if not 1 <= args.list_scale <= 65536:
        raise RecompError('invalid task list scale')
    if not 0 <= args.commands <= 65536 or args.opcode is not None and not 0 <= args.opcode <= 255:
        raise RecompError('invalid command count or opcode')
    snapshot = Snapshot(args.report)
    words = snapshot.task()
    for name, word in zip(FIELDS, words):
        print(f'{name:20s} {word:#010x} ({word})')
    rom = args.rom.read_bytes() if args.rom else None
    if rom is not None and hashlib.sha1(rom).hexdigest() != snapshot.report['rom_sha1']:
        raise RecompError('ROM identity differs from the crash report')
    regions = [('ucode', words[4], words[5] or 0x1000),
               ('ucode-data', words[6], words[7])]
    list_name = 'display-list' if words[0] == 1 else 'audio-list'
    if words[0] in (1, 2):
        regions.append((list_name, words[12], words[13]*args.list_scale))
    extracted = {}
    for name, address, size in regions:
        raw = snapshot.read(address, size)
        extracted[name] = raw
        print(f'\n{name}: {address:#010x}, {size:#x} bytes, SHA-256 {hashlib.sha256(raw).hexdigest()}')
        if rom is not None:
            match = rom.find(raw) if raw else -1
            print(f'  Exact ROM match: {match:#x}' if match >= 0 else '  No exact ROM match (bytes may be mutable or loaded/decompressed).')
        if name == 'ucode-data':
            for text in re.findall(rb'[ -~]{12,}', raw):
                print('  String: ' + text.decode('ascii'))
    raw = extracted.get(list_name)
    if raw is not None and not args.structured:
        if len(raw) % 8:
            raise RecompError('command-list span is not a multiple of 8; check --list-scale')
        counts = Counter(raw[i] for i in range(0, len(raw), 8))
        print('\nFlat opcode counts (nested lists are not followed):')
        print('  ' + ' '.join(f'{op:02x}:{count}' for op, count in sorted(counts.items())))
        printed = 0
        for i in range(0, len(raw), 8):
            if printed >= args.commands:
                break
            if args.opcode is None or raw[i] == args.opcode:
                print(f'{words[12]+i:08x}: {raw[i:i+4].hex()} {raw[i+4:i+8].hex()}')
                printed += 1
    if args.extract:
        args.extract.mkdir(parents=True, exist_ok=True)
        for name, raw in extracted.items():
            (args.extract/(name+'.bin')).write_bytes(raw)
        print(f'\nExtracted private task bytes to {args.extract}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RecompError, OSError, ValueError, KeyError, TypeError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
