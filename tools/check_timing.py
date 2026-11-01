#!/usr/bin/env python3
"""Check optional timing CSV failures and shutdown without a ROM.

Run from Developer PowerShell with --cc cl, or use --cc clang/gcc on Linux.
Real files exercise exclusive creation; a one-shot flush fault covers header,
in-session and shutdown write failures independently of filesystem behavior.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#define _POSIX_C_SOURCE 200809L
#include "timing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail_flush;
static int checked_flush(FILE *file) {
    if (fail_flush) { fail_flush = false; return EOF; }
    return fflush(file);
}
#define fflush checked_flush
#include "@TIMING_SOURCE@"
#undef fflush

_Noreturn void recomp_fail(const char *message, uint32_t address) {
    fprintf(stderr, "Unexpected fatal error: %s (%08x)\n", message, address);
    exit(2);
}
static void check(bool condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static void disabled_calls(Runtime *rt) {
    check(!rt->timing, "profiler remains enabled after CSV failure");
    uint64_t start = recomp_timing_begin(rt);
    recomp_timing_add(rt, RECOMP_TIMING_DISPATCH, 3, 1);
    recomp_timing_end(rt, RECOMP_TIMING_SLEEP, start);
    recomp_timing_poll(rt);
    recomp_timing_destroy(rt);
    recomp_timing_destroy(rt);
    check(!rt->timing, "disabled profiler became active");
}
static void setting(const char *path) {
    check(!setenv("RECOMP_TIMING", path, 1), "set timing environment");
}
static void force_report(Runtime *rt) {
    // Ensure a nonempty reporting window, without a sleep or clock assumption.
    rt->timing->previous_wall--;
    recomp_timing_add(rt, RECOMP_TIMING_DISPATCH, 5, 2);
}
int main(void) {
    Runtime rt = {0};
    setting("");
    check(recomp_timing_init(&rt), "disabled init failed");
    disabled_calls(&rt);

    setting("existing.csv");
    check(recomp_timing_init(&rt), "existing report blocked startup");
    disabled_calls(&rt);
    setting("missing-parent/timing.csv");
    check(recomp_timing_init(&rt), "missing parent blocked startup");
    disabled_calls(&rt);
    setting("directory.csv");
    check(recomp_timing_init(&rt), "directory target blocked startup");
    disabled_calls(&rt);

    setting("header-error.csv");
    fail_flush = true;
    check(recomp_timing_init(&rt), "header failure blocked startup");
    disabled_calls(&rt);
    // The header-error file must have been closed, including on Windows.
    check(!remove("header-error.csv"), "header failure leaked file handle");

    setting("healthy.csv");
    check(recomp_timing_init(&rt) && rt.timing, "valid CSV did not enable profiling");
    force_report(&rt);
    recomp_timing_report(&rt, true);
    check(rt.timing != NULL, "successful report disabled profiling");
    recomp_timing_destroy(&rt);
    disabled_calls(&rt);

    setting("write-error.csv");
    check(recomp_timing_init(&rt) && rt.timing, "write error setup");
    force_report(&rt);
    fail_flush = true;
    recomp_timing_report(&rt, true);
    disabled_calls(&rt);
    check(!remove("write-error.csv"), "report failure leaked file handle");

    setting("shutdown-error.csv");
    check(recomp_timing_init(&rt) && rt.timing, "shutdown error setup");
    force_report(&rt);
    fail_flush = true;
    recomp_timing_destroy(&rt);
    disabled_calls(&rt);
    check(!remove("shutdown-error.csv"), "shutdown failure leaked file handle");
    puts("PASS: timing CSV errors are nonfatal, profiling disables, valid CSV records, shutdown is safe");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cl' if os.name == 'nt' else 'clang')
    parser.add_argument('--build', type=Path, default=PROJECT / 'build')
    args = parser.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    msvc = Path(args.cc).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
    with tempfile.TemporaryDirectory(prefix='timing-check-', dir=build) as directory:
        output = Path(directory)
        source = output / 'fixture.c'
        source.write_text(HARNESS.replace('@TIMING_SOURCE@', (PROJECT / 'runtime/timing.c').as_posix()))
        for index, optimization in enumerate(('/Od', '/O2') if msvc else ('-O0', '-O2')):
            run = output / str(index)
            run.mkdir()
            original = b'existing report must remain byte-for-byte unchanged\n'
            (run / 'existing.csv').write_bytes(original)
            (run / 'directory.csv').mkdir()
            binary = run / ('fixture.exe' if os.name == 'nt' else 'fixture')
            if msvc:
                flags = ['/nologo', '/std:c11', '/W4', '/WX', optimization,
                         '/D_CRT_SECURE_NO_WARNINGS', f'/I{PROJECT / "include"}', f'/Fe{binary}']
            else:
                flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', optimization,
                         f'-I{PROJECT / "include"}', '-o', str(binary)]
            sources = [str(source)]
            if os.name == 'nt':
                sources.append(str(PROJECT / 'runtime/host.c'))
            print(f'{args.cc}: {optimization}', flush=True)
            subprocess.run([args.cc, *flags, *sources], cwd=run, check=True)
            result = subprocess.run([str(binary)], cwd=run, capture_output=True, text=True, timeout=15)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            for warning in ('cannot create existing.csv', 'cannot create missing-parent/timing.csv',
                            'cannot create directory.csv', 'cannot write CSV header', 'report write failed'):
                assert warning in result.stderr, result.stderr
            assert result.stderr.count('profiling disabled') == 6, result.stderr
            assert (run / 'existing.csv').read_bytes() == original
            rows = (run / 'healthy.csv').read_text().splitlines()
            assert rows[0].startswith('elapsed_s,window_ms,logical_cpus,cpu_cores,dispatch_count,dispatch_ms')
            assert any(row.split(',')[4] == '2' for row in rows[1:]), rows
            assert (run / 'directory.csv').is_dir()
            print(result.stdout.strip())


if __name__ == '__main__':
    main()
