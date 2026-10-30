#!/usr/bin/env python3
"""Exercise checked direct calls to runtime-registered code using synthetic bytes.

Windows: run from Developer PowerShell, passing an existing MSVC game build.
Linux: pass an existing CMake game build and optionally --cc clang/gcc.
"""
from copy import deepcopy
import argparse
import os
import re
from pathlib import Path
import struct
import subprocess
import tempfile

from common import RecompError
from discover import discover
from emit import FunctionRegistry, analyze, decode_function, emit_function

PROJECT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--cc', default='cl' if os.name == 'nt' else 'clang')
    args = parser.parse_args()
    build = args.build.resolve()
    debug = bool(re.search(r'^CMAKE_BUILD_TYPE:[^=]+=Debug$', (build / 'CMakeCache.txt').read_text(), re.M))
    # Preserve the outer return; untaken branches must never resolve the target.
    words = [0x03e08021, 0x10800003, 0x24060009, 0x0c000400, 0,
             0x0200f821, 0x03e00008, 0]
    rom = bytearray(0x40) + struct.pack('>8I', *words)
    struct.pack_into('>I', rom, 8, 0x80000400)
    site = dict(pc=0x8000040c, expected_word=words[3], target=0x80001000)
    profile = dict(game=dict(name='registered-call fixture', entry_symbol='entry', code_sections=['.text']),
                   discovery=dict(regions=[dict(name='.text', vram=0x80000400, rom_offset=0x40, size=32)]),
                   native_hooks=dict(enabled=False))
    original = discover(bytes(rom), profile)
    assert any('unmapped call target' in error['reason']
               for error in original['functions'][0]['discovery_errors'])
    profile['discovery']['registered_direct_calls'] = [site]
    metadata = discover(bytes(rom), profile)
    function = metadata['functions'][0]
    assert not function['discovery_errors']
    instructions = decode_function(bytes(rom), function)
    assert not analyze(function, instructions, FunctionRegistry(metadata['functions']))
    for field in ('expected_word', 'target'):
        bad = deepcopy(profile)
        bad['discovery']['registered_direct_calls'][0][field] ^= 4
        try:
            discover(bytes(rom), bad)
        except RecompError:
            pass
        else:
            raise AssertionError('mismatched direct-call evidence accepted')
    emitted = emit_function(function, instructions, FunctionRegistry(metadata['functions']))
    source = '#include "runtime.h"\n#include <assert.h>\n#include <stdlib.h>\n#include <string.h>\n' + emitted
    source += r'''
static void callback(Runtime *rt, MipsContext *ctx) { (void)rt; ctx->gpr[2] = 42; }
static void failure(const RecompFailureInfo *info, void *unused) {
    (void)unused;
    _Exit(!strcmp(info->message, "unregistered or unloaded function") && info->address == 0x80001000 ? 77 : 78);
}
int main(int argc, char **argv) {
    (void)argv;
    RecompFunctionEntry entries[] = {
        {.address=0x80000400,.size=32,.section=0,.function=recomp_s0_80000400,.name="caller"},
        {.address=0x80001000,.size=4,.section=1,.function=callback,.name="callback"}};
    bool loaded[] = {true, false}; Runtime rt;
    assert(recomp_runtime_init(&rt,0x400000,entries,2,loaded,2));
    recomp_set_failure_handler(failure,NULL);
    MipsContext ctx={0};ctx.gpr[2]=7;ctx.gpr[31]=mips_s32(0xf0000000);
    recomp_s0_80000400(&rt,&ctx);
    assert(ctx.gpr[2]==7 && ctx.gpr[6]==9 && ctx.gpr[31]==mips_s32(0xf0000000));
    ctx.gpr[4]=1;
    if(argc==1)rt.section_loaded[1]=true;
    recomp_s0_80000400(&rt,&ctx);
    assert(ctx.gpr[2]==42 && ctx.gpr[31]==mips_s32(0xf0000000));
    recomp_runtime_destroy(&rt);
}
'''
    with tempfile.TemporaryDirectory(prefix='v64-registered-calls-') as temporary:
        output = Path(temporary)
        (output / 'generated.h').write_text('#include "recomp.h"\n')
        (output / 'check.c').write_text(source)
        environment = os.environ.copy()
        for optimize in (False, True):
            executable = output / ('check.exe' if os.name == 'nt' else 'check')
            if os.name == 'nt':
                dependencies = PROJECT / 'build/vcpkg_installed/x64-windows'
                config = 'debug/' if debug else ''
                environment['PATH'] = str(dependencies / config / 'bin') + os.pathsep + environment.get('PATH', '')
                library = build / ('Debug' if debug else 'Release') / 'recomp_runtime.lib'
                command = [args.cc, '/nologo', '/std:c11', '/O2' if optimize else '/Od',
                           '/MDd' if debug else '/MD', '/I'+str(PROJECT/'include'), '/I'+str(output)]
                if debug:
                    command.append('/DRECOMP_DEBUG')
                command += [str(output/'check.c'), str(library), '/Fe:'+str(executable), '/link',
                            '/LIBPATH:'+str(dependencies/config/'lib'), 'libcrypto.lib',
                            'pthreadVC3d.lib' if debug else 'pthreadVC3.lib', 'dbghelp.lib']
            else:
                command = [args.cc, '-std=c11', '-O2' if optimize else '-O0',
                           '-I'+str(PROJECT/'include'), '-I'+str(output)]
                if debug:
                    command.append('-DRECOMP_DEBUG')
                command += [str(output/'check.c'), str(build/'librecomp_runtime.a'), '-lpthread',
                            '-lcrypto', '-ldl', '-lm', '-o', str(executable)]
            compilation = subprocess.run(command, cwd=output, env=environment, capture_output=True)
            if compilation.returncode:
                raise RuntimeError(compilation.stdout.decode(errors='replace') + compilation.stderr.decode(errors='replace'))
            subprocess.run([str(executable)], check=True, env=environment, capture_output=True)
            missing = subprocess.run([str(executable), 'unloaded'], env=environment, capture_output=True)
            assert missing.returncode == 77, missing.stderr.decode(errors='replace')
    print('Conditional registered calls, delay slots, runtime section selection and evidence rejection passed at O0/O2')


if __name__ == '__main__':
    main()
