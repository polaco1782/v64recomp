#!/usr/bin/env python3
"""Run a game's CMake workflow in a separate tree for each configuration and target."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
EMSCRIPTEN_TOOLCHAIN = Path('cmake/Modules/Platform/Emscripten.cmake')


def cached_toolchain(build_dir):
    cache = build_dir / 'CMakeCache.txt'
    if cache.is_file():
        for line in cache.read_text(errors='replace').splitlines():
            key, _, value = line.partition('=')
            if key.startswith('CMAKE_TOOLCHAIN_FILE:') and value:
                return build_dir / value
    return None


def wasm_toolchain(game, mode, env):
    build = ROOT / 'build'
    selected = build / f'{game}-wasm-{mode.lower()}'
    cached = cached_toolchain(selected)
    if (selected / 'CMakeCache.txt').is_file() and (not cached or cached.name != EMSCRIPTEN_TOOLCHAIN.name):
        raise ValueError(f'{selected} has a CMake cache without an Emscripten toolchain. '
                         'Move that build directory aside to configure a fresh Wasm build.')
    if cached is not None:
        if cached.is_file():
            return cached
        raise ValueError(f'{selected} references a missing toolchain at {cached}. '
                         'Restore that SDK or move the build directory aside to configure a fresh one.')

    candidates = []
    if env.get('CMAKE_TOOLCHAIN_FILE'):
        candidates.append(ROOT / env['CMAKE_TOOLCHAIN_FILE'])
    if env.get('EMSDK'):
        candidates.append(Path(env['EMSDK']) / 'upstream/emscripten' / EMSCRIPTEN_TOOLCHAIN)
    other_mode = 'release' if mode == 'Debug' else 'debug'
    candidates.append(cached_toolchain(build / f'{game}-wasm-{other_mode}'))
    compiler = shutil.which('emcc', path=env.get('PATH'))
    if compiler:
        candidates.append(Path(compiler).resolve().parent / EMSCRIPTEN_TOOLCHAIN)
    for sdk in (build / 'emsdk', ROOT / 'local/emsdk', ROOT / 'emsdk'):
        candidates.append(sdk / 'upstream/emscripten' / EMSCRIPTEN_TOOLCHAIN)
    for path in candidates:
        if path and path.name == EMSCRIPTEN_TOOLCHAIN.name and path.is_file():
            return path

    # Packaged compiler launchers can live outside the Emscripten installation.
    config = shutil.which('em-config', path=env.get('PATH'))
    if config:
        result = subprocess.run([config, 'EMSCRIPTEN_ROOT'], capture_output=True, text=True, env=env)
        if result.returncode == 0 and result.stdout.strip():
            path = Path(result.stdout.strip()) / EMSCRIPTEN_TOOLCHAIN
            if path.is_file():
                return path
    raise ValueError('no Emscripten toolchain found for --wasm. Install the Emscripten SDK '
                     'and source its emsdk_env.sh (emsdk_env.bat on Windows), '
                     'set EMSDK to its directory, or put the installed emcc on PATH.')


def main():
    presets = json.loads((ROOT / 'CMakePresets.json').read_text())
    parser = argparse.ArgumentParser(description=__doc__, epilog='Default: RT64 Release. Browser builds require Emscripten.')
    parser.add_argument('game', choices=[p['name'] for p in presets['workflowPresets']])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--debug', dest='mode', action='store_const', const='Debug', help='build with diagnostics')
    mode.add_argument('--release', dest='mode', action='store_const', const='Release', help='build the optimized configuration')
    target = parser.add_mutually_exclusive_group()
    for name, help_text in [('rt64', 'desktop RT64'), ('gles2', 'desktop GLES2'), ('wasm', 'browser WebAssembly with GLES2')]:
        target.add_argument('--' + name, dest='target', action='store_const', const=name, help=help_text)
    parser.set_defaults(mode='Release', target='rt64')
    args = parser.parse_args()
    variant = args.mode.lower() if args.target == 'rt64' else f'{args.target}-{args.mode.lower()}'
    env = dict(os.environ, V64_BUILD_SUFFIX='-' + variant, CMAKE_BUILD_TYPE=args.mode,
               V64_WORKFLOW_RENDERER='rt64' if args.target == 'rt64' else 'gles2')
    if args.target == 'wasm':
        if args.game != 'sm64-us':
            parser.error('the browser ROM loader currently supports sm64-us only')
        try:
            toolchain = wasm_toolchain(args.game, args.mode, env)
        except ValueError as error:
            parser.error(str(error))
        env['CMAKE_TOOLCHAIN_FILE'] = str(toolchain.resolve())
    print(f'{args.game} {args.mode} ({args.target}) -> build/{args.game}-{variant}', flush=True)
    return subprocess.run(['cmake', '--workflow', '--preset', args.game], cwd=ROOT, env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
