#!/usr/bin/env python3
"""Execute emitted VR4300 branch-and-link cases against a built runtime library.

Example: .venv/bin/python tools/check_branch_link.py --build build/re2-us-v1-debug
The fixtures are synthetic; no ROM is needed. See VR4300 manual chapter 16:
https://n64dev.org/p/U10504EJ7V0UMJ1.pdf (BLTZAL/BGEZAL and likely forms).
"""
from __future__ import annotations
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
from discover import discover
from emit import analyze, decode, emit_function, FunctionRegistry

PROJECT = Path(__file__).resolve().parents[1]
BASE, CALLEE = 0x80000400, 0x80001000


def words(op: int, rs: int) -> list[int]:
    return [0x03e08025, 0x04000000 | rs << 21 | op << 16 | ((CALLEE - (BASE + 8)) // 4),
            0x03e02825, 0x03e03025, 0x0200f825, 0x03e00008, 0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path, help='existing Debug or Release CMake build')
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    build = args.build.resolve()
    cache = (build / 'CMakeCache.txt').read_text()
    debug = 'CMAKE_BUILD_TYPE:STRING=Debug' in cache
    # A never-taken link has no executable edge to its encoded target.
    raw = struct.pack('>7I', *words(16, 0))
    rom = bytearray(64) + raw
    rom[8:12] = BASE.to_bytes(4, 'big')
    profile = dict(game=dict(name='branch-link fixture', entry_symbol='entry', code_sections=['.text']),
                   discovery=dict(regions=[dict(name='.text', vram=BASE, rom_offset=64, size=len(raw))]),
                   native_hooks=dict(enabled=False))
    metadata = discover(bytes(rom), profile)
    assert [f['vram'] for f in metadata['functions']] == [BASE]
    assert not metadata['functions'][0]['discovery_errors']

    functions, cases = [], []
    opcodes = [('bltzal', 16), ('bgezal', 17), ('bltzall', 18), ('bgezall', 19)]
    for opcode, encoding in opcodes:
        for rs in (0, 4, 31):
            instructions = decode(struct.pack('>7I', *words(encoding, rs)), BASE)
            assert instructions[1].opcode == opcode
            assert instructions[1].gpr_write_mask & (1 << 31)
            function = dict(name='fixture', section=0, vram=BASE, size=28)
            callee = dict(name='callee', section=0, vram=CALLEE, size=4)
            registry = FunctionRegistry([function, callee])
            assert not analyze(function, instructions, registry)
            emitted = emit_function(function, instructions, registry, hooks={(0, CALLEE): 'test_callee'})
            name = f'case_{opcode}_{rs}'
            functions.append(emitted.replace('recomp_s0_80000400', name))
            for value in (-1, 0, 1):
                source = -1 if rs == 31 else 0 if rs == 0 else value
                taken = source < 0 if opcode.startswith('bltz') else source >= 0
                slot = taken or not opcode.endswith('all')
                cases.append(f'check({name}, {value}, {int(taken)}, {int(slot)});')

    with tempfile.TemporaryDirectory(prefix='v64-branch-link-') as temporary:
        output = Path(temporary)
        (output / 'generated.h').write_text('#include "recomp.h"\n'
            'extern const RecompFunctionEntry recomp_functions[];\n'
            'void test_callee(Runtime *, MipsContext *);\n')
        source = '''#include "generated.h"
#include "runtime.h"
#include <assert.h>
#include <stdio.h>
void test_callee(Runtime *rt, MipsContext *ctx) {
    (void)rt; ctx->gpr[3]++; ctx->gpr[2]=0xfeed;
}
const RecompFunctionEntry recomp_functions[] = {
    {.address=0x80000400,.size=28,.section=0,.function=test_callee,.name="caller"},
    {.address=0x80001000,.size=4,.section=0,.function=test_callee,.name="callee"}
};
'''
        source += '\n'.join(functions)
        source += '''
static void check(RecompFunction function, int value, bool taken, bool slot) {
    Runtime rt; bool loaded=true;
    assert(recomp_runtime_init(&rt,0x400000,recomp_functions,2,&loaded,1));
    MipsContext ctx={0};ctx.gpr[4]=mips_s32(value);ctx.gpr[5]=0x1234;
    ctx.gpr[31]=mips_s32(0x80010000);
    function(&rt,&ctx);
    assert(ctx.gpr[3]==(unsigned)taken);
    assert(ctx.gpr[2]==(taken?0xfeed:0));
    assert(ctx.gpr[5]==(slot?mips_s32(0x8000040c):0x1234));
    assert(ctx.gpr[6]==mips_s32(0x8000040c));
    assert(ctx.gpr[31]==mips_s32(0x80010000));
    recomp_runtime_destroy(&rt);
}
int main(void) {
'''
        source += '\n'.join(cases)
        source += '\nputs("36 branch-link execution cases and never-taken discovery passed");\n}\n'
        (output / 'fixture.c').write_text(source)
        for optimization in ('-O0', '-O2'):
            binary = output / 'fixture'
            subprocess.run([args.cc, '-std=c11', optimization, '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-label', *(['-DRECOMP_DEBUG'] if debug else []),
                '-I' + str(PROJECT / 'include'), '-I' + str(output), str(output / 'fixture.c'),
                str(build / 'librecomp_runtime.a'), '-lcrypto', '-lpthread', '-ldl', '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
