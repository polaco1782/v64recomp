#!/usr/bin/env python3
"""Check MULT/MULTU/DMULT/DMULTU against Python integer products, without a ROM.

Run from a Developer PowerShell for --cc cl; --cc clang also supports Linux.
Both USE_INTRISICS modes run at unoptimized and optimized settings. Temporary
fixtures and binaries are removed after the check.
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
#include <stdio.h>
#include <stdlib.h>

static void mult_s(MipsContext *ctx, uint64_t x, uint64_t y) { mips_mult(ctx, x, y, true); }
static void mult_u(MipsContext *ctx, uint64_t x, uint64_t y) { mips_mult(ctx, x, y, false); }
static void dmult_s(MipsContext *ctx, uint64_t x, uint64_t y) { mips_dmult(ctx, x, y, true); }
static void dmult_u(MipsContext *ctx, uint64_t x, uint64_t y) { mips_dmult(ctx, x, y, false); }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *input = fopen(argv[1], "rb");
    if (!input) return 2;
    void (*const helpers[])(MipsContext *, uint64_t, uint64_t) = {mult_s, mult_u, dmult_s, dmult_u};
    const char *names[] = {"MULT", "MULTU", "DMULT", "DMULTU"};
    uint64_t row[10];
    size_t count = 0, bytes;
    while ((bytes = fread(row, 1, sizeof(row), input)) != 0) {
        if (bytes != sizeof(row)) return 2;
        for (unsigned op = 0; op < 4; op++) {
            MipsContext ctx, saved;
            memset(&saved, 0xa5, sizeof(saved));
            memcpy(&ctx, &saved, sizeof(ctx));
            helpers[op](&ctx, row[0], row[1]);
            if (ctx.hi != row[2 + op * 2] || ctx.lo != row[3 + op * 2]) {
                fprintf(stderr, "%s case %zu (%016" PRIx64 ", %016" PRIx64 "): "
                    "got %016" PRIx64 ":%016" PRIx64 ", expected %016" PRIx64 ":%016" PRIx64 "\n",
                    names[op], count, row[0], row[1], ctx.hi, ctx.lo, row[2 + op * 2], row[3 + op * 2]);
                return 1;
            }
            ctx.hi = saved.hi; ctx.lo = saved.lo;
            if (memcmp(&ctx, &saved, sizeof(ctx))) {
                fprintf(stderr, "%s modified registers other than HI/LO\n", names[op]);
                return 1;
            }
        }
        count++;
    }
    if (ferror(input) || fclose(input) || !count) return 2;
    printf("PASS: %zu signed/unsigned multiply results and context checks\n", count * 4);
    return 0;
}
'''


def signed(value: int, width: int) -> int:
    value &= (1 << width) - 1
    return value - (1 << width) if value & (1 << (width - 1)) else value


def expected(x: int, y: int) -> list[int]:
    result = []
    for width, is_signed in ((32, True), (32, False), (64, True), (64, False)):
        mask = (1 << width) - 1
        a, b = (signed(x, width), signed(y, width)) if is_signed else (x & mask, y & mask)
        product = (a * b) & ((1 << (width * 2)) - 1)
        high, low = product >> width, product & mask
        if width == 32:
            high, low = signed(high, 32) & MASK64, signed(low, 32) & MASK64
        result.extend((high, low))
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
             0xdeadbeef00000001, 0xdeadbeef80000000, 0x5555555555555555, 0xaaaaaaaaaaaaaaaa]
    pairs = [(x, y) for x in edges for y in edges]
    rng = random.Random(0x4d495053)
    pairs.extend((rng.getrandbits(64), rng.getrandbits(64)) for _ in range(10000))
    # Exercise carry propagation at every 32-bit limb boundary.
    pairs.extend(((1 << bit) - 1, MASK64 - (1 << bit) + 1) for bit in range(1, 64))
    with tempfile.TemporaryDirectory(prefix='multiply-check-', dir=build) as directory:
        output = Path(directory)
        source, vectors = output / 'fixture.c', output / 'vectors.bin'
        source.write_text(HARNESS)
        vectors.write_bytes(b''.join(struct.pack('<10Q', x, y, *expected(x, y)) for x, y in pairs))
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
