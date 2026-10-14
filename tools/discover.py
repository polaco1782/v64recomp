#!/usr/bin/env python3
"""Discover reachable MIPS code from a ROM and explicit load mappings, without ELF."""
from __future__ import annotations

import argparse
from bisect import bisect_right
from collections import Counter, deque
import hashlib
from pathlib import Path
import sys

from common import RecompError, load_profile, load_rom, validate_hook_prefix, write_json
from code_inventory import load_inventory, load_entry_inventory, complete_code
from emit import decode, target
from native_hooks import classify_native_bodies, resolve_native_hooks


def discover(rom: bytes, profile: dict) -> dict:
    profile = dict(profile, hooks=resolve_native_hooks(rom, profile))
    game, config = profile['game'], profile.get('discovery', {})
    regions = config.get('regions', [])
    if not regions:
        raise RecompError('ROM discovery needs explicit regions mapping ROM offsets to guest addresses')
    sections = []
    for region in regions:
        name, address, size, offset = (region[k] for k in ('name', 'vram', 'size', 'rom_offset'))
        if (name not in game['code_sections'] or address < 0 or address % 4
                or size <= 0 or size % 4 or offset < 0 or offset % 4
                or offset + size > len(rom) or address + size > 0x100000000):
            raise RecompError(f'invalid ROM discovery region: {name}')
        if any(s['name'] == name for s in sections):
            raise RecompError(f'duplicate discovery region: {name}')
        if any(address < s['vram'] + s['size'] and s['vram'] < address + size for s in sections):
            raise RecompError('overlapping load mappings need explicit overlay support; not implemented yet')
        sections.append(dict(id=len(sections), name=name, vram=address, size=size, rom_offset=offset,
                             loaded_at_boot=name in game.get('boot_sections', []),
                             sha256=hashlib.sha256(rom[offset:offset + size]).hexdigest()))
    if set(game['code_sections']) != {s['name'] for s in sections}:
        raise RecompError('discovery regions must cover every configured section')
    inventory,inventory_entries=load_inventory(config['inventory'],rom,sections) if config.get('inventory') else (None,[])
    if inventory is not None:
        inventory, inventory_entries = classify_native_bodies(rom, sections, inventory, inventory_entries, profile['hooks'])
    partial_entries = load_entry_inventory(config['entry_inventory'], rom, sections) if config.get('entry_inventory') else []
    dynamic_calls=config.get('allow_registered_indirect_calls',False)
    if type(dynamic_calls) is not bool:raise RecompError('allow_registered_indirect_calls must be boolean')
    if dynamic_calls and inventory is None and not partial_entries:
        raise RecompError('registered indirect calls require a checked code inventory or entry inventory')

    ordered_sections = sorted(sections, key=lambda s: s['vram'])
    section_starts = [s['vram'] for s in ordered_sections]
    def mapping(address):
        index = bisect_right(section_starts, address) - 1
        if index < 0:
            return None
        section = ordered_sections[index]
        return section if address < section['vram'] + section['size'] else None

    def read(address):
        section = mapping(address)
        if section is None or address % 4:
            return None
        offset = section['rom_offset'] + address - section['vram']
        return decode(rom[offset:offset + 4], address)[0]

    entry = config.get('entry_vram', int.from_bytes(rom[8:12], 'big'))
    entries, queue = {}, deque()
    limit = config.get('max_functions', 8192)
    instruction_limit = config.get('max_instructions_per_function', 16384)
    if limit <= 0 or instruction_limit <= 0:
        raise RecompError('discovery limits must be positive')

    def add_entry(address, name=None, origin='direct-call'):
        section = mapping(address)
        if section is None or address % 4:
            raise RecompError(f'unmapped or unaligned function root: {address:#010x}')
        key = (section['id'], address)
        if key not in entries:
            if len(entries) >= limit:
                raise RecompError(f'function discovery limit reached ({limit})')
            entries[key] = {'name': name or f'func_{address:08x}', 'origin': origin}
            queue.append(key)
        elif name is not None and entries[key]['name'] != name:
            raise RecompError(f'duplicate names for discovery root: {address:#010x}')
        return key

    add_entry(entry, game['entry_symbol'], 'entry-point')
    roots = config.get('roots', [])
    exclusions = config.get('exclude_roots', [])
    if not isinstance(exclusions, list) or any(not isinstance(name, str) for name in exclusions):
        raise RecompError('exclude_roots must be a list of root names')
    excluded = set(exclusions)
    if excluded - {r.get('name') for r in roots}:
        raise RecompError('excluded discovery root does not exist')
    for root in roots:
        if root.get('name') in excluded:
            continue
        add_entry(root['vram'], root.get('name'), 'profile-root')
    for root in inventory_entries:
        add_entry(root['vram'],root.get('name'),'inventory-entry')
    for root in partial_entries:
        add_entry(root['vram'], origin='checked-partial-entry')
    replacements = {}
    for hook in [*profile.get('hooks', []), *profile.get('boundaries', [])]:
        if 'vram' not in hook:
            continue
        if hook.get('kind') not in ('replace_function', 'unsupported_function'):
            raise RecompError(f"hook kind not implemented: {hook.get('kind')}")
        section = mapping(hook['vram'])
        existing = entries.get((section['id'], hook['vram'])) if section else None
        if 'catalog_body_size' in hook and existing and existing['name'] != hook['symbol']:
            key = add_entry(hook['vram'], origin='native-replacement')
            existing.setdefault('aliases', []).append(hook['symbol'])
        else:
            key = add_entry(hook['vram'], hook['symbol'], 'native-replacement')
        section = sections[key[0]]
        offset = section['rom_offset'] + key[1] - section['vram']
        size = validate_hook_prefix(rom, offset, key[1], section['vram'] + section['size'] - key[1], hook)
        if key in replacements:
            raise RecompError(f"duplicate native replacement: {hook['symbol']}")
        replacements[key] = (size, hook.get('kind'))
    transfers = {}
    table_transfers = []
    tables = [(t, 'jump') for t in config.get('jump_tables', [])]
    tables.extend((t, 'call') for t in config.get('function_tables', []))
    for table, kind in tables:
        address, count = table['table_vram'], table['count']
        section = mapping(address) if type(address) is int else None
        if (type(address) is not int or type(count) is not int or address % 4 or not 0 < count <= 4096
                or section is None or address + count * 4 > section['vram'] + section['size']):
            raise RecompError('invalid or unmapped checked dispatch table')
        offset = section['rom_offset'] + address - section['vram']
        raw = rom[offset:offset + count * 4]
        if hashlib.sha256(raw).hexdigest() != table.get('sha256'):
            raise RecompError(f'dispatch table fingerprint mismatch at {address:#010x}')
        indexes = table.get('indices', list(range(count)))
        if (not isinstance(indexes, list) or not indexes
                or any(type(i) is not int or not 0 <= i < count for i in indexes)
                or len(set(indexes)) != len(indexes)):
            raise RecompError('invalid checked dispatch table indices')
        targets = sorted({int.from_bytes(raw[i*4:i*4+4], 'big') for i in indexes})
        table_transfers.append(dict(pc=table['pc'], expected_word=table['expected_word'], kind=kind,
                                    targets=targets, table_vram=address, table_count=count,
                                    table_indices=indexes, table_sha256=table['sha256']))
    for transfer in [*config.get('transfers', []), *table_transfers]:
        pc = transfer['pc']
        i = read(pc)
        kind = transfer['kind']
        targets = transfer['targets']
        if (i is None or i.word != transfer['expected_word'] or pc in transfers
                or kind not in ('tail', 'jump', 'call') or not targets or len(set(targets)) != len(targets)
                or (kind == 'call' and i.opcode != 'jalr')
                or (kind != 'call' and (i.opcode != 'jr' or i.rs == 31))
                or any(mapping(a) is None or a % 4 for a in targets)):
            raise RecompError(f'invalid or mismatched checked indirect transfer at {pc:#010x}')
        transfers[pc] = transfer
    fallthroughs = {}
    for transfer in config.get('fallthroughs', []):
        pc, destination = transfer['pc'], transfer['target']
        i=read(pc)
        if (i is None or i.delay or i.word != transfer['expected_word'] or pc in fallthroughs
                or destination != pc+4 or mapping(destination) != mapping(pc)
                or not isinstance(transfer.get('reason'),str) or not transfer['reason']):
            raise RecompError(f'invalid checked function fallthrough at {pc:#010x}')
        add_entry(destination)
        fallthroughs[pc]=transfer

    def walk(key):
        section_id, start = key
        section = sections[section_id]
        if key in replacements:
            # Only the fingerprint is classified here. No function boundary is
            # inferred, and privileged OS internals are not followed past it.
            size, kind = replacements[key]
            offset = section['rom_offset'] + start - section['vram']
            return dict(name=entries[key]['name'], aliases=entries[key].get('aliases', []), section=section_id, vram=start, size=size,
                        rom_offset=offset, sha256=hashlib.sha256(rom[offset:offset + size]).hexdigest(),
                        size_source='native-replacement-prefix' if kind == 'replace_function' else 'unsupported-boundary-prefix',
                        origin='native-replacement' if kind == 'replace_function' else 'unsupported-boundary',
                        instruction_addresses=list(range(start, start + size, 4)), discovery_errors=[],
                        indirect_transfers=[])
        pending, visited, expanded, errors, used_transfers = deque([start]), set(), set(), [], []
        used_fallthroughs=[]

        def error(pc, reason):
            i = read(pc)
            errors.append({'pc': pc, 'word': i.word if i else 0,
                           'assembly': i.assembly if i else '', 'reason': reason})

        def local(address, source):
            destination = mapping(address)
            if destination is None or address % 4:
                error(source, f'unmapped control-flow target: {address:#010x}')
            elif destination['id'] != section_id or (section_id, address) in entries and address != start:
                add_entry(address, origin='tail-transfer')
            elif address < start:
                error(source, f'branch below function entry requires ownership review: {address:#010x}')
            else:
                pending.append(address)

        def call(address, source):
            if mapping(address) is None or address % 4:
                error(source, f'unmapped call target: {address:#010x}')
            else:
                add_entry(address)

        while pending:
            pc = pending.popleft()
            if pc in expanded:
                continue
            # A word decoded as a delay slot still needs expansion if another
            # branch enters it as an ordinary instruction.
            expanded.add(pc)
            if pc != start and (section_id, pc) in entries:
                error(pc, 'fallthrough reaches another function entry; boundary needs review')
                continue
            if mapping(pc) != section or pc < start:
                error(pc, 'fallthrough leaves mapped function section')
                continue
            if len(visited) >= instruction_limit:
                error(pc, f'instruction discovery limit reached ({instruction_limit})')
                break
            i = read(pc)
            visited.add(pc)
            if i.opcode == 'invalid':
                error(pc, 'invalid instruction on reachable path')
                continue
            if i.opcode == 'break':
                # A fatal native trap terminates this path; it is not a stub.
                continue
            if i.opcode in ('syscall', 'eret'):
                error(pc, f'{i.opcode} requires a native exception/OS boundary')
                continue
            if not i.delay:
                if pc in fallthroughs:
                    used_fallthroughs.append(fallthroughs[pc])
                    continue
                pending.append(pc + 4)
                continue
            slot = read(pc + 4)
            if slot is None or mapping(pc + 4) != section or slot.delay or slot.opcode == 'invalid':
                error(pc, 'missing/invalid delay slot or control transfer in delay slot')
                continue
            visited.add(pc + 4)
            if i.opcode == 'jr' and i.rs == 31:
                continue
            if i.opcode in ('jr', 'jalr'):
                override = transfers.get(pc)
                if i.opcode=='jalr' and dynamic_calls:
                    if override and override['kind']!='call':
                        error(pc,'indirect call metadata kind mismatch');continue
                    if override:
                        for address in override['targets']:call(address,pc)
                    used_transfers.append(dict(pc=pc,expected_word=i.word,kind='call',targets=[],registered_targets=True))
                    pending.append(pc+8)
                    continue
                if override is None:
                    error(pc, 'unresolved indirect transfer; add checked targets or a local replacement')
                    if i.opcode == 'jalr':
                        pending.append(pc + 8)
                    continue
                used_transfers.append(override)
                for address in override['targets']:
                    if override['kind'] == 'jump':
                        if mapping(address) != section or address < start or (section_id, address) in entries and address != start:
                            error(pc, 'computed local jump crosses a function/section boundary')
                        else:
                            pending.append(address)
                    else:
                        call(address, pc)
                if override['kind'] == 'call':
                    pending.append(pc + 8)
                continue
            if i.opcode == 'jal':
                call(target(i), pc)
                pending.append(pc + 8)
            elif i.opcode == 'j':
                if target(i) < start:
                    call(target(i), pc)
                else:
                    local(target(i), pc)
            elif i.opcode in ('bltzal', 'bgezal', 'bltzall', 'bgezall'):
                call(target(i), pc)
                pending.append(pc + 8)
            else:
                # Follow both possible paths, except branches provable from
                # register identity/zero alone. Never use a guessed register value.
                always = i.opcode == 'b' or i.opcode in ('beq', 'beql') and i.rs == i.rt
                never = i.opcode in ('bne', 'bnel') and i.rs == i.rt
                if not never:
                    local(target(i), pc)
                if not always:
                    pending.append(pc + 8)
        addresses = sorted(visited)
        size = addresses[-1] + 4 - start
        offset = section['rom_offset'] + start - section['vram']
        return dict(name=entries[key]['name'], aliases=[], section=section_id, vram=start, size=size,
                    rom_offset=offset, sha256=hashlib.sha256(rom[offset:offset + size]).hexdigest(),
                    size_source='rom-control-flow', origin=entries[key]['origin'],
                    instruction_addresses=addresses, discovery_errors=errors,
                    indirect_transfers=used_transfers, fallthrough_transfers=used_fallthroughs)

    while queue:
        walk(queue.popleft())
    # Calls found later can turn an earlier followed branch into a tail call.
    # Revisit with the complete entry set before assigning instruction ownership.
    functions = [walk(key) for key in sorted(entries)]
    owners, issues = {}, []
    conflicts = set()
    for function in functions:
        key = (function['section'], function['vram'])
        for pc in function['instruction_addresses']:
            word_key = (function['section'], pc)
            if word_key in owners:
                conflicts.update((key, owners[word_key]))
                issues.append({'section': function['section'], 'vram': pc,
                               'reason': 'ambiguous instruction ownership; functions omitted pending review'})
            else:
                owners[word_key] = key
    functions = [f for f in functions if (f['section'], f['vram']) not in conflicts]
    inventory_summary=complete_code(rom,sections,functions,inventory,set(replacements)) if inventory else None
    visited_by_section = {s['id']: set() for s in sections}
    for f in functions:
        visited_by_section[f['section']].update(f['instruction_addresses'])
    unvisited = []
    for section in sections:
        cursor = section['vram']
        for pc in sorted(visited_by_section[section['id']]):
            if cursor < pc:
                unvisited.append(dict(section=section['id'], vram=cursor, size=pc - cursor))
            cursor = pc + 4
        if cursor < section['vram'] + section['size']:
            unvisited.append(dict(section=section['id'], vram=cursor, size=section['vram'] + section['size'] - cursor))
    if unvisited and inventory is None:
        issues.append({'reason': 'unvisited mapped bytes may be data or undiscovered code; full classification is incomplete',
                       'size': sum(r['size'] for r in unvisited)})
    symbols = {f['name']: f['vram'] for f in functions}
    for name, address in config.get('symbols', {}).items():
        if name in symbols and symbols[name] != address:
            raise RecompError(f'data symbol conflicts with function name: {name}')
        symbols[name] = address
    return dict(format_version=1, metadata_source='rom-control-flow', game=game['name'],
                rom_sha1=hashlib.sha1(rom).hexdigest(), rom_size=len(rom), entry_vram=entry,
                sections=sections, functions=functions, symbols=symbols, issues=issues,
                unvisited_ranges=unvisited,
                code_inventory=inventory_summary,
                discovery_summary=dict(functions=len(functions), omitted_conflicting_functions=len(conflicts),
                                       instructions=sum(len(f['instruction_addresses']) for f in functions),
                                       unresolved_functions=sum(bool(f['discovery_errors']) for f in functions),
                                       origins=dict(Counter(f['origin'] for f in functions))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--rom', type=Path)
    args = parser.parse_args()
    profile = load_profile(args.profile)
    game = profile['game']
    rom, _ = load_rom(args.rom or game['rom'], game['rom_sha1'], game['rom_size'])
    metadata = discover(rom, profile)
    write_json(args.output, metadata)
    print(f"ROM discovery: {metadata['discovery_summary']}")
    print(f"Unvisited ranges: {len(metadata['unvisited_ranges'])}; metadata: {args.output}")


if __name__ == '__main__':
    try:
        main()
    except (RecompError, OSError, ValueError, KeyError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
