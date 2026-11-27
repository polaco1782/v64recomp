#!/usr/bin/env python3
"""Export the exact discovery image, including prepared CPU code, for address inspection."""
from __future__ import annotations

import argparse
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
import sys
import tempfile

from common import RecompError, load_metadata, load_profile, load_rom
from generate_game import GAMES, PROJECT, load_generator


@contextmanager
def prepared_analysis_rom(game: str) -> Iterator[tuple[bytes, dict]]:
    """Return the discovery image and resolved profile without changing build outputs."""
    base = PROJECT / 'games' / game
    profile = load_profile(base / 'profile.toml')
    identity = profile['game']
    rom, _ = load_rom(identity['rom'], identity['rom_sha1'], identity['rom_size'])
    generator = load_generator(game)
    if hasattr(generator, 'prepare_analysis'):
        yield generator.prepare_analysis(rom, profile)
    elif hasattr(generator, 'prepare'):
        # Some prepare hooks also emit files. Keep those out of the real build.
        with tempfile.TemporaryDirectory(prefix='v64-analysis-') as tmp:
            yield generator.prepare(rom, profile, Path(tmp))
    else:
        yield rom, profile


def load_analysis_rom(game: str, metadata: Path) -> tuple[bytes, dict]:
    """Prepare and verify the discovery image without changing build outputs."""
    with prepared_analysis_rom(game) as (rom, _):
        return rom, load_metadata(metadata, rom)


def export_analysis_rom(game: str, metadata: Path, output: Path) -> None:
    rom, _ = load_analysis_rom(game, metadata)
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
