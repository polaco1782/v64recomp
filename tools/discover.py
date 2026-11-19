#!/usr/bin/env python3
"""Discover reachable MIPS code from a ROM and explicit load mappings, without ELF."""
from __future__ import annotations

import argparse
from collections import Counter, deque
import hashlib
from pathlib import Path
import sys

from common import RecompError, load_profile, load_rom, validate_recomp_prefix, write_json, shared_return_delay_slot
from code_inventory import load_inventory, load_entry_inventory, complete_code
from emit import constant_condition, decode, target
from native_hooks import classify_native_bodies, resolve_native_hooks
from metadata_candidates import (fixed_callback_stores, indexed_call_tables, indirect_parameter_uses, initializer_callback_stores,
                                 materialized_entries, preserves_stack_words, section_pages, shared_entry_slots,
                                 visible_shared_slots)


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
        group = region.get('overlay_group')
        if group is not None and (not isinstance(group, str) or not group or name in game.get('boot_sections', [])):
            raise RecompError('overlay_group requires a nonempty name on a section unloaded at boot')
        for s in sections:
            if address < s['vram'] + s['size'] and s['vram'] < address + size:
                if not group or group != s.get('overlay_group'):
                    raise RecompError('overlapping load mappings require the same explicit overlay_group')
        sections.append(dict(id=len(sections), name=name, vram=address, size=size, rom_offset=offset,
                             loaded_at_boot=name in game.get('boot_sections', []),
                             sha256=hashlib.sha256(rom[offset:offset + size]).hexdigest()))
        if group:
            sections[-1]['overlay_group'] = group
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

    current_section = None
    declared_entries = [*partial_entries, *inventory_entries, *config.get('roots', [])]
    by_section = {identifier: s for s in sections for identifier in (s['id'], s['name'])}
    pages = section_pages(sections)
    def mapping(address, preferred=None):
        preferred = current_section if preferred is None else preferred
        if preferred is not None:
            section = by_section.get(preferred)
            if section is None:
                raise RecompError(f'unknown discovery section: {preferred}')
            if section['vram'] <= address < section['vram'] + section['size']:
                return section
        matches = [s for s in pages.get(address >> 12, ())
                   if s['vram'] <= address < s['vram'] + s['size']]
        if len(matches) > 1:
            declared = {root['section'] for root in declared_entries
                        if root.get('vram') == address and 'section' in root}
            selected = [s for s in matches if s['name'] in declared or s['id'] in declared]
            if len(selected) == 1:
                return selected[0]
            raise RecompError(f'ambiguous overlay address {address:#010x}; select its section explicitly')
        return matches[0] if matches else None

    def read(address):
        section = mapping(address)
        if section is None or address % 4:
            return None
        offset = section['rom_offset'] + address - section['vram']
        return decode(rom[offset:offset + 4], address)[0]

    # Some guest APIs retain a callback in RAM for a different function to
    # invoke. A checked ABI ties literal arguments to that reviewed consumer.
    callback_parameters = {}
    for contract in config.get('callback_parameters', []):
        if not dynamic_calls or inventory is not None:
            raise RecompError('callback parameters require partial registered-call discovery')
        address, size = contract.get('vram'), contract.get('size')
        register, pc = contract.get('register'), contract.get('call_pc')
        section = next((s for s in sections if s['name'] == contract.get('section')), None)
        if (section is None or type(register) is not int or register not in range(4, 8)
                or any(type(n) is not int or n % 4 for n in (address, size, pc))
                or size <= 0 or min(address, pc) < section['vram']
                or max(address + size, pc + 4) > section['vram'] + section['size']):
            raise RecompError('invalid checked callback parameter')
        offset = section['rom_offset'] + address - section['vram']
        digest = hashlib.sha256(rom[offset:offset + size]).hexdigest()
        current_section = section['id']
        call = read(pc)
        key = (section['id'], address, register)
        if (key in callback_parameters or digest != contract.get('sha256')
                or call.opcode != 'jalr' or call.rd != 31 or call.word != contract.get('expected_word')):
            raise RecompError(f'invalid checked callback contract at {address:#010x}')
        callback_parameters[key] = contract
    current_section = None

    registered_calls = {}
    if config.get('registered_direct_calls') and inventory is not None:
        raise RecompError('runtime-registered direct calls require partial discovery, not a complete code inventory')
    for site in config.get('registered_direct_calls', []):
        current_section = site.get('section')
        pc = site.get('pc')
        if type(pc) is not int:
            raise RecompError('registered direct call requires an instruction PC')
        instruction, section = read(pc), mapping(pc)
        if (instruction is None or instruction.opcode != 'jal'
                or instruction.word != site.get('expected_word')
                or target(instruction) != site.get('target')
                or (section['id'], pc) in registered_calls):
            raise RecompError(f'invalid checked registered direct call at {pc:#010x}')
        registered_calls[section['id'], pc] = dict(pc=pc, expected_word=instruction.word,
                                                  target=target(instruction))
    current_section = None

    entry = config.get('entry_vram', int.from_bytes(rom[8:12], 'big'))
    entries, queue = {}, deque()
    limit = config.get('max_functions', 8192)
    instruction_limit = config.get('max_instructions_per_function', 16384)
    if limit <= 0 or instruction_limit <= 0:
        raise RecompError('discovery limits must be positive')

    def add_entry(address, name=None, origin='direct-call', preferred=None):
        section = mapping(address, preferred)
        if (section is None or address % 4 or preferred is not None
                and preferred not in (section['name'], section['id'])):
            raise RecompError(f'unmapped or unaligned function root: {address:#010x}')
        key = (section['id'], address)
        if key not in entries:
            if len(entries) >= limit:
                raise RecompError(f'function discovery limit reached ({limit})')
            default = (f'func_s{section["id"]}_{address:08x}' if section.get('overlay_group')
                       else f'func_{address:08x}')
            entries[key] = {'name': name or default, 'origin': origin}
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
        add_entry(root['vram'], root.get('name'), 'profile-root', root.get('section'))
    for root in inventory_entries:
        add_entry(root['vram'],root.get('name'),'inventory-entry',root.get('section'))
    for root in partial_entries:
        add_entry(root['vram'], origin='checked-partial-entry', preferred=root.get('section'))
    replacements = {}
    native_callbacks = {}
    for hook in [*profile.get('hooks', []), *profile.get('boundaries', [])]:
        if 'vram' not in hook:
            continue
        if hook.get('kind') not in ('replace_function', 'unsupported_function'):
            raise RecompError(f"hook kind not implemented: {hook.get('kind')}")
        section = mapping(hook['vram'], hook.get('section'))
        existing = entries.get((section['id'], hook['vram'])) if section else None
        if 'catalog_body_size' in hook and existing and existing['name'] != hook['symbol']:
            key = add_entry(hook['vram'], origin='native-replacement', preferred=section['id'])
            existing.setdefault('aliases', []).append(hook['symbol'])
        else:
            key = add_entry(hook['vram'], hook['symbol'], 'native-replacement', section['id'])
        section = sections[key[0]]
        offset = section['rom_offset'] + key[1] - section['vram']
        size = validate_recomp_prefix(rom, offset, key[1], section['vram'] + section['size'] - key[1], hook)
        # Shared native signatures verify the complete body. A custom handler
        # overriding one of them may check only a short prefix, but it must
        # still replace the entire recognized body in the function registry.
        if 'catalog_body_size' in hook:
            size = hook['catalog_body_size']
        if key in replacements:
            raise RecompError(f"duplicate native replacement: {hook['symbol']}")
        replacements[key] = (size, hook.get('kind'))
        # A complete catalog match establishes the shared handler's ABI. Its
        # thread entry is a deferred function call even though native OS bodies
        # are deliberately excluded from translated-callee dataflow analysis.
        if hook.get('catalog_body_size') and hook.get('handler') == 'recomp_os_create_thread':
            native_callbacks[key] = {6:hook['handler']}
    transfers_by_section = {}
    table_transfers = []
    tables = [(t, 'jump') for t in config.get('jump_tables', [])]
    tables.extend((t, 'call') for t in config.get('function_tables', []))
    for table, kind in tables:
        current_section = table.get('section')
        address, count = table['table_vram'], table['count']
        stride, field = table.get('stride', 4), table.get('field_offset', 0)
        nullable = table.get('nullable', False)
        if (type(stride) is not int or stride < 4 or stride % 4 or type(field) is not int
                or field % 4 or not 0 <= field < stride or type(nullable) is not bool
                or (kind == 'jump' and (stride != 4 or nullable))):
            raise RecompError('invalid checked dispatch table layout')
        section = mapping(address) if type(address) is int else None
        if (type(address) is not int or type(count) is not int or address % 4 or not 0 < count <= 8192
                or section is None or address + count * stride > section['vram'] + section['size']):
            raise RecompError('invalid or unmapped checked dispatch table')
        offset = section['rom_offset'] + address - section['vram']
        raw = rom[offset:offset + count * stride]
        if hashlib.sha256(raw).hexdigest() != table.get('sha256'):
            raise RecompError(f'dispatch table fingerprint mismatch at {address:#010x}')
        indexes = table.get('indices', list(range(count)))
        if (not isinstance(indexes, list) or not indexes
                or any(type(i) is not int or not 0 <= i < count for i in indexes)
                or len(set(indexes)) != len(indexes)):
            raise RecompError('invalid checked dispatch table indices')
        slots = [i * stride + field for i in indexes]
        pointers = {int.from_bytes(raw[pos:pos + 4], 'big') for pos in slots}
        if nullable:
            pointers.discard(0)
        targets = sorted(pointers)
        table_transfers.append(dict(pc=table['pc'], expected_word=table['expected_word'], kind=kind,
                                    targets=targets, table_vram=address, table_count=count,
                                    table_indices=indexes, table_sha256=table['sha256'], section=section['name'],
                                    **({'table_stride': stride, 'table_field_offset': field, 'nullable': nullable}
                                       if stride != 4 or nullable else {})))
    for transfer in [*config.get('transfers', []), *table_transfers]:
        current_section = transfer.get('section')
        pc = transfer['pc']
        i = read(pc)
        section = mapping(pc)
        key = (section['id'], pc) if section else None
        kind = transfer['kind']
        targets = transfer['targets']
        if (i is None or i.word != transfer['expected_word'] or key in transfers_by_section
                or kind not in ('tail', 'jump', 'call', 'return')
                or (bool(targets) == (kind == 'return')) or len(set(targets)) != len(targets)
                or (kind == 'call' and i.opcode != 'jalr')
                or (kind != 'call' and (i.opcode != 'jr' or i.rs == 31))
                or any(mapping(a) is None or a % 4 for a in targets)):
            raise RecompError(f'invalid or mismatched checked indirect transfer at {pc:#010x}')
        if kind == 'return':
            save = read(transfer.get('save_pc', -1))
            link_entry = read(transfer.get('entry_pc', -1))
            # An explicit saved-RA return also checks the instruction copying RA
            # into its alternate register. Emission checks the runtime target.
            saved_ra = not (save is None or save.word != transfer.get('expected_save_word')
                    or save.opcode not in ('or', 'addu') or save.rs != 31 or save.rt != 0
                    or save.rd != i.rs or mapping(save.pc) != mapping(pc) or save.pc >= pc)
            entry_link = (link_entry is not None and link_entry.word == transfer.get('expected_entry_word')
                          and mapping(link_entry.pc) == mapping(pc) and link_entry.pc < pc
                          and type(transfer.get('entry_register')) is int
                          and 0 < transfer.get('entry_register', 0) == i.rs < 31)
            if ((not entry_link or 'save_pc' in transfer or 'expected_save_word' in transfer)
                    if 'entry_register' in transfer else not saved_ra):
                raise RecompError(f'invalid checked return save at {pc:#010x}')
        transfers_by_section[key] = transfer
    fallthroughs_by_section = {}
    for transfer in config.get('fallthroughs', []):
        current_section = transfer.get('section')
        pc, destination = transfer['pc'], transfer['target']
        i=read(pc)
        section = mapping(pc)
        key = (section['id'], pc) if section else None
        if (i is None or i.delay or i.word != transfer['expected_word'] or key in fallthroughs_by_section
                or destination != pc+4 or mapping(destination) != mapping(pc)
                or not isinstance(transfer.get('reason'),str) or not transfer['reason']):
            raise RecompError(f'invalid checked function fallthrough at {pc:#010x}')
        add_entry(destination)
        fallthroughs_by_section[key]=transfer
    continuations_by_section = {}
    for site in config.get('tail_continuations', []):
        current_section = site.get('section')
        pc = site['pc']
        i = read(pc)
        section = mapping(pc)
        key = (section['id'], pc) if section else None
        resume = site['resume_pc']
        if (i is None or not i.delay or i.opcode in ('jr', 'jal', 'jalr', 'bltzal', 'bgezal', 'bltzall', 'bgezall')
                or i.word != site['expected_word'] or key in continuations_by_section
                or type(resume) is not int or resume % 4 or mapping(resume) != mapping(pc)):
            raise RecompError(f'invalid checked tail continuation at {pc:#010x}')
        continuations_by_section[key] = site
    current_section = None

    def infer_local_switch(jump, start, section, expanded, visited):
        # The shift may be in the guard's delay slot or immediately after it.
        # In the latter layout the slot must preserve the checked index.
        first = jump.pc - 24
        if first < start:
            return None
        scale, high, add, load = (read(pc) for pc in range(jump.pc - 16, jump.pc, 4))
        if read(first).opcode == 'beq':
            first -= 4
        if first < start or first not in visited:
            return None
        bound, branch = read(first), read(first + 4)
        if ([i.opcode for i in (bound, branch, scale, high, add, load)] !=
                ['sltiu', 'beq', 'sll', 'lui', 'addu', 'lw']):
            return None
        if scale.pc != branch.pc + 4:
            slot = read(branch.pc + 4)
            if (scale.pc not in expanded or slot.gpr_write_mask & (1 << bound.rs)
                    or not (slot.opcode in ('or', 'addu', 'sll') or (slot.opcode == 'sw' and slot.rs == 29))):
                return None
        if (not 0 < bound.immediate <= 8192 or not bound.rs or not bound.rt or bound.rt == bound.rs
                or {branch.rs, branch.rt} != {0, bound.rt} or target(branch) < jump.pc + 8
                or scale.rt != bound.rs or scale.shift != 2 or not scale.rd
                or not high.rt or high.rt == scale.rd
                or {add.rs, add.rt} != {scale.rd, high.rt} or not add.rd
                or load.rs != add.rd or not load.rt or jump.rs != load.rt):
            return None

        # The comparison can itself be a delay slot. On the not-taken
        # path of an ordinary conditional branch that slot still executes.
        if first not in expanded:
            before = read(first - 4)
            if (first - 4 not in expanded or before.likely or before.opcode not in
                    ('beq', 'bne', 'bltz', 'bgez', 'blez', 'bgtz', 'bc1t', 'bc1f')
                    or constant_condition(before) is True):
                return None
        if any(pc not in expanded for pc in (branch.pc, high.pc, add.pc, load.pc)):
            return None
        if any((section['id'], pc) in fallthroughs_by_section or
               (section['id'], pc) in continuations_by_section for pc in range(first, jump.pc, 4)):
            return None

        base = (((high.word & 0xffff) << 16) + load.immediate) & 0xffffffff
        size = 4 * bound.immediate
        tables = [s for s in sections if s['vram'] <= base < s['vram'] + s['size']]
        if len(tables) != 1 or base % 4 or base + size > tables[0]['vram'] + tables[0]['size']:
            return None
        table = tables[0]
        if table.get('overlay_group') and table != section:
            return None
        offset = table['rom_offset'] + base - table['vram']
        raw = rom[offset:offset + size]
        targets = sorted({int.from_bytes(raw[n:n + 4], 'big') for n in range(0, size, 4)})
        # New case blocks must be forward and local. Backward table entries
        # can reuse a block already reached independently in this function.
        next_entry = min((pc for sid, pc in entries if sid == section['id'] and pc > start),
                         default=section['vram'] + section['size'])
        if any(pc % 4 or pc >= next_entry or (pc < jump.pc + 8 and pc not in expanded)
               or read(pc).opcode == 'invalid'
               for pc in targets):
            return None
        return dict(pc=jump.pc, expected_word=jump.word, kind='jump', targets=targets,
                    table_vram=base, table_count=bound.immediate, table_sha256=hashlib.sha256(raw).hexdigest(),
                    origin='bounded-switch', guard_pc=first)

    def walk(key):
        nonlocal current_section
        section_id, start = key
        current_section = section_id
        section = sections[section_id]
        transfers = {pc:t for (sid,pc),t in transfers_by_section.items() if sid == section_id}
        fallthroughs = {pc:t for (sid,pc),t in fallthroughs_by_section.items() if sid == section_id}
        continuations = {pc:t for (sid,pc),t in continuations_by_section.items() if sid == section_id}
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
        used_registered_calls = []
        used_fallthroughs=[]
        used_continuations=[]

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
            checked = registered_calls.get((section_id, source))
            if checked:
                used_registered_calls.append(checked)
                return
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
            if i.opcode in ('break','syscall'):
                # A fatal native trap terminates this path; it is not a stub.
                continue
            if i.opcode == 'eret':
                error(pc, f'{i.opcode} requires a native exception/OS boundary')
                continue
            if not i.delay:
                if pc in fallthroughs:
                    used_fallthroughs.append(fallthroughs[pc])
                    if 'resume_pc' in fallthroughs[pc]:
                        local(fallthroughs[pc]['resume_pc'], pc)
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
                if override is None and i.opcode == 'jr' and inventory is None:
                    override = infer_local_switch(i, start, section, expanded, visited)
                if override is None:
                    error(pc, 'unresolved indirect transfer; add checked targets or a local replacement')
                    if i.opcode == 'jalr':
                        pending.append(pc + 8)
                    continue
                used_transfers.append(override)
                if override['kind'] == 'return' and 'entry_register' in override and override['entry_pc'] != start:
                    error(pc, 'checked link register belongs to another function entry')
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
                if pc in continuations:
                    used_continuations.append(continuations[pc])
                    call(target(i), pc)
                    local(continuations[pc]['resume_pc'], pc)
                elif target(i) < start:
                    call(target(i), pc)
                else:
                    local(target(i), pc)
            elif i.opcode in ('bltzal', 'bgezal', 'bltzall', 'bgezall'):
                # BLTZAL zero is also used solely to materialize PC+8 in r31.
                # Its never-taken destination can be embedded data, not a callee.
                if constant_condition(i) is not False:
                    call(target(i), pc)
                pending.append(pc + 8)
            else:
                # Follow both possible paths, except branches provable from
                # register identity/zero alone. Never use a guessed register value.
                condition = constant_condition(i)
                if condition is not False:
                    if pc in continuations:
                        used_continuations.append(continuations[pc])
                        call(target(i), pc)
                        local(continuations[pc]['resume_pc'], pc)
                    else:
                        local(target(i), pc)
                if condition is not True:
                    # A checked fallthrough may sit in the delay slot of the
                    # final branch. It applies only to the not-taken successor.
                    if pc + 4 in fallthroughs:
                        used_fallthroughs.append(fallthroughs[pc + 4])
                        if 'resume_pc' in fallthroughs[pc + 4]:
                            local(fallthroughs[pc + 4]['resume_pc'], pc)
                    else:
                        pending.append(pc + 8)
        inferred = [t for t in used_transfers if t.get('origin') == 'bounded-switch']
        if inferred:
            branch_entries = {target(i) for pc in expanded if pc in visited
                              and (i := read(pc)).delay and i.opcode not in ('jr', 'jalr', 'jal')
                              and constant_condition(i) is not False}
            branch_entries.update(addr for t in used_transfers if t['kind'] == 'jump'
                                  for addr in t['targets'])
            branch_entries.update(t['resume_pc'] for t in [*used_fallthroughs, *used_continuations]
                                  if 'resume_pc' in t)
            for switch in inferred:
                if any(pc in branch_entries for pc in range(switch['guard_pc'] + 4, switch['pc'] + 4, 4)):
                    error(switch['pc'], 'alternate entry into inferred switch requires checked targets')
        addresses = sorted(visited)
        size = addresses[-1] + 4 - start
        offset = section['rom_offset'] + start - section['vram']
        result = dict(name=entries[key]['name'], aliases=[], section=section_id, vram=start, size=size,
                    rom_offset=offset, sha256=hashlib.sha256(rom[offset:offset + size]).hexdigest(),
                    size_source='rom-control-flow', origin=entries[key]['origin'],
                    instruction_addresses=addresses, discovery_errors=errors,
                    indirect_transfers=used_transfers, fallthrough_transfers=used_fallthroughs)
        if entries[key].get('sources'):
            result['entry_sources'] = entries[key]['sources']
        if used_registered_calls:
            result['registered_direct_calls'] = sorted(used_registered_calls, key=lambda site: site['pc'])
        if used_continuations:
            result['tail_call_continuations'] = used_continuations
        link_registers = {t['entry_register'] for t in used_transfers if 'entry_register' in t}
        if link_registers:
            if len(link_registers) != 1:
                raise RecompError(f'conflicting entry link registers at {start:#010x}')
            result['return_address_register'] = link_registers.pop()
        return result

    while True:
        while queue:
            walk(queue.popleft())
        # Calls found later can turn an earlier branch into a tail call. Settle
        # ownership before using owned instructions as callback producers.
        functions = [walk(key) for key in sorted(entries)]
        if queue:
            continue
        if not dynamic_calls or inventory is not None:
            break
        by_address = {}
        for f in functions:
            by_address.setdefault(f['vram'], []).append(f)
        overlapping_sections = {
            section['id'] for section in sections
            if any(section['id'] != other['id']
                   and section['vram'] < other['vram'] + other['size']
                   and other['vram'] < section['vram'] + section['size']
                   for other in sections)
        }
        unambiguous_sections = [dict(section, section=section['id']) for section in sections
                                if section['id'] not in overlapping_sections]
        owned_by_section = {section['id']: set() for section in sections}
        for function in functions:
            owned_by_section[function['section']].update(function['instruction_addresses'])
        unambiguous_owned = set().union(*(owned_by_section[section['id']]
                                          for section in unambiguous_sections))
        text_by_section = {
            section['id']: (unambiguous_sections if section['id'] not in overlapping_sections
                            else [dict(other, section=other['id']) for other in sections
                                  if other['id'] == section['id'] or other['id'] not in overlapping_sections])
            for section in sections
        }
        owned_by_selection = {
            section['id']: (unambiguous_owned if section['id'] not in overlapping_sections
                            else unambiguous_owned | owned_by_section[section['id']])
            for section in sections
        }
        shared_slots = visible_shared_slots(sections, overlapping_sections, shared_entry_slots(rom, functions))
        called_slots = {s['id']: {} for s in sections}
        for f in functions:
            if f['discovery_errors'] or not any(t['kind'] == 'call' for t in f['indirect_transfers']):
                continue
            for site in indexed_call_tables(rom, f, fixed_slots=True):
                address = site['slot_vram']
                if 0x80000000 <= address < 0x80000000 + game['ram_size']:
                    called_slots[f['section']].setdefault(address, []).append(
                        dict(site, function=f['vram']))

        stack_preservation = {}
        def callee_preserves_stack(address):
            matches = by_address.get(address, [])
            if len(matches) != 1:
                return False
            if address not in stack_preservation:
                stack_preservation[address] = preserves_stack_words(rom, matches[0])
            return stack_preservation[address]

        direct_parameters = {}
        def called_parameters(address):
            matches = by_address.get(address, [])
            if len(matches) != 1:
                return {}
            f = matches[0]
            if f['discovery_errors'] or f['origin'] in ('native-replacement', 'unsupported-boundary'):
                return {}
            if address not in direct_parameters:
                direct_parameters[address] = indirect_parameter_uses(rom, f, callee_preserves_stack)
            return direct_parameters[address]

        initializer_stores = initializer_callback_stores(rom, functions, callback_parameters)
        parameters = {}
        for function in functions:
            if function['discovery_errors'] or function['origin'] in ('native-replacement','unsupported-boundary'):
                continue
            current_section = function['section']
            # Infer callbacks only where the caller selects a mapping, or the
            # target has one unambiguous mapping. Other overlay roots require
            # a checked inventory entry with an explicit section.
            candidates = materialized_entries(rom,{'functions':functions},text_by_section[current_section],
                source_function=function,owned_addresses=owned_by_selection[current_section],
                shared_entries=shared_slots[current_section])
            for candidate in candidates:
                sources = []
                for use in candidate['address_uses']:
                    source = dict(section=function['section'],function=function['vram'],
                                  pc=use['pc'],expected_word=read(use['pc']).word,kind=use['kind'])
                    if use['kind']=='constant-indirect-call':
                        sources.append(source)
                    elif use['kind'] == 'stored-address':
                        for proof in initializer_stores.get((current_section, use['pc']), []):
                            sources.append(dict(source, initializer_flow=proof))
                    elif use['kind'] in ('call-argument','stack-argument') and use.get('callee') in by_address:
                        matches = by_address[use['callee']]
                        callee = next((f for f in matches if f['section'] == current_section),
                                      matches[0] if len(matches) == 1 else None)
                        if callee is None:
                            continue
                        callee_key = (callee['section'], callee['vram'])
                        handler = (native_callbacks.get(callee_key,{}).get(use['register'])
                                   if use['kind'] == 'call-argument' else None)
                        if handler:
                            sources.append(dict(source,callee=callee['vram'],
                                                argument_register=use['register'],native_handler=handler,
                                                callee_sha256=callee['sha256']))
                            continue
                        if callee['discovery_errors'] or callee['origin'] in ('native-replacement','unsupported-boundary'):
                            continue
                        if use['kind'] == 'call-argument':
                            contract = callback_parameters.get((*callee_key, use['register']))
                            if contract is not None:
                                if callee['size'] != contract['size'] or callee['sha256'] != contract['sha256']:
                                    raise RecompError(f"callback callee body mismatch at {callee['vram']:#010x}")
                                sources.append(dict(source, callee=callee['vram'], callee_sha256=callee['sha256'],
                                                    argument_register=use['register'],
                                                    indirect_calls=[dict(pc=contract['call_pc'],
                                                                         expected_word=contract['expected_word'])]))
                                continue
                        if callee_key not in parameters:
                            uses = indirect_parameter_uses(rom, callee, callee_preserves_stack, called_parameters)
                            for reg, sites in fixed_callback_stores(rom, callee, called_slots[callee['section']]).items():
                                uses.setdefault(reg, []).extend(sites)
                            parameters[callee_key] = uses
                        parameter = (('stack',use['stack_offset']) if use['kind'] == 'stack-argument'
                                     else use['register'])
                        sites = parameters[callee_key].get(parameter,[])
                        if sites:
                            location = ({'argument_stack_offset':use['stack_offset'],'store_pc':use['store_pc']}
                                        if use['kind'] == 'stack-argument'
                                        else {'argument_register':use['register']})
                            sources.append(dict(source,callee=callee['vram'],indirect_calls=sites,**location))
                if sources:
                    key = add_entry(candidate['vram'],origin='inferred-callback')
                    entries[key].setdefault('sources',[]).extend(sources)
        if not queue:
            break
    owners, issues = {}, []
    functions_by_key = {(f['section'], f['vram']): f for f in functions}
    conflicts = set()
    for function in functions:
        key = (function['section'], function['vram'])
        for pc in function['instruction_addresses']:
            word_key = (function['section'], pc)
            if word_key in owners and not shared_return_delay_slot(
                    rom, sections[function['section']], pc, functions_by_key[owners[word_key]], function):
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
