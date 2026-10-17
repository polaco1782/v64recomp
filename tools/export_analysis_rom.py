#!/usr/bin/env python3
"""Export the exact discovery image, including prepared CPU code, for address inspection."""
from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import sys
import tempfile

from common import RecompError, load_metadata, load_profile, load_rom
from generate_game import GAMES, PROJECT


def export_analysis_rom(game: str, metadata: Path, output: Path) -> None:
    base = PROJECT / 'games' / game
    profile = load_profile(base / ('full.toml' if game == 'sm64-us' else 'startup.toml'))
    identity = profile['game']
    rom, _ = load_rom(identity['rom'], identity['rom_sha1'], identity['rom_size'])
    sys.path.insert(0, str(base / 'release'))
    spec = importlib.util.spec_from_file_location('game_generate', base / 'release/generate.py')
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    # Some prepare hooks also emit headers. Keep those out of the real build.
    with tempfile.TemporaryDirectory(prefix='v64-analysis-') as tmp:
        if hasattr(generator, 'prepare'):
            rom, _ = generator.prepare(rom, profile, Path(tmp))
    load_metadata(metadata, rom)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        if output.read_bytes() != rom:
            raise RecompError('output already contains different bytes; choose a new path')
    else:
        with output.open('xb') as stream:
            stream.write(rom)
    print(f'Verified analysis image: {output} ({len(rom):#x} bytes)')
    print('Offsets match symbols.json. Runtime must continue to use the original ROM.')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, choices=GAMES)
    parser.add_argument('--metadata', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    export_analysis_rom(args.game, args.metadata, args.output)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RecompError, OSError, ValueError, KeyError, ImportError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
