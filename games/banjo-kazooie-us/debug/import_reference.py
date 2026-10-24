#!/usr/bin/env python3
"""Explicitly import reviewed resident-to-level JAL roots from the matching ELF map.

This is an offline evidence operation, never part of normal generation. Supply a
checkout of BanjoRecompSyms; no ROM, assets or external checkout is distributed.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tomllib

PROJECT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(PROJECT / 'tools'))
from common import RecompError, load_profile, load_rom, write_json

# Ambiguous ELF addresses were reviewed against resident callers in spawnqueue.c,
# overlay lifecycle wrappers and code_CC1E0.c, not selected by pointer resemblance.
SECTIONS = {
    0x803863f0: '.fight', 0x80387340: '.MMM', 0x803885dc: '.BGS',
    0x80388760: '.CC', 0x80388f4c: '.CC', 0x80389cd8: '.MMM',
    0x8038a994: '.MMM', 0x8038a9c0: '.FP', 0x8038e2fc: '.GV',
    0x8039040c: '.GV',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    base = PROJECT / 'games/banjo-kazooie-us'
    profile = load_profile(base / 'profile.toml')
    rom, _ = load_rom(profile['game']['rom'], profile['game']['rom_sha1'], profile['game']['rom_size'])
    spec = importlib.util.spec_from_file_location('banjo_generate', base / 'generate.py')
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    image, prepared = generator.prepare(rom, profile, args.output.parent)
    decoded = {o['name']: generator.decode_overlay(rom, o) for o in profile['discovery']['overlays']}
    order = ('core1', 'core2', 'CC', 'GV', 'MMM', 'TTC', 'MM', 'BGS',
             'RBB', 'FP', 'SM', 'cutscenes', 'lair', 'fight', 'CCW', 'emptyLvl')
    canonical = rom[:0xf19250] + b''.join(decoded['.' + name] for name in order)
    if hashlib.sha1(canonical).hexdigest() != '1fb13cad402518d3ae9a8dc4b52c5c54b2a4adc7':
        raise RecompError('Reference decompilation image identity mismatch')
    source = args.reference.read_bytes()
    sections = tomllib.loads(source.decode())['section']
    core = next(s for s in sections if s['name'] == '.core2')
    regions = {r['name']: r for r in prepared['discovery']['regions']}
    inventory = json.loads((base / 'startup-entries.json').read_text())
    if inventory['rom_sha1'] != hashlib.sha1(image).hexdigest():
        raise RecompError('Checked inventory analysis identity mismatch')
    existing = {(e.get('section'), e['vram']): e for e in inventory['entries']}
    for caller in core['functions']:
        offset = regions['.core2']['rom_offset'] + caller['vram'] - core['vram']
        for delta in range(0, caller['size'], 4):
            word = int.from_bytes(image[offset + delta:offset + delta + 4], 'big')
            address = 0x80000000 | ((word & 0x03ffffff) << 2)
            if word >> 26 != 3 or not 0x803863f0 <= address < 0x803a0000:
                continue
            matches = [(s, f) for s in sections if s['name'] in decoded
                       for f in s.get('functions', []) if f['vram'] == address]
            if address in SECTIONS:
                matches = [(s, f) for s, f in matches if s['name'] == SECTIONS[address]]
            if len(matches) != 1:
                raise RecompError(f'Cross-overlay entry needs review: {address:#x}')
            section, function = matches[0]
            region = regions[section['name']]
            start = address - region['vram']
            body = decoded[section['name']][start:start + function['size']]
            if len(body) != function['size'] or len(body) < 8 or len(body) % 4:
                raise RecompError('Reference function body is outside decoded image')
            entry = existing.setdefault((section['name'], address), dict(
                section=section['name'], vram=address, origin='checked-root',
                sha256=hashlib.sha256(decoded[section['name']][start:start + 16]).hexdigest()))
            entry['reference_name'] = function['name']
            entry['body_size'] = len(body)
            entry['body_sha256'] = hashlib.sha256(body).hexdigest()
            site = dict(caller=caller['name'], pc=caller['vram'] + delta, expected_word=word)
            if site not in entry.setdefault('direct_call_evidence', []):
                entry['direct_call_evidence'].append(site)
    inventory['entries'] = sorted(existing.values(), key=lambda e: (e.get('section', ''), e['vram']))
    inventory['reference'] = dict(
        url='https://github.com/BanjoRecomp/BanjoRecompSyms',
        revision='6820055ca076e94e30e53d917bd9e5f71c28ca20',
        source_sha256=hashlib.sha256(source).hexdigest(),
        canonical_image_sha1=hashlib.sha1(canonical).hexdigest(),
        scope='Reviewed resident-to-level direct JALs only; not a complete function inventory')
    write_json(args.output, inventory)
    print(f"Wrote {len(inventory['entries'])} checked entries to {args.output}")


if __name__ == '__main__':
    main()
