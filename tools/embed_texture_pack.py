#!/usr/bin/env python3
"""Append a texture ZIP to a freshly linked executable without extracting it."""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import zipfile


def check_pack(path: Path) -> None:
    with zipfile.ZipFile(path) as archive:
        names = set()
        for entry in archive.infolist():
            if entry.is_dir():
                continue
            if entry.filename in names or entry.flag_bits & 1:
                raise ValueError('texture ZIP contains duplicate names or encrypted entries')
            if entry.compress_type not in (0, 8, 93):
                raise ValueError(f'unsupported texture ZIP compression: {entry.compress_type}')
            names.add(entry.filename)


def append_pack(binary: Path, pack: Path) -> None:
    check_pack(pack)
    if binary.resolve() == pack.resolve():
        raise ValueError('the executable and texture ZIP must be different files')
    with binary.open('r+b') as output, pack.open('rb') as source:
        output.seek(0, 2)
        shutil.copyfileobj(source, output)
    # zipfile recognizes the executable prefix when reading the appended ZIP.
    # Rewriting its central directory makes all offsets absolute, so RT64's
    # miniz reader can stream entries straight from the executable. The local
    # headers and compressed image bytes are copied unchanged, including ZIP64.
    # Python conservatively starts ZIP64 at 2 GiB. Miniz treats an offset-only
    # ZIP64 sentinel as a 32-bit offset in one bounds check, rejecting archives
    # between 2 and 4 GiB. Our 64-bit file operations support the full unsigned
    # ZIP range; emit ZIP64 only when the format actually requires it.
    original_limit = zipfile.ZIP64_LIMIT
    try:
        zipfile.ZIP64_LIMIT = 0xfffffffe  # 0xffffffff is the ZIP64 sentinel.
        with zipfile.ZipFile(binary, 'a') as archive:
            archive.comment = archive.comment
    finally:
        zipfile.ZIP64_LIMIT = original_limit


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack', type=Path, required=True)
    output = parser.add_mutually_exclusive_group()
    output.add_argument('--binary', type=Path, help='freshly linked executable')
    output.add_argument('--build-source', type=Path, help='write a small C source to track ZIP rebuilds')
    args = parser.parse_args()
    try:
        if args.binary:
            append_pack(args.binary, args.pack)
            print(f'Embedded texture ZIP: {args.pack} -> {args.binary}')
        else:
            check_pack(args.pack)
            if args.build_source:
                args.build_source.parent.mkdir(parents=True, exist_ok=True)
                # Refresh even when only the ZIP bytes changed. Compiling this
                # source forces a fresh link on all generators, including MSVC.
                args.build_source.write_text(
                    '/* Generated ZIP dependency; the archive is appended after linking. */\n'
                    'const unsigned char recomp_embedded_texture_pack_present = 1;\n', encoding='utf-8')
    except (OSError, ValueError, zipfile.BadZipFile, NotImplementedError) as error:
        parser.exit(1, f'Texture ZIP embedding failed: {error}\n')


if __name__ == '__main__':
    main()
