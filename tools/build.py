#!/usr/bin/env python3
"""Run a game's CMake workflow in a separate tree for each configuration and target."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    presets = json.loads((ROOT / 'CMakePresets.json').read_text())
    parser = argparse.ArgumentParser(description=__doc__, epilog='Default: RT64 Release. Browser builds require an activated Emscripten SDK.')
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
        sdk = env.get('EMSDK')
        if not sdk:
            parser.error('--wasm requires an activated Emscripten SDK (EMSDK)')
        toolchain = Path(sdk) / 'upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake'
        if not toolchain.is_file():
            parser.error(f'Emscripten toolchain missing from the active SDK: {toolchain}')
        env['CMAKE_TOOLCHAIN_FILE'] = str(toolchain.resolve())
    print(f'{args.game} {args.mode} ({args.target}) -> build/{args.game}-{variant}', flush=True)
    return subprocess.run(['cmake', '--workflow', '--preset', args.game], cwd=ROOT, env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
