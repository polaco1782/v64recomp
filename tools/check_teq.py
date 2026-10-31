#!/usr/bin/env python3
"""Compile and execute synthetic TEQ cases (Developer PowerShell for --cc cl)."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from discover import discover
from emit import analyze, decode, emit_function, FunctionRegistry

PROJECT = Path(__file__).resolve().parents[1]
BASE = 0x80000400


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cl' if os.name == 'nt' else 'clang')
    args = parser.parse_args()
    functions = []
    for mode in ('plain', 'delay', 'annulled'):
        # Code 1023 exercises all ten trap-code bits; compare a0 with a1.
        teq = (4 << 21) | (5 << 16) | (1023 << 6) | 0x34
        # BEQL zero,a2,skip: a2=1 annuls the trap in its delay slot.
        words = [teq, 0x24020007, 0x03e00008, 0] if mode == 'plain' else [
            0x50060002 if mode == 'annulled' else 0x10000002, teq, 0, 0x24020007, 0x03e00008, 0]
        raw = struct.pack('>'+str(len(words))+'I', *words)
        rom = bytearray(64)+raw
        rom[8:12] = BASE.to_bytes(4, 'big')
        profile = dict(game=dict(name='TEQ fixture', entry_symbol='entry', code_sections=['.text']),
                       discovery=dict(regions=[dict(name='.text', vram=BASE, rom_offset=64, size=len(raw))]),
                       native_hooks=dict(enabled=False))
        metadata = discover(bytes(rom), profile)
        function = metadata['functions'][0]
        instructions = decode(raw, BASE)
        registry = FunctionRegistry(metadata['functions'])
        assert not analyze(function, instructions, registry), mode
        functions.append(emit_function(function, instructions, registry).replace('recomp_s0_80000400', 'case_'+mode))
    harness = r'''
#include "recomp.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
static jmp_buf trap;
static uint32_t trap_pc;
static const char *trap_message;
_Noreturn void recomp_fail(const char *message, uint32_t pc) {
    trap_message = message; trap_pc = pc; longjmp(trap, 1);
}
'''+ '\n'.join(functions) + r'''
static Runtime runtime;
static MipsContext ctx;
int main(void) {
    const uint64_t pairs[][2] = {{0,0},{1,1},{UINT64_MAX,UINT64_MAX},{1,0},
        {UINT64_C(0x100000001),1},{UINT64_C(0xffffffff80000000),UINT64_C(0x80000000)}};
    void (*cases[])(Runtime *, MipsContext *) = {case_plain,case_delay,case_annulled};
    unsigned count=0;
    for (unsigned mode=0; mode<3; mode++) for (unsigned i=0; i<sizeof(pairs)/sizeof(pairs[0]); i++) {
        memset(&ctx,0,sizeof(ctx));
        ctx.gpr[2]=99; ctx.gpr[4]=pairs[i][0]; ctx.gpr[5]=pairs[i][1]; ctx.gpr[6]=1;
        bool expected = mode != 2 && pairs[i][0] == pairs[i][1];
        bool trapped = setjmp(trap) != 0;
        if (!trapped) cases[mode](&runtime,&ctx);
        if (trapped != expected || ctx.gpr[4]!=pairs[i][0] || ctx.gpr[5]!=pairs[i][1] ||
            (trapped && (strcmp(trap_message,"MIPS TEQ 1023") || trap_pc!=0x80000400+(mode?4:0) || ctx.gpr[2]!=99)) ||
            (!trapped && ctx.gpr[2]!=7)) {
            fprintf(stderr,"TEQ failure: mode=%u case=%u\n",mode,i); return 1;
        }
        count++;
    }
    printf("PASS: %u TEQ comparisons, branch delays and likely-slot annulment\n",count);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='teq-', dir=PROJECT/'build') as tmp:
        output = Path(tmp)
        (output/'fixture.c').write_text(harness)
        msvc = Path(args.cc).name.lower() in ('cl','cl.exe','clang-cl','clang-cl.exe')
        binary = output/('fixture.exe' if os.name == 'nt' else 'fixture')
        for debug in (False, True):
            flags = (['/nologo','/std:c11','/W4','/O2','/wd4102',f'/I{PROJECT / "include"}',f'/Fe{binary}'] if msvc
                     else ['-std=c11','-Wall','-Wextra','-Wno-unused-label','-O2',f'-I{PROJECT / "include"}','-o',str(binary)])
            if debug: flags += ['/DRECOMP_DEBUG'] if msvc else ['-DRECOMP_DEBUG']
            subprocess.run([args.cc,*flags,str(output/'fixture.c')],cwd=output,check=True)
            subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    main()
