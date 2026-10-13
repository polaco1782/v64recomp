#!/usr/bin/env python3
"""Verify and normalize a ROM into the selected profile's local ROM path."""
import argparse
from pathlib import Path
import sys

from common import RecompError, load_profile, load_rom


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('--rom', required=True, type=Path)
    args = parser.parse_args()
    game = load_profile(args.profile)['game']
    rom, identity = load_rom(args.rom, game['rom_sha1'], game['rom_size'])
    project = Path(__file__).resolve().parents[1]
    destination = game['rom']
    if not destination.is_relative_to(project / 'local'):
        raise RecompError('import destination must be inside local/')
    if destination.exists():
        if destination.read_bytes() != rom:
            raise RecompError('destination already contains different bytes')
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(rom)
    print(f"Validated {identity['input_order']} ROM: {identity['sha1']}")
    print(f'Normalized ROM: {destination}')


if __name__ == '__main__':
    try:
        main()
    except (RecompError, OSError, ValueError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
