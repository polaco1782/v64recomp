#!/usr/bin/env python3
"""Report SDK-body candidates that differ from catalog bodies at relocation sites.

This is an investigation aid, never native-hook recognition. Review every changed
operand, callee and global before adding a complete-body fingerprint to the catalog.
Reference ROMs remain private; only catalog bodies with matching SHA-256 are used.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

from common import load_metadata, load_rom
from native_hooks import catalog


def relocation_mask(word: int) -> int:
    opcode, base = word >> 26, (word >> 21) & 31
    if opcode in (2, 3):  # Direct jump/call destinations.
        return 0xfc000000
    if opcode == 15:  # LUI address halves; constants still need manual review.
        return 0xffff0000
    if opcode in (9, 13, 32, 33, 35, 36, 37, 39, 40, 41, 43, 49, 53, 57, 61) and base not in (0, 29):
        return 0xffff0000
    return 0xffffffff


def compare(target: bytes, metadata: dict, references: list[bytes]) -> list[dict]:
    candidates = []
    for signature in catalog():
        prefix = b''.join(w.to_bytes(4, 'big') for w in signature['expected_words'])
        size = signature['body_size']
        body = None
        for reference in references:
            offset = reference.find(prefix)
            while offset >= 0:
                raw = reference[offset:offset + size]
                if hashlib.sha256(raw).hexdigest() == signature['sha256']:
                    body = raw
                    break
                offset = reference.find(prefix, offset + 4)
            if body is not None:
                break
        if body is None:
            continue
        original = struct.unpack(f'>{size // 4}I', body)
        for function in metadata['functions']:
            offset = function['rom_offset']
            section = metadata['sections'][function['section']]
            if offset + size > section['rom_offset'] + section['size']:
                continue
            raw = target[offset:offset + size]
            words = struct.unpack(f'>{size // 4}I', raw)
            if words == original or any((a & relocation_mask(a)) != (b & relocation_mask(a)) for a, b in zip(original, words)):
                continue
            candidates.append(dict(symbol=signature['symbol'], handler=signature['handler'],
                vram=function['vram'], section=function['section'], body_size=size,
                sha256=hashlib.sha256(raw).hexdigest(), reference_sha256=signature['sha256'],
                expected_words=list(words[:8]),
                changes=[dict(pc=function['vram'] + i * 4, reference=a, candidate=b)
                         for i, (a, b) in enumerate(zip(original, words)) if a != b]))
    return sorted(candidates, key=lambda c: (c['vram'], c['symbol'], c['body_size']))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', required=True, type=Path, help='exact normalized/prepared target image')
    parser.add_argument('--metadata', required=True, type=Path)
    parser.add_argument('--reference-rom', required=True, action='append', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    target, _ = load_rom(args.rom)
    metadata = load_metadata(args.metadata, target)
    references = [load_rom(path)[0] for path in args.reference_rom]
    candidates = compare(target, metadata, references)
    args.output.write_text(json.dumps(dict(warning='Candidates require operand, ABI and full-body review.',
        rom_sha1=metadata['rom_sha1'], candidates=candidates), indent=2) + '\n')
    for candidate in candidates:
        print(f"0x{candidate['vram']:08x} {candidate['symbol']} size=0x{candidate['body_size']:x} changed={len(candidate['changes'])}")


if __name__ == '__main__':
    main()
