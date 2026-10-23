#!/usr/bin/env python3
"""Show verified MIPS and available compiled x86-64 code for a generated function."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

from common import RecompError, load_metadata, load_profile
from emit import cname, decode_function
from export_analysis_rom import load_analysis_rom
from generate_game import GAMES, PROJECT


def locate_binary(metadata_path: Path) -> Path | None:
    """Use only the executable belonging to this metadata's build directory."""
    path = metadata_path.resolve()
    if path.parent.parent.name not in ('generated', 'reports'):
        return None
    binary = path.parent.parent.parent / 'bin' / path.parent.name
    return binary if binary.is_file() else None


def disassemble_host(name: str, metadata_path: Path, binary_path: Path | None) -> None:
    binary = binary_path.resolve() if binary_path else locate_binary(metadata_path)
    print('\nRecompiled x86-64 (Intel syntax)')
    if binary is None:
        print('Unavailable: no executable for this metadata; build the game or pass --binary PATH.')
        return
    if not binary.is_file():
        raise RecompError(f'compiled binary does not exist: {binary}')
    print(f'Binary: {binary}')
    objdump = shutil.which('objdump')
    if objdump is None:
        print('Unavailable: install GNU binutils (objdump) to disassemble the compiled function.')
        return
    result = subprocess.run([objdump, '-d', '-Mintel', f'--disassemble={name}', str(binary)],
                            capture_output=True, text=True)
    if result.returncode:
        print(f'Unavailable: objdump failed: {result.stderr.strip() or result.stdout.strip()}')
        return
    if not re.search(r'file format \S*x86-64\b', result.stdout):
        print('Unavailable: the selected binary is not x86-64.')
        return
    # objdump exits successfully even when the requested symbol is absent.
    if not re.search(rf'^[0-9a-fA-F]+ <{re.escape(name)}>:', result.stdout, re.MULTILINE):
        print(f'Unavailable: {name} has no disassembly in this binary (missing or stripped symbol).')
        return
    if binary.stat().st_mtime_ns < metadata_path.stat().st_mtime_ns:
        print('Note: the binary predates the metadata; rebuild to ensure it reflects current sources.')
    print(result.stdout.strip())


def find_function(metadata: dict, section: int, address: int) -> dict | None:
    matches = [f for f in metadata['functions']
               if f['section'] == section and f['vram'] == address]
    if len(matches) > 1:
        raise RecompError('metadata has duplicate function entries for this section/address')
    return matches[0] if matches else None


def locate_metadata(section: int, address: int, path: Path | None,
                    game: str | None) -> tuple[Path, dict]:
    game_name = load_profile(PROJECT/'games'/game/'profile.toml')['game']['name'] if game else None
    paths = [path] if path else sorted((PROJECT/'build').rglob('symbols.json'))
    matches = []
    for candidate in paths:
        metadata = json.loads(candidate.read_text())
        if metadata.get('format_version') != 1:
            if path:
                raise RecompError('unsupported function metadata version')
            continue
        if game_name and metadata['game'] != game_name:
            continue
        function = find_function(metadata, section, address)
        if function:
            matches.append((candidate, metadata, function))
    if not matches:
        raise RecompError('function not found in generated symbols.json; generate sources first '
                          'or select the matching --game / --metadata PATH')
    # Equivalent Debug/Release/report copies can share a symbol. Different ROMs
    # or instruction ownership need an explicit choice rather than a guess.
    identities = {(m['game'], m['rom_sha1'], f['rom_offset'], f['sha256'],
                   tuple(f.get('instruction_addresses', range(f['vram'], f['vram']+f['size'], 4))))
                  for _, m, f in matches}
    if len(identities) != 1:
        candidates = '\n'.join(f"  {p}: {m['game']}" for p, m, _ in matches)
        raise RecompError('function matches different games or builds; select --game or --metadata:\n'
                          + candidates)
    # Prefer an equivalent build with an executable over a newer report-only copy.
    selected = max(matches, key=lambda item: (locate_binary(item[0]) is not None,
                                              item[0].stat().st_mtime_ns))
    return selected[0], selected[1]


def disassemble_function(name: str, game: str | None = None,
                         metadata_path: Path | None = None, rom_path: Path | None = None,
                         binary_path: Path | None = None, arch: str = 'both') -> None:
    match = re.fullmatch(r'recomp_s([0-9]+)_([0-9a-fA-F]{8})', name)
    if not match:
        raise RecompError('expected a generated function name such as recomp_s3_7f068008')
    section, address = int(match[1]), int(match[2], 16)
    if address % 4:
        raise RecompError('function entry address must be word-aligned')
    path, metadata = locate_metadata(section, address, metadata_path, game)
    if arch == 'x86':
        function = find_function(metadata, section, address)
        print(f"{metadata['game']}: {cname(function)} ({function['name']})")
        print(f'Metadata: {path.resolve()}')
        disassemble_host(cname(function), path, binary_path)
        return
    if rom_path:
        rom = rom_path.read_bytes()
        metadata = load_metadata(path, rom)
    else:
        if game is None:
            games = [candidate for candidate in GAMES
                     if load_profile(PROJECT/'games'/candidate/'profile.toml')['game']['name'] == metadata['game']]
            if len(games) != 1:
                raise RecompError('cannot identify the game; supply --game or an exact analysis image with --rom')
            game = games[0]
        rom, metadata = load_analysis_rom(game, path)
    function = find_function(metadata, section, address)
    if function is None:
        raise RecompError('function disappeared from metadata while loading; retry with a stable build')
    mapped = next(s for s in metadata['sections'] if s['id'] == section)
    instructions = decode_function(rom, function)
    print(f"{metadata['game']}: {cname(function)} ({function['name']})")
    print(f'Metadata: {path.resolve()}')
    print(f"Section {section} {mapped['name']}; entry {address:#010x}; "
          f"analysis-image offset {function['rom_offset']:#x}; span {function['size']:#x} bytes")
    print(f'Verified {len(instructions)} owned instructions; analysis SHA-1 {metadata["rom_sha1"]}')
    print('\nPC        ROM offset  Word      MIPS')
    previous = address - 4
    for instruction in instructions:
        if instruction.pc != previous + 4:
            print(f'; {instruction.pc-previous-4:#x} bytes not owned by this function omitted')
        offset = function['rom_offset'] + instruction.pc - address
        print(f'{instruction.pc:08x}  {offset:08x}    {instruction.word:08x}  {instruction.assembly}')
        previous = instruction.pc
    if arch == 'both':
        disassemble_host(cname(function), path, binary_path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('function', help='generated C function name: recomp_s<decimal section>_<hex address>')
    parser.add_argument('--game', choices=GAMES, help='restrict lookup to this game; otherwise inferred')
    parser.add_argument('--metadata', type=Path, help='exact symbols.json; otherwise search build/ automatically')
    parser.add_argument('--rom', type=Path, help='exact normalized/prepared analysis image; otherwise prepare the pinned ROM')
    parser.add_argument('--binary', type=Path, help='compiled executable; otherwise use bin/<game> from the selected build')
    parser.add_argument('--arch', choices=('mips', 'x86', 'both'), default='both',
                        help='instruction output to display (default: both); x86 selects x86-64')
    args = parser.parse_args()
    disassemble_function(args.function, args.game, args.metadata, args.rom, args.binary, args.arch)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RecompError, OSError, ValueError, KeyError, TypeError, ImportError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
