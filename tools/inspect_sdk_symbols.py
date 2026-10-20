#!/usr/bin/env python3
"""Identify SDK routines and report native-handler gaps in a verified analysis image.

Diagnostic only: never changes discovery names, roots, hooks, or generated code.
CRC matches identify candidates; complete measured SHA-256 bodies authorize hooks.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import sys
import zlib

from common import RecompError, load_metadata, load_profile, validate_hook_prefix
from export_analysis_rom import load_analysis_rom
from generate_game import GAMES, PROJECT
from symbol_database import DATABASE, load_database

MASKS = {'hi16': 0xffff0000, 'lo16': 0xffff0000, 'targ26': 0xfc000000}


def relocation_masks(signature: dict) -> tuple:
    masks = {}
    for kind, _, offsets in signature['references']:
        for offset in offsets:
            masks[offset] = masks.get(offset, 0xffffffff) & MASKS[kind]
    return tuple(sorted(masks.items()))


def masked_bytes(raw: bytes, masks: tuple) -> bytes:
    result = bytearray(raw)
    for offset, mask in masks:
        if offset + 4 <= len(result):
            word = int.from_bytes(result[offset:offset + 4], 'big') & mask
            result[offset:offset + 4] = word.to_bytes(4, 'big')
    return bytes(result)


def signature_index(database: dict, selected: set[str]) -> tuple[dict, dict]:
    prefixes, natives = {}, defaultdict(list)
    for symbol, entry in database['symbols'].items():
        if selected and symbol not in selected:
            continue
        for signature in entry.get('sdk_signatures', []):
            masks = relocation_masks(signature)
            prefix_masks = tuple((offset, mask) for offset, mask in masks if offset < 8)
            key = (min(signature['body_size'], 8), prefix_masks)
            prefixes.setdefault(key, defaultdict(list))[signature['crc32_prefix']].append(
                (symbol, signature, masks))
        for signature in entry.get('native_signatures', []):
            prefix = b''.join(word.to_bytes(4, 'big') for word in signature['expected_words'])
            natives[prefix].append((symbol, signature))
    return prefixes, natives


def hardware_accesses(raw: bytes, address: int) -> list[dict]:
    """Bounded evidence from adjacent LUI[/ORI or ADDIU]/memory instructions.

    This deliberately does not propagate constants through branches or calls.
    Absence of this evidence does not establish that a routine avoids hardware.
    """
    words = [int.from_bytes(raw[i:i + 4], 'big') for i in range(0, len(raw), 4)]
    result = []
    loads = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x30, 0x31, 0x34, 0x35, 0x37}
    stores = {0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x38, 0x39, 0x3c, 0x3d, 0x3f}
    for i, word in enumerate(words[:-1]):
        if word >> 26 != 15:
            continue
        register, value = (word >> 16) & 31, (word & 0xffff) << 16
        if not register:
            continue
        j = i + 1
        following = words[j]
        if (following >> 26 in (9, 13) and (following >> 21) & 31 == register
                and (following >> 16) & 31 == register):
            immediate = following & 0xffff
            value = ((value | immediate) if following >> 26 == 13 else
                     value + (immediate if immediate < 0x8000 else immediate - 0x10000)) & 0xffffffff
            j += 1
        if j >= len(words):
            continue
        memory = words[j]
        opcode = memory >> 26
        if opcode not in loads | stores or (memory >> 21) & 31 != register:
            continue
        immediate = memory & 0xffff
        effective = (value + (immediate if immediate < 0x8000 else immediate - 0x10000)) & 0xffffffff
        bus = effective & 0x1fffffff if 0x80000000 <= effective < 0xc0000000 else effective
        if 0x03f00000 <= bus < 0x04900000 or 0x1fc00000 <= bus < 0x1fc00800:
            result.append(dict(pc=address + j * 4, address=effective, physical_address=bus,
                               access='write' if opcode in stores else 'read'))
    return result


def handler_definitions(game: str | None) -> dict[str, list[str]]:
    """Source definitions, not declarations or a claim that the build links them."""
    paths = list((PROJECT / 'runtime').glob('*.[cC]'))
    paths += list((PROJECT / 'runtime').glob('*.cpp'))
    if game:
        paths += [p for p in (PROJECT / 'games' / game).rglob('*') if p.suffix in ('.c', '.cpp')]
    definitions = defaultdict(list)
    pattern = re.compile(r'^\s*(?:static\s+)?void\s+([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{', re.M)
    for path in sorted(paths):
        for match in pattern.finditer(path.read_text()):
            definitions[match[1]].append(str(path.relative_to(PROJECT)))
    return dict(definitions)


def native_coverage(entry: dict, exact: list[dict], function: dict | None,
                    profile: dict | None, definitions: dict) -> dict:
    custom = next((h for h in (profile or {}).get('hooks', []) if function and h.get('vram') == function['vram']), None)
    selected = None
    if function and function.get('origin') == 'native-replacement':
        if custom and custom.get('kind') == 'replace_function':
            selected = custom['handler']
        elif profile is not None and profile.get('native_hooks', {}).get('enabled', True) and exact:
            selected = exact[0]['handler']
    available = sorted({r['handler'] for r in entry.get('native_signatures', [])})
    if profile is None and function and function.get('origin') == 'native-replacement':
        status = 'binding_unknown'
    elif selected:
        status = 'native_replacement' if selected in definitions else 'handler_definition_missing'
    elif available:
        status = 'variant_unbound' if any(h in definitions for h in available) else 'handler_definition_missing'
    else:
        status = 'no_registered_handler'
    return dict(status=status, selected_handler=selected, registered_handlers=available,
                definitions={h: definitions.get(h, []) for h in sorted(set(available + ([selected] if selected else [])))},
                metadata_origin=function.get('origin') if function else None)


def inspect(rom: bytes, metadata: dict, database: dict, profile: dict | None, definitions: dict,
            scope: str = 'entries', symbols: set[str] | None = None, section: str | None = None) -> dict:
    if scope not in ('entries', 'sections'):
        raise RecompError('unknown SDK scan scope')
    selected = symbols or set()
    if selected - database['symbols'].keys():
        raise RecompError('unknown database symbols: ' + ', '.join(sorted(selected - database['symbols'].keys())))
    sections = [s for s in metadata['sections'] if section is None or section in (s['name'], str(s['id']))]
    if section is not None and len(sections) != 1:
        raise RecompError('unknown or ambiguous section selector')
    prefixes, natives = signature_index(database, selected)
    functions = {(f['section'], f['vram']): f for f in metadata['functions']}
    owners = {(f['section'], pc) for f in metadata['functions'] for pc in f['instruction_addresses']}
    records = {}
    scanned = 0

    def record(region, address, offset, symbol, size):
        key = (region['id'], address, symbol)
        if key not in records:
            function = functions.get((region['id'], address))
            records[key] = dict(section=region['id'], section_name=region['name'], vram=address,
                rom_offset=offset, symbol=symbol, existing_name=function['name'] if function else None,
                ownership='entry' if function else 'owned_interior' if (region['id'], address) in owners else 'unowned',
                native_matches=[], sdk_matches=[], callers=[], callees=[], hardware_accesses=[], body_sizes=[])
        item = records[key]
        if size not in item['body_sizes']:
            item['body_sizes'].append(size)
        return item

    for region in sections:
        start, end = region['rom_offset'], region['rom_offset'] + region['size']
        if start < 0 or end > len(rom) or region['vram'] % 4:
            raise RecompError('SDK scan section lies outside its verified analysis image')
        if scope == 'entries':
            addresses = sorted(address for sid, address in functions if sid == region['id'])
        else:
            addresses = range(region['vram'], region['vram'] + region['size'] - 3, 4)
        for address in addresses:
            offset = start + address - region['vram']
            scanned += 1
            body_cache = {}
            for (length, masks), by_crc in prefixes.items():
                if offset + length > end:
                    continue
                crc = zlib.crc32(masked_bytes(rom[offset:offset + length], masks))
                for symbol, signature, full_masks in by_crc.get(crc, []):
                    size = signature['body_size']
                    if offset + size > end:
                        continue
                    key = (size, full_masks)
                    if key not in body_cache:
                        body_cache[key] = zlib.crc32(masked_bytes(rom[offset:offset + size], full_masks))
                    if body_cache[key] != signature['crc32_body']:
                        continue
                    # TARG26 references must actually be direct MIPS jumps/calls.
                    if any(int.from_bytes(rom[offset + o:offset + o + 4], 'big') >> 26 not in (2, 3)
                           for kind, _, offsets in signature['references'] if kind == 'targ26' for o in offsets):
                        continue
                    record(region, address, offset, symbol, size)['sdk_matches'].append(signature)
        # Full native prefixes are inexpensive to find independently of the CRC index.
        for prefix, variants in natives.items():
            offset = rom.find(prefix, start, end)
            while offset != -1:
                address = region['vram'] + offset - start
                if (offset - start) % 4 == 0 and (scope == 'sections' or (region['id'], address) in functions):
                    for symbol, signature in variants:
                        size = signature['body_size']
                        if offset + size <= end and hashlib.sha256(rom[offset:offset + size]).hexdigest() == signature['sha256']:
                            record(region, address, offset, symbol, size)['native_matches'].append(signature)
                offset = rom.find(prefix, offset + 4, end)

    by_address = defaultdict(list)
    for item in records.values():
        by_address[item['vram']].append(item)
    for item in records.values():
        entry = database['symbols'][item['symbol']]
        function = functions.get((item['section'], item['vram']))
        exact = [s for other in by_address[item['vram']] if other['section'] == item['section']
                 for s in other['native_matches']]
        if function and function.get('origin') == 'native-replacement' and profile is not None:
            custom = next((h for h in profile.get('hooks', []) if h.get('vram') == item['vram']), None)
            if custom and custom.get('kind') == 'replace_function':
                region = next(s for s in sections if s['id'] == item['section'])
                validate_hook_prefix(rom, item['rom_offset'], item['vram'],
                                     region['rom_offset'] + region['size'] - item['rom_offset'], custom)
        item['native'] = native_coverage(entry, exact, function, profile, definitions)
        item['body_sizes'].sort()
        item['body_sha256'] = {str(size): hashlib.sha256(rom[item['rom_offset']:item['rom_offset'] + size]).hexdigest()
                               for size in item['body_sizes']}
        item['candidate_symbols'] = sorted({other['symbol'] for other in by_address[item['vram']]
                                            if other['section'] == item['section']})
        references, accesses = {}, {}
        for signature in item['sdk_matches']:
            for kind, target_name, offsets in signature['references']:
                if kind != 'targ26':
                    continue
                for offset in offsets:
                    pc = item['vram'] + offset
                    word = int.from_bytes(rom[item['rom_offset'] + offset:item['rom_offset'] + offset + 4], 'big')
                    target = ((pc + 4) & 0xf0000000) | ((word & 0x03ffffff) << 2)
                    targets = by_address.get(target, [])
                    names = {t['symbol'] for t in targets}
                    refs = dict(pc=pc, address=target, expected_symbol=target_name,
                                kind='jal' if word >> 26 == 3 else 'j',
                                matched_symbols=sorted(names),
                                corroborated=names == {target_name} and len({t['section'] for t in targets}) == 1
                                    and not (target == item['vram'] and targets[0]['section'] == item['section']))
                    references[(pc, target_name)] = refs
        item['callees'] = [references[k] for k in sorted(references)]
        for size in item['body_sizes']:
            for access in hardware_accesses(rom[item['rom_offset']:item['rom_offset'] + size], item['vram']):
                accesses[(access['pc'], access['address'])] = access
        item['hardware_accesses'] = [accesses[k] for k in sorted(accesses)]
        if item['native_matches']:
            item['confidence'] = 'verified_native_sha256'
        elif len(item['candidate_symbols']) > 1:
            item['confidence'] = 'ambiguous_crc'
        elif any(r['corroborated'] for r in item['callees']):
            item['confidence'] = 'corroborated_crc_candidate'
        else:
            item['confidence'] = 'crc_candidate'

    seen_calls = set()
    for function in metadata['functions']:
        calls = []
        for pc in function['instruction_addresses']:
            offset = function['rom_offset'] + pc - function['vram']
            word = int.from_bytes(rom[offset:offset + 4], 'big')
            if word >> 26 in (2, 3):
                target = ((pc + 4) & 0xf0000000) | ((word & 0x03ffffff) << 2)
                calls.append((pc, target, 'jal' if word >> 26 == 3 else 'j'))
        for transfer in function.get('indirect_transfers', []):
            calls.extend((transfer['pc'], target, 'checked_' + transfer['kind'])
                         for target in transfer.get('targets', []))
        for pc, target, kind in calls:
            key = (function['section'], function['vram'], pc, target, kind)
            if key in seen_calls:
                continue
            seen_calls.add(key)
            targets = by_address.get(target, [])
            for item in targets:
                item['callers'].append(dict(section=function['section'], function=function['name'],
                    function_vram=function['vram'], pc=pc, kind=kind,
                    evidence='owned_instruction_or_checked_transfer',
                    source_native_replaced=function.get('origin') == 'native-replacement',
                    ambiguous_target_sections=len({t['section'] for t in targets}) > 1))

    # CRC-matched, undiscovered SDK bodies supply useful additional candidate
    # callers, but never become owned code or replace the metadata's evidence.
    owned_calls = {(sid, pc, target) for sid, _, pc, target, _ in seen_calls}
    for source in records.values():
        for reference in source['callees']:
            key = (source['section'], reference['pc'], reference['address'])
            if key in owned_calls:
                continue
            targets = by_address.get(reference['address'], [])
            for item in targets:
                if item['symbol'] != reference['expected_symbol']:
                    continue
                caller = dict(section=source['section'], function=source['symbol'],
                    function_vram=source['vram'], pc=reference['pc'], kind=reference['kind'],
                    evidence='sdk_signature_reference',
                    source_native_replaced=source['native']['status'] == 'native_replacement',
                    ambiguous_target_sections=len({t['section'] for t in targets}) > 1)
                if caller not in item['callers']:
                    item['callers'].append(caller)

    matches = sorted(records.values(), key=lambda r: (r['section'], r['vram'], r['symbol']))
    for item in matches:
        item['callers'].sort(key=lambda r: (r['section'], r['pc'], r['function']))
        item['review_reason'] = ('ambiguous_identification' if item['confidence'] == 'ambiguous_crc' else
            'native_boundary_selected' if item['native']['status'] == 'native_replacement' else
            'direct_hardware_access' if item['hardware_accesses'] else 'review_callers_and_behavior')
    return dict(format_version=1, game=metadata['game'], analysis_sha1=hashlib.sha1(rom).hexdigest(),
        scan_scope=scope, scanned_addresses=scanned, sources=database['sources'],
        summary=dict(matches=len(matches), matched_addresses=len({(r['section'], r['vram']) for r in matches}),
                     confidence=dict(Counter(r['confidence'] for r in matches)),
                     native_status=dict(Counter(r['native']['status'] for r in matches))),
        warnings=['CRC matches are identification candidates, not native replacement authorization.',
                  'Missing registered handlers are review items; ordinary recompiled SDK code may work.',
                  'Section scans include unowned bytes and data; a match does not establish code ownership.',
                  'Source definitions do not prove that a handler is linked into an executable.',
                  'Owned instructions/checked transfers and SDK-inferred caller references are labeled separately.',
                  'Calls inside replaced native bodies describe the original ROM, not the native execution path.'],
        matches=matches)


def report_text(report: dict, missing_only: bool = False) -> str:
    lines = [f"{report['game']}: {report['summary']['matches']} SDK matches at "
             f"{report['summary']['matched_addresses']} addresses ({report['scan_scope']})",
             f"Verified analysis SHA-1: {report['analysis_sha1']}",
             'Confidence is evidence, not a probability. Handler gaps require behavior review.', '']
    for item in report['matches']:
        if missing_only and item['native']['status'] == 'native_replacement':
            continue
        lines.append(f"s{item['section']} 0x{item['vram']:08x} {item['symbol']} "
                     f"[{item['confidence']}; {item['ownership']}; {item['native']['status']}]")
        if item['native']['selected_handler']:
            lines.append('  Selected handler: ' + item['native']['selected_handler'])
        elif item['native']['registered_handlers']:
            lines.append('  Registered handlers: ' + ', '.join(item['native']['registered_handlers']))
        for caller in item['callers']:
            suffix = ' (target section ambiguous)' if caller['ambiguous_target_sections'] else ''
            if caller['source_native_replaced']:
                suffix += ' (source body replaced natively)'
            lines.append(f"  Caller: s{caller['section']} 0x{caller['pc']:08x} "
                         f"{caller['function']} ({caller['kind']}; {caller['evidence']}){suffix}")
        for access in item['hardware_accesses']:
            lines.append(f"  Hardware {access['access']}: 0x{access['address']:08x} at 0x{access['pc']:08x}")
        if not item['callers']:
            lines.append('  No caller found in supplied metadata or matched SDK references.')
    lines.extend(['', *('Note: ' + w for w in report['warnings'])])
    return '\n'.join(lines) + '\n'


def symbol_map(report: dict) -> str:
    lines = ['# Diagnostic candidates only: SECTION ADDRESS SYMBOL CONFIDENCE']
    lines.extend(f"s{r['section']} 0x{r['vram']:08x} {r['symbol']} {r['confidence']}" for r in report['matches']
                 if len(r['candidate_symbols']) == 1)
    return '\n'.join(lines) + '\n'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--metadata', required=True, type=Path)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument('--game', choices=GAMES, help='prepare and verify the pinned game image')
    inputs.add_argument('--rom', type=Path, help='exact prepared analysis image matching metadata')
    parser.add_argument('--database', type=Path, default=DATABASE)
    parser.add_argument('--scope', choices=('entries', 'sections'), default='entries')
    parser.add_argument('--section', help='section ID or name, including overlay selection')
    parser.add_argument('--symbol', action='append', default=[], help='limit signature names; repeatable')
    parser.add_argument('--missing-only', action='store_true', help='hide selected native replacements in text output')
    parser.add_argument('--output', required=True, type=Path, help='new report directory, normally under build/')
    args = parser.parse_args()
    if args.output.exists():
        raise RecompError('output already exists; choose a new report directory')
    inputs = [args.metadata, args.database]
    if args.game:
        inputs.append(PROJECT/'games'/args.game/'profile.toml')
    fingerprints = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    if args.game:
        rom, metadata = load_analysis_rom(args.game, args.metadata)
        profile = load_profile(PROJECT/'games'/args.game/'profile.toml')
    else:
        rom = args.rom.read_bytes()
        metadata = load_metadata(args.metadata, rom)
        profile = None
    database = load_database(args.database)
    report = inspect(rom, metadata, database, profile, handler_definitions(args.game),
                     args.scope, set(args.symbol), args.section)
    if any(hashlib.sha256(p.read_bytes()).hexdigest() != digest for p, digest in fingerprints.items()):
        raise RecompError('analysis inputs changed while scanning; retry with stable files')
    report.update(metadata=str(args.metadata.resolve()), metadata_sha256=fingerprints[args.metadata],
                  database=str(args.database.resolve()), database_sha256=fingerprints[args.database])
    if args.game:
        report['profile_sha256'] = fingerprints[PROJECT/'games'/args.game/'profile.toml']
    args.output.mkdir(parents=True)
    (args.output/'report.json').write_text(json.dumps(report, indent=2) + '\n')
    text = report_text(report, args.missing_only)
    (args.output/'report.txt').write_text(text)
    (args.output/'symbols.map').write_text(symbol_map(report))
    print(text, end='')
    print(f'Report: {args.output}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RecompError, OSError, ValueError, KeyError, TypeError, ImportError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
