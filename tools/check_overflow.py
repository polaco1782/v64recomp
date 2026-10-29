#!/usr/bin/env python3
"""Check signed ADD/DADD/SUB results and traps against Python integers, without a ROM.

Run from a Developer PowerShell for --cc cl; --cc clang also supports Linux.
Both USE_INTRISICS modes run at unoptimized and optimized settings. Each trap
is caught by a test-only reporter so overflowing inputs can be checked in bulk.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
MASK64 = (1 << 64) - 1

HARNESS = r'''
#include "recomp.h"
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

static jmp_buf trap;
static const char *trap_message;
static uint32_t trap_address;
_Noreturn void recomp_fail(const char *message, uint32_t address) {
    trap_message = message; trap_address = address;
    longjmp(trap, 1);
}

static void check(unsigned op, const uint64_t row[8], size_t index, size_t *traps) {
    uint64_t (*const helpers[])(uint64_t, uint64_t) = {mips_add32, mips_add64, mips_sub32};
    const char *messages[] = {"signed ADD overflow", "signed DADD overflow", "signed SUB overflow"};
    const uint64_t sentinel = UINT64_C(0xa5a5a5a5a5a5a5a5);
    volatile uint64_t destination = sentinel;
    trap_message = NULL; trap_address = UINT32_MAX;
    bool trapped = false;
    if (setjmp(trap)) trapped = true;
    else destination = helpers[op](row[0], row[1]);
    if ((unsigned)(trapped != 0) != row[2 + op * 2]) {
        fprintf(stderr, "%s case %zu (%016" PRIx64 ", %016" PRIx64 "): trap=%d, expected=%" PRIu64 "\n",
            messages[op], index, row[0], row[1], trapped != 0, row[2 + op * 2]);
        exit(1);
    }
    if (trapped) {
        if (strcmp(trap_message, messages[op]) || trap_address != 0 || destination != sentinel) {
            fprintf(stderr, "overflow changed the failure boundary or wrote a result\n");
            exit(1);
        }
        (*traps)++;
    } else if (destination != row[3 + op * 2]) {
        fprintf(stderr, "%s case %zu (%016" PRIx64 ", %016" PRIx64 "): "
            "got %016" PRIx64 ", expected %016" PRIx64 "\n",
            messages[op], index, row[0], row[1], destination, row[3 + op * 2]);
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *input = fopen(argv[1], "rb");
    if (!input) return 2;
    uint64_t row[8];
    size_t count = 0, traps = 0, bytes;
    while ((bytes = fread(row, 1, sizeof(row), input)) != 0) {
        if (bytes != sizeof(row)) return 2;
        for (unsigned op = 0; op < 3; op++) check(op, row, count, &traps);
        count++;
    }
    if (ferror(input) || fclose(input) || !count || !traps) return 2;
    printf("PASS: %zu add/subtract cases (%zu overflow traps, %zu results)\n", count * 3, traps, count * 3 - traps);
    return 0;
}
'''


def signed(value: int, width: int) -> int:
    value &= (1 << width) - 1
    return value - (1 << width) if value & (1 << (width - 1)) else value


def expected(x: int, y: int) -> list[int]:
    result = []
    for width, subtract in ((32, False), (64, False), (32, True)):
        a, b = signed(x, width), signed(y, width)
        value = a - b if subtract else a + b
        overflow = not -(1 << (width - 1)) <= value < (1 << (width - 1))
        result.extend((int(overflow), 0 if overflow else value & MASK64))
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cl' if os.name == 'nt' else 'clang')
    parser.add_argument('--build', type=Path, default=PROJECT / 'build', help='parent for temporary fixtures')
    args = parser.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    msvc = Path(args.cc).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
    edges = [0, 1, 2, MASK64, MASK64 - 1, 0x7fffffff, 0x80000000, 0xffffffff,
             0x100000000, 0xffffffff00000000, 0xffffffff80000000,
             0x7fffffffffffffff, 0x8000000000000000, 0x8000000000000001,
             0xdeadbeef00000001, 0xdeadbeef80000000]
    pairs = [(x, y) for x in edges for y in edges]
    rng = random.Random(0x414444535542)
    pairs.extend((rng.getrandbits(64), rng.getrandbits(64)) for _ in range(10000))
    # Straddle each signed limit with both signs, including zero and carry/borrow.
    for width in (32, 64):
        for limit in (-(1 << (width - 1)), (1 << (width - 1)) - 1):
            for offset in range(4):
                a = limit + offset if limit < 0 else limit - offset
                pairs.extend((a & MASK64, b & MASK64) for b in range(-4, 5))
    with tempfile.TemporaryDirectory(prefix='overflow-check-', dir=build) as directory:
        output = Path(directory)
        source, vectors = output / 'fixture.c', output / 'vectors.bin'
        source.write_text(HARNESS)
        vectors.write_bytes(b''.join(struct.pack('<8Q', x, y, *expected(x, y)) for x, y in pairs))
        for intrinsics in (0, 1):
            for optimization in (('/Od', '/O2') if msvc else ('-O0', '-O2')):
                binary = output / ('fixture.exe' if os.name == 'nt' else 'fixture')
                if msvc:
                    flags = ['/nologo', '/std:c11', '/W4', '/WX', optimization,
                             '/D_CRT_SECURE_NO_WARNINGS', f'/DUSE_INTRISICS={intrinsics}',
                             f'/I{PROJECT / "include"}', f'/Fe{binary}']
                else:
                    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', optimization,
                             f'-DUSE_INTRISICS={intrinsics}', f'-I{PROJECT / "include"}', '-o', str(binary)]
                    if os.name == 'nt':
                        flags.append('-D_CRT_SECURE_NO_WARNINGS')
                print(f'{args.cc}: USE_INTRISICS={intrinsics} {optimization}', flush=True)
                subprocess.run([args.cc, *flags, str(source)], check=True, cwd=output)
                subprocess.run([str(binary), str(vectors)], check=True, cwd=output)


if __name__ == '__main__':
    main()
