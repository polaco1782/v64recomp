#!/usr/bin/env python3
"""Check generated SET_OBJ_PHYSICS and shared arithmetic shifts without starting SM64.

Use a Developer PowerShell for --cc cl. The selected build must already contain
SM64 US generated sources. No ROM bytes or game assets enter the fixture.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[3]
SYMBOL = 'recomp_s1_8038586c'

HARNESS = r'''
#include "recomp.h"
#include <stdio.h>
#include <stdlib.h>
_Noreturn void recomp_fail(const char *message, uint32_t address) {
    fprintf(stderr, "%s at %08x\n", message, address); exit(1);
}
RecompFunction recomp_lookup(Runtime *rt, uint32_t address) {
    (void)rt; (void)address; abort();
}
uint32_t recomp_load32_shared(Runtime *rt, uint32_t address) {
    (void)rt; (void)address; abort();
}
void recomp_store(Runtime *rt, uint32_t address, uint64_t value, unsigned bytes) {
    (void)rt; (void)address; (void)value; (void)bytes; abort();
}
uint32_t recomp_ram_address_write_slow(Runtime *rt, uint32_t address, size_t bytes) {
    (void)rt; (void)address; (void)bytes; abort();
}
'''

CHECKS = r'''
/* Independent division-based oracle, including negative rounding toward -inf. */
static uint64_t shifted(uint64_t value, unsigned count) {
    if (!count) return value;
    uint64_t divisor = UINT64_C(1) << count;
    if (!(value >> 63)) return value / divisor;
    uint64_t magnitude = UINT64_C(0) - value;
    return UINT64_C(0) - magnitude / divisor - (magnitude % divisor != 0);
}
static void check_shifts(void) {
    uint64_t seed = UINT64_C(0xfedcba9876543210);
    const uint64_t edges[] = {0, 1, UINT64_MAX, UINT64_C(0x8000000000000000),
        UINT64_C(0x7fffffffffffffff), UINT64_C(0x80000000), UINT64_C(0x7fffffff),
        UINT64_C(0xffffffff80000000), UINT64_C(0xffffffffffce03e8)};
    for (unsigned n = 0; n < 1033; n++) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        uint64_t value = n < sizeof(edges) / sizeof(edges[0]) ? edges[n] : seed;
        for (unsigned count = 0; count < 128; count++) {
            uint64_t word = (uint32_t)value;
            if (word & UINT64_C(0x80000000)) word |= UINT64_C(0xffffffff00000000);
            if (mips_sra32(value, count) != shifted(word, count & 31) ||
                mips_sra64(value, count) != shifted(value, count & 63))
                recomp_fail("arithmetic shift mismatch", count);
        }
    }
}
static void check_physics(void) {
    Runtime rt = {0};
    rt.rdram_size = 0x400000;
    rt.rdram = calloc(1, rt.rdram_size);
    if (!rt.rdram) abort();
    const int drag_values[] = {1000, 0, 1, 32767, -1, -32768};
    const unsigned offsets[] = {0x128, 0xe4, 0x158, 0x12c, 0x170, 0x174};
    for (unsigned n = 0; n < sizeof(drag_values) / sizeof(drag_values[0]); n++) {
        MipsContext ctx = {0};
        ctx.gpr[29] = mips_s32(0x80200000);
        recomp_store32(&rt, 0x80361160, 0x8034c208);
        recomp_store32(&rt, 0x80361164, 0x80010000);
        recomp_store32(&rt, 0x80010004, 0x0028fe70); /* radius 40; gravity -400 */
        recomp_store32(&rt, 0x80010008, 0xffce0000 | (uint16_t)drag_values[n]);
        recomp_store32(&rt, 0x8001000c, 0x03e80000); /* friction 1000; buoyancy 0 */
        float expected[] = {40, -4, -0.5f, drag_values[n] / 100.0f, 10, 0};
        recomp_s1_8038586c(&rt, &ctx);
        for (unsigned field = 0; field < 6; field++) {
            uint32_t bits = recomp_load32(&rt, 0x8034c208 + offsets[field]);
            float actual; memcpy(&actual, &bits, sizeof(actual));
            if (actual != expected[field]) {
                fprintf(stderr, "drag input %d, field +%03x: got %.9g, expected %.9g\n",
                    drag_values[n], offsets[field], actual, expected[field]);
                exit(1);
            }
        }
    }
    free(rt.rdram);
}
int main(void) {
    check_shifts(); check_physics();
    puts("PASS: 264448 arithmetic shifts and 36 generated physics fields");
    return 0;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--cc', default='cl' if os.name == 'nt' else 'clang')
    args = parser.parse_args()
    build = args.build.resolve()
    generated = build / 'generated' / 'sm64-us'
    metadata = json.loads((generated / 'symbols.json').read_text())
    if metadata['rom_sha1'] != '9bef1128717f958171a4afac3ed78ee2bb4e86ce':
        raise ValueError('the fixture requires the checked SM64 US revision')
    pattern = rf'^void {SYMBOL}\(.*?^\}}'
    functions = [match[0] for path in generated.glob('cpu-*.c')
                 if (match := re.search(pattern, path.read_text(), re.M | re.S))]
    if len(functions) != 1:
        raise ValueError(f'expected one generated {SYMBOL}, got {len(functions)}')
    msvc = Path(args.cc).name.lower() in ('cl', 'cl.exe')
    with tempfile.TemporaryDirectory(prefix='physics-check-', dir=build) as directory:
        output = Path(directory)
        source = output / 'fixture.c'
        source.write_text(HARNESS + functions[0] + CHECKS)
        for intrinsics in (0, 1):
            for optimization in (('/Od', '/O2') if msvc else ('-O0', '-O2')):
                binary = output / ('fixture.exe' if os.name == 'nt' else 'fixture')
                if msvc:
                    flags = ['/nologo', '/std:c11', '/w', '/fp:strict', optimization,
                             f'/DUSE_INTRISICS={intrinsics}', f'/I{PROJECT / "include"}',
                             f'/Fe{binary}']
                    libraries = []
                else:
                    flags = ['-std=c11', optimization, '-frounding-math', '-ffp-contract=off',
                             f'-DUSE_INTRISICS={intrinsics}', f'-I{PROJECT / "include"}', '-o', str(binary)]
                    libraries = ['-lm']
                print(f'USE_INTRISICS={intrinsics} {optimization}', flush=True)
                subprocess.run([args.cc, *flags, str(source), str(PROJECT / 'runtime/fpu.c'),
                                *libraries], check=True, cwd=output)
                subprocess.run([str(binary)], check=True, cwd=output)


if __name__ == '__main__':
    main()
