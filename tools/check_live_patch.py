#!/usr/bin/env python3
"""Exercise a paused missing call, checked call, local jump, and clock freeze."""
from pathlib import Path
import subprocess
import tempfile
from emit import FunctionRegistry, decode, emit_function


ROOT = Path(__file__).resolve().parents[1]
MODULE = r'''
#include "recomp_live.h"
static void missing(Runtime *rt, MipsContext *ctx) { (void)rt; ctx->gpr[2] += 5; }
static void resume(Runtime *rt, MipsContext *ctx, uint32_t pc, uint32_t ra, uint32_t sp) {
    (void)rt;
    ctx->gpr[2] += pc == 0x80002000 && ra == 0x80005555 && sp == 0x80006666 ? 7 : 1000;
}
static const RecompFunctionEntry entries[] = {{0x80001000, 4, 0, missing, "fixture"}};
static const RecompLiveTransfer calls[] = {{0x80003000, 0x80001000, 0}};
static const RecompLiveTransfer jumps[] = {{0x80004000, 0x80002000, 0}};
const RecompLiveManifest recomp_live_manifest = {
    .abi = RECOMP_LIVE_ABI, .debug_build = 0,
    .runtime_size = sizeof(Runtime), .context_size = sizeof(MipsContext),
    .entry_size = sizeof(RecompFunctionEntry),
    .functions = entries, .function_count = 1,
    .calls = calls, .call_count = 1, .jumps = jumps, .jump_count = 1, .resume = resume
};
'''
DRIVER = r'''
#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    Runtime rt;
    bool loaded[] = {true};
    if (!recomp_runtime_init(&rt, 4096, NULL, 0, loaded, 1)) return 3;
    rt.rom = (const uint8_t *)"test";
    if (!recomp_clock_init(&rt, &(RecompClockConfig){.ticks_per_second = 1000000000})) return 4;
    if (unsetenv("RECOMP_LIVE_PATCH_DIR")) return 5;
    rt.live_patch_dir = argv[1];
    if (setenv("RECOMP_TEST_MODULE", argv[2], 1) || setenv("RECOMP_TEST_DIR", argv[1], 1)) return 6;
    rt.live_patch_python = "/bin/sh";
    rt.live_patch_script = argv[3];
    rt.live_patch_game = "fixture";
    rt.live_patch_build_dir = argv[1];
    uint64_t before = recomp_count64(&rt);
    struct timespec wall_before, wall_after;
    clock_gettime(CLOCK_MONOTONIC, &wall_before);
    RecompFunction function = recomp_lookup(&rt, 0x80001000);
    clock_gettime(CLOCK_MONOTONIC, &wall_after);
    uint64_t after = recomp_count64(&rt);
    long wall_ms = (wall_after.tv_sec - wall_before.tv_sec) * 1000 +
                   (wall_after.tv_nsec - wall_before.tv_nsec) / 1000000;
    if (!function || wall_ms < 250 || after - before > 150000000) return 9;
    MipsContext ctx = {0};
    function(&rt, &ctx);
    recomp_live_checked_call(&rt, &ctx, 0x80001000, 0x80003000, 0);
    recomp_live_checked_jump(&rt, &ctx, 0x80002000, 0x80004000, 0, 0x80005555, 0x80006666);
    if (ctx.gpr[2] != 17 || !recomp_live_boundary(&rt, &ctx, 0x80001000) || ctx.gpr[2] != 22) return 10;
    if (setenv("RECOMP_LIVE_PATCH_DIR", "off", 1) || recomp_live_lookup(&rt, 0x80001004)) return 12;
    char request[4096];
    snprintf(request, sizeof(request), "%s/request-80001000.json", argv[1]);
    if (!access(request, F_OK)) return 11;
    recomp_runtime_destroy(&rt);
    return 0;
}
'''


def check_generated_resume(base: Path, library: Path) -> None:
    words = [0x01000008, 0x24420003, 0x24420029, 0x03e00008, 0]
    function = {'section': 0, 'vram': 0x80001000, 'name': 'resume_fixture',
                'size': len(words) * 4, 'rom_offset': 0, 'aliases': [],
                'indirect_transfers': [{'pc': 0x80001000, 'expected_word': words[0],
                                        'kind': 'jump', 'targets': [0x80001008]}]}
    instructions = decode(b''.join(word.to_bytes(4, 'big') for word in words), function['vram'])
    code = emit_function(function, instructions, FunctionRegistry([function]),
                         direct_targets={}, resume_entry=True)
    source = ('#include "recomp.h"\n' + code + '\n'
              'int main(void) {\n'
              '    Runtime rt = {0}; MipsContext ctx = {0};\n'
              '    ctx.gpr[2] = 1; ctx.gpr[31] = 0x80002000;\n'
              '    recomp_s0_80001000_live_resume(&rt, &ctx, 0x80001008, 0x80002000, 0);\n'
              '    return ctx.gpr[2] == 42 ? 0 : 1;\n}\n')
    (base / 'resume.c').write_text(source)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-label',
                    '-I', str(ROOT / 'include'), str(base / 'resume.c'), str(library),
                    '-lcrypto', '-lpthread', '-ldl', '-lm', '-o', str(base / 'resume')], check=True)
    subprocess.run([str(base / 'resume')], check=True, timeout=10)


def main() -> None:
    library = ROOT / 'build/host/librecomp_runtime.a'
    if not library.is_file():
        raise SystemExit('build host runtime first: cmake --build build/host')
    with tempfile.TemporaryDirectory(prefix='v64-live-check-') as directory:
        base = Path(directory)
        (base / 'module.c').write_text(MODULE)
        (base / 'driver.c').write_text(DRIVER)
        script = base / 'helper.sh'
        script.write_text('#!/bin/sh\n'
                          'sleep 0.3\n'
                          'test -f "$RECOMP_TEST_DIR/request-80001000.json" || exit 7\n'
                          'printf "%s\\n" "$RECOMP_TEST_MODULE" > "$RECOMP_TEST_DIR/pending.path"\n'
                          'mv "$RECOMP_TEST_DIR/pending.path" "$RECOMP_TEST_DIR/function-80001000.path"\n')
        subprocess.run(['cc', '-std=c11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'include'), str(base / 'module.c'), '-o', str(base / 'module.so')],
                       check=True)
        subprocess.run(['cc', '-std=c11', '-rdynamic', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'include'), str(base / 'driver.c'), str(library),
                        '-lcrypto', '-lpthread', '-ldl', '-lm', '-o', str(base / 'driver')], check=True)
        subprocess.run([str(base / 'driver'), str(base), str(base / 'module.so'), str(script)], check=True, timeout=10)
        check_generated_resume(base, library)
    print('live patch pause, helper invocation, dispatch, generated resume, and clock freeze: OK')


if __name__ == '__main__':
    main()
