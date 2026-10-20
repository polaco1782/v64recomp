#!/usr/bin/env python3
"""Merge pinned n64sym identification signatures and reorganize the shared symbol database.

Imported CRCs and library references never authorize native replacements. Native
handlers retain their independently measured, complete-body SHA-256 fingerprints.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import tomllib
from urllib.request import Request, urlopen

from common import RecompError
from symbol_database import DATABASE, encode_database, load_database, name, reorganize

PROJECT = Path(__file__).resolve().parents[1]
REVISION = 'ccf4600f3389f1a84bde23339225cf372fdf7712'
SOURCE_SHA256 = '725d901a7c796c5ea7a56550aa5374e79a2f0c1bea331155bb0ec4e3aeeabf0d'
LICENSE_SHA256 = '5a060e234d7d92edf81387957dc8baf1e67e6790db9954206e02560bc9676834'
LICENSE = PROJECT / 'licenses/n64sym.txt'


def read_native_catalog(path: Path) -> list[dict]:
    """One-time migration of the former TOML catalog, retaining its ROM notes."""
    raw = path.read_text()
    records = tomllib.loads(raw)['signatures']
    notes, pending = [], []
    for line in raw.splitlines():
        if line.strip() == '[[signatures]]':
            notes.append(pending)
            pending = []
        elif line.lstrip().startswith('#'):
            pending.append(line.lstrip()[1:].strip())
        elif line.strip():
            pending = []
    if len(notes) != len(records):
        raise RecompError('cannot associate native catalog notes with signatures')
    return [dict(record, **({'notes': note} if note else {}))
            for record, note in zip(records, notes)]


def merge(database: dict, natives: list[dict] = (), sdk: list | None = None, source: dict | None = None) -> dict:
    database = copy.deepcopy(database)
    if sdk is None:
        sdk = []
    if not isinstance(sdk, list):
        raise RecompError('n64sym input must be an array')
    source_id = None
    if source is not None:
        source_id = 'n64sym-' + source['revision']
        previous = database['sources'].get(source_id)
        if previous is not None and previous != source:
            raise RecompError('conflicting provenance for the same n64sym revision')
        database['sources'][source_id] = source
    for raw in natives:
        record = dict(raw)
        symbol = name(record.pop('symbol', None))
        database['symbols'].setdefault(symbol, {}).setdefault('native_signatures', []).append(record)
    for row in sdk:
        if source_id is None or not isinstance(row, list) or len(row) != 5:
            raise RecompError('invalid n64sym symbol record')
        symbol, size, prefix, body, references = row
        name(symbol)
        record = dict(body_size=size, crc32_prefix=prefix, crc32_body=body,
                      references=references, sources=[source_id])
        database['symbols'].setdefault(symbol, {}).setdefault('sdk_signatures', []).append(record)
    return reorganize(database)


def fetch(url: str) -> bytes:
    with urlopen(Request(url, headers={'User-Agent': 'v64recomp-symbol-import'}), timeout=30) as response:
        return response.read(32 * 1024 * 1024 + 1)


def checked_bytes(raw: bytes, digest: str, label: str) -> bytes:
    if not re.fullmatch(r'[0-9a-f]{64}', digest) or hashlib.sha256(raw).hexdigest() != digest:
        raise RecompError(f'{label} SHA-256 mismatch')
    if len(raw) > 32 * 1024 * 1024:
        raise RecompError(f'{label} exceeds the import size limit')
    return raw


def atomic_write(path: Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                         prefix=path.name + '.', suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--database', type=Path, default=DATABASE)
    input_group = parser.add_mutually_exclusive_group()
    input_group.add_argument('--n64sym', type=Path, help='downloaded web/signatures.json; hash checked')
    input_group.add_argument('--download', action='store_true', help='fetch the pinned n64sym data and MIT license')
    parser.add_argument('--revision', default=REVISION, help='full upstream commit ID')
    parser.add_argument('--sha256', default=SOURCE_SHA256, help='expected signature-file SHA-256')
    parser.add_argument('--license', type=Path, default=LICENSE, help='license path, also written by --download')
    parser.add_argument('--license-sha256', default=LICENSE_SHA256)
    parser.add_argument('--native-catalog', type=Path, help='migrate a former native_hooks.toml catalog')
    parser.add_argument('--check', action='store_true', help='validate canonical output without writing any files')
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9a-f]{40}', args.revision):
        raise RecompError('--revision must be a complete lowercase Git commit ID')
    database = load_database(args.database) if args.database.exists() else dict(
        format_version=1, sources={}, symbols={})
    natives = read_native_catalog(args.native_catalog) if args.native_catalog else []
    rows, source, license_text = [], None, None
    if args.download or args.n64sym:
        base = f'https://raw.githubusercontent.com/shygoo/n64sym/{args.revision}'
        raw = fetch(base + '/web/signatures.json') if args.download else args.n64sym.read_bytes()
        checked_bytes(raw, args.sha256, 'n64sym signatures')
        license_raw = fetch(base + '/LICENSE.md') if args.download else args.license.read_bytes()
        checked_bytes(license_raw, args.license_sha256, 'n64sym license')
        license_text = license_raw.decode('utf-8')
        rows = json.loads(raw)
        source = dict(url=base + '/web/signatures.json', revision=args.revision,
                      sha256=args.sha256, license='MIT', license_sha256=args.license_sha256)
    merged = merge(database, natives, rows, source)
    encoded = encode_database(merged)
    previous = args.database.read_text() if args.database.exists() else None
    if args.check:
        if previous != encoded:
            raise RecompError('symbol database differs from the canonical merged output')
    else:
        if license_text is not None and (not args.license.exists() or args.license.read_text() != license_text):
            atomic_write(args.license, license_text)
        if previous != encoded:
            atomic_write(args.database, encoded)
    entries = merged['symbols'].values()
    native_count = sum(len(e.get('native_signatures', [])) for e in entries)
    sdk_count = sum(len(e.get('sdk_signatures', [])) for e in entries)
    print(f'{args.database}: {len(merged["symbols"])} symbols, {native_count} native signatures, '
          f'{sdk_count} SDK identification signatures; ' +
          ('checked' if args.check else 'unchanged' if previous == encoded else 'updated'))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RecompError, OSError, ValueError, KeyError, TypeError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
