#!/usr/bin/env python3
"""Find potentially missing function entries before gameplay reaches them.

Scan owned address-producing code and aligned pointers throughout the verified
analysis image, including script/assets outside CPU mappings. Probe candidate
control flow without registering addresses or changing checked inventories.
"""
from __future__ import annotations

import argparse
from collections import Counter, deque
import hashlib
from pathlib import Path
import struct
import sys

from common import RecompError, load_metadata, write_json
from discover import infer_local_switch
from emit import SUPPORTED, constant_condition, decode, target
from analysis.export_analysis_rom import load_analysis_rom
from generate_game import GAMES
from metadata_candidates import (indexed_call_tables, indirect_field_calls, materialized_entries,
                                 section_pages, shared_entry_slots, visible_shared_slots)
from sdk_signatures import matching_sdk_bodies, sdk_address_references, sdk_jal_references, signature_index
from symbol_database import load_database

JR_RA = bytes.fromhex('03e00008')
NOP = bytes(4)
NO_TERMINAL = 'no return, tail transfer or trap was established'
REVIEW_EVIDENCE = {'after-return', 'sdk-body', 'probed-call-argument', 'sdk-direct-call', 'repeated-direct-call'}


def probe_body(rom, section, address, owners, entries, transfers, limit, shared_entries=(),
               sections=(), boundaries=(), *, visited=None):
    """A bounded control-flow check, not proof of function ownership or ABI."""
    pending, errors, calls = deque([address]), [], []
    if visited is None:
        visited = set()
    expanded = set()
    switches = []
    branch_entries = set()
    terminals, indirect_calls, returns = 0, 0, 0
    sid, low, high = section['id'], section['vram'], section['vram']+section['size']

    def read(pc):
        if pc%4 or not low <= pc < high:
            return None
        offset = section['rom_offset']+pc-low
        return decode(rom[offset:offset+4], pc)[0]

    def error(pc, reason):
        errors.append(dict(pc=pc, reason=reason))

    def local(destination, pc):
        nonlocal terminals
        branch_entries.add(destination)
        if (sid, destination) in entries and destination != address:
            terminals += 1
            calls.append(dict(pc=pc,vram=destination))
        elif destination < address:
            error(pc, 'branch below candidate entry needs ownership review')
        else:
            pending.append(destination)

    while pending and len(errors) < 8:
        pc = pending.popleft()
        if pc in expanded:
            continue
        if pc not in visited and len(visited) >= limit:
            error(pc, 'bounded probe stopped before all paths were checked')
            break
        expanded.add(pc)
        if (sid, pc) in owners and not (pc == address and (sid,pc) in shared_entries):
            error(pc, 'overlaps an already owned instruction')
            continue
        i = read(pc)
        if i is None:
            error(pc, 'control flow leaves the candidate mapping')
            continue
        if i.opcode not in SUPPORTED:
            error(pc, 'invalid or unsupported instruction: '+i.opcode)
            continue
        visited.add(pc)
        if i.opcode in ('break','syscall'):
            terminals += 1
            continue
        if not i.delay:
            pending.append(pc+4)
            continue
        slot = read(pc+4)
        if (slot is None or slot.delay or slot.opcode not in SUPPORTED
                or (sid,pc+4) in owners):
            error(pc, 'missing, unsupported or already owned delay slot')
            continue
        if pc+4 not in visited and len(visited) >= limit:
            error(pc, 'bounded probe stopped before its delay slot was checked')
            break
        visited.add(pc+4)
        if i.opcode == 'jr' and i.rs == 31:
            terminals += 1
            returns += 1
        elif i.opcode in ('jr','jalr'):
            checked = transfers.get((sid,pc))
            if i.opcode == 'jalr':
                indirect_calls += 1
                pending.append(pc+8)
            elif checked is None:
                switch = infer_local_switch(rom, sections, section, read, i, address, expanded, visited,
                                            entries, boundaries)
                if switch is None:
                    error(pc, 'computed jump needs checked targets or return metadata')
                else:
                    switches.append(switch)
                    for destination in switch['targets']:
                        local(destination, pc)
            elif checked['kind'] == 'return':
                terminals += 1
                returns += 1
            elif checked['kind'] == 'jump':
                for destination in checked['targets']:
                    local(destination,pc)
            else:
                calls.extend(dict(pc=pc,vram=a) for a in checked['targets'])
                terminals += 1
        elif i.opcode in ('jal','bltzal','bgezal','bltzall','bgezall'):
            if i.opcode == 'jal' or constant_condition(i) is not False:
                calls.append(dict(pc=pc,vram=target(i)))
            pending.append(pc+8)
        elif i.opcode == 'j':
            local(target(i),pc)
        else:
            outcome = constant_condition(i)
            if outcome is not False:
                local(target(i),pc)
            if outcome is not True:
                pending.append(pc+8)
    for switch in switches:
        if any(pc in branch_entries for pc in range(switch['guard_pc'] + 4, switch['pc'] + 4, 4)):
            error(switch['pc'], 'alternate entry into inferred switch requires checked targets')
    if not errors and not terminals:
        error(address, NO_TERMINAL)
    first = []
    for n in range(min(8,(high-address)//4)):
        i = read(address+n*4)
        if i.delay:
            break
        first.append(i)
    prologue = any(i.opcode in ('addiu','addi') and i.rs==29 and i.rt==29
                   and i.immediate<0 and i.immediate%8==0 for i in first)
    if not errors and not returns and not calls:
        error(address,'only a trap or closed loop was found; code ownership needs review')
    return dict(status='review' if errors else 'closed', instructions=len(visited),
                stack_prologue=prologue, indirect_calls=indirect_calls,
                direct_calls=calls, issues=errors, **({'local_switches': switches} if switches else {}))


def sdk_entry_matches(rom, offset, end, pc, prefixes, expected_symbol):
    """Return one supported body extent and its possible catalogue names."""
    matches = set()
    for symbol, sig in matching_sdk_bodies(rom, offset, end, prefixes):
        size = sig['body_size']
        if expected_symbol is not None:
            if symbol != expected_symbol:
                continue
        elif size < 20:
            # Short generic getters and empty stubs also match interior blocks.
            # Fixed control-register routines have a distinctive extra operand.
            if size < 12 or sig['references']:
                continue
            if not any(i.opcode in ('mfc0', 'mtc0', 'cfc1', 'ctc1')
                       for i in decode(rom[offset:offset + size], pc)):
                continue
        matches.add((size, symbol))
    sizes = {size for size, _ in matches}
    if len(sizes) == 1:
        return sizes.pop(), sorted(symbol for _, symbol in matches)


def sdk_gap_call_sites(rom, section, start, end):
    """Find multiply called entries immediately after an unconditional jump in the same gap."""
    base = section['vram'] - section['rom_offset']
    sites = {}
    for index, (word,) in enumerate(struct.iter_unpack('>I', memoryview(rom)[start:end])):
        if word >> 26 != 3:
            continue
        offset = start + 4 * index
        pc = base + offset
        address = ((pc + 4) & 0xf0000000) | ((word & 0x03ffffff) << 2)
        dest = address - base
        if dest < offset + 16 or dest >= end:
            continue
        jump = int.from_bytes(rom[dest - 8:dest - 4], 'big')
        if jump >> 26 != 2:
            continue
        calls = sites.setdefault(address, [])
        if len(calls) >= 2:
            continue
        if offset > section['rom_offset'] and decode(rom[offset - 4:offset], pc - 4)[0].delay:
            continue
        slot = decode(rom[offset + 4:offset + 8], pc + 4)[0]
        if slot.opcode in SUPPORTED and not slot.delay:
            calls.append(pc)
    for address, calls in sites.items():
        if len(calls) < 2:
            continue
        offset = address - base - 8
        if decode(rom[offset - 4:offset], address - 12)[0].delay:
            continue
        jump, slot = decode(rom[offset:offset + 8], address - 8)
        if slot.opcode in SUPPORTED and not slot.delay:
            yield address, dict(pc=calls[0], call_pcs=calls, preceding_jump_pc=address - 8,
                                preceding_jump_target=target(jump), rom_offset=calls[0] - base)


def rejected_pointer_probe(item):
    body = item['body']
    return (body['status'] == 'review' and not body['stack_prologue']
            and all(ref['kind'] == 'image-pointer' for ref in item['references']))


def inspect_candidates(rom: bytes, metadata: dict, *, include_registered=False,
                       max_instructions=4096) -> dict:
    sections = sorted(metadata['sections'],key=lambda s:s['vram'])
    prefixes = {prefix for s in sections
                for prefix in range(s['vram']>>24,((s['vram']+s['size']-1)>>24)+1)}
    complete = bool((metadata.get('code_inventory') or {}).get('complete'))
    scan = not complete or include_registered
    entries = {(f['section'],f['vram']):f for f in metadata['functions']}
    owners = ({(f['section'],pc) for f in metadata['functions'] for pc in f['instruction_addresses']}
              if scan else set())
    owned_offsets = ({f['rom_offset']+pc-f['vram'] for f in metadata['functions']
                      for pc in f['instruction_addresses']} if scan else set())
    transfers = {(f['section'],t['pc']):t for f in metadata['functions']
                 for t in f.get('indirect_transfers',[])}
    boundaries = {(f['section'], t['pc']) for f in metadata['functions']
                  for field in ('fallthrough_transfers', 'tail_call_continuations')
                  for t in f.get(field, [])}
    shared_entries = shared_entry_slots(rom,metadata['functions'])
    records = {}
    table_uses = []
    field_uses = []
    table_candidates = []
    skipped_interior = 0

    pages = section_pages(sections)
    def mappings(address, size=16, preferred=None):
        matches = [s for s in pages.get(address >> 12, ()) if not address % 4
                   and s['vram'] <= address and address + size <= s['vram'] + s['size']]
        selected = [s for s in matches if s['id'] == preferred]
        return selected or matches

    def mapping(address, size=16, preferred=None):
        matches = mappings(address, size, preferred)
        return matches[0] if len(matches) == 1 else None

    def record(address, reference):
        nonlocal skipped_interior
        added = []
        for section in mappings(address, preferred=reference.get('section')):
            key = section['id'],address
            if key in entries and not include_registered:
                continue
            if key in owners and key not in entries and key not in shared_entries:
                skipped_interior += 1
                continue
            offset = section['rom_offset']+address-section['vram']
            if key not in records:
                added.append(key)
            item = records.setdefault(key,dict(vram=address,section=section['id'],
                rom_offset=offset,sha256=hashlib.sha256(rom[offset:offset+16]).hexdigest(),
                registered=key in entries,reference_count=0,references=[]))
            item['reference_count'] += 1
            # Preserve counts but bound reports for frequently repeated script operands.
            if len(item['references'])<64 and reference not in item['references']:
                item['references'].append(reference)
            elif ((reference.get('kind') == 'indexed-table-call' or reference.get('kind') in REVIEW_EVIDENCE)
                  and reference not in item['references']):
                # Raw asset pointers must not crowd out the active consumer.
                replace = next((n for n, r in enumerate(item['references'])
                                if r['kind'] == 'image-pointer'), None)
                if replace is not None:
                    item['references'][replace] = reference
        return added

    # Local switch labels must not become function candidates just because their
    # table contains code-looking pointers. Exclude checked jump table words.
    excluded_offsets = set()
    for (sid, _), transfer in transfers.items():
        if transfer['kind']!='jump' or 'table_vram' not in transfer:
            continue
        s = mapping(transfer['table_vram'],transfer['table_count']*4,sid)
        if s:
            offset = s['rom_offset']+transfer['table_vram']-s['vram']
            excluded_offsets.update(range(offset,offset+transfer['table_count']*4,4))
    if scan:
        overlapping_sections = {
            section['id'] for section in sections
            if any(section['id'] != other['id']
                   and section['vram'] < other['vram'] + other['size']
                   and other['vram'] < section['vram'] + section['size']
                   for other in sections)
        }
        shared_by_section = visible_shared_slots(sections, overlapping_sections, shared_entries)
        unambiguous_sections = [dict(section, section=section['id']) for section in sections
                                if section['id'] not in overlapping_sections]
        text_by_section = {
            section['id']: (unambiguous_sections if section['id'] not in overlapping_sections
                            else [dict(other, section=other['id']) for other in sections
                                  if other['id'] == section['id'] or other['id'] not in overlapping_sections])
            for section in sections
        }
        for index,(value,) in enumerate(struct.iter_unpack('>I',rom)):
            if value>>24 not in prefixes or value%4:
                continue
            offset = index*4
            if offset in owned_offsets or offset in excluded_offsets:
                continue
            record(value,dict(kind='image-pointer',rom_offset=offset))
        for gap in metadata.get('unvisited_ranges',[]):
            section = next(s for s in sections if s['id']==gap['section'])
            offset = section['rom_offset']+gap['vram']-section['vram']
            for n,(word,) in enumerate(struct.iter_unpack('>I',rom[offset:offset+gap['size']])):
                # ADDIU SP,SP,-frame, corroborated by a nearby saved RA or S reg.
                frame = word&65535
                if word>>16!=0x27bd or not frame&0x8000 or frame%8:
                    continue
                pc = gap['vram']+n*4
                near = rom[offset+n*4+4:offset+min(gap['size'],n*4+32)]
                if any(w>>26==43 and (w>>21)&31==29 and ((w>>16)&31 in (*range(16,24),31))
                       for (w,) in struct.iter_unpack('>I',near)):
                    record(pc,dict(kind='stack-prologue',rom_offset=offset+n*4,section=gap['section']))
        for function in metadata['functions']:
            # Replaced original bodies are not active callback producers.
            if function.get('origin') in ('native-replacement','unsupported-boundary'):
                continue
            table_uses.extend(dict(use, section=function['section'], function=function['name'])
                              for use in indexed_call_tables(rom, function))
            field_uses.extend(dict(use,section=function['section'],function=function['name'])
                              for use in indirect_field_calls(rom,function))
            if include_registered:
                for pc in function['instruction_addresses']:
                    offset = function['rom_offset']+pc-function['vram']
                    word = int.from_bytes(rom[offset:offset+4],'big')
                    if word>>26==3:
                        address = ((pc+4)&0xf0000000)|((word&0x3ffffff)<<2)
                        record(address,dict(kind='direct-call',pc=pc,expected_word=word,
                            rom_offset=offset,section=function['section'],function=function['name']))
            candidates = materialized_entries(rom,{'functions':[]},text_by_section[function['section']],
                                               source_function=function,
                                               shared_entries=shared_by_section[function['section']])
            for candidate in candidates:
                for use in candidate['address_uses']:
                    record(candidate['vram'],dict(use,section=function['section'],
                        function=function['name'],rom_offset=function['rom_offset']+use['pc']-function['vram']))
        # Bound the diagnostic scan, not the runtime selector. Adjacent pointer
        # runs can be separate tables; even a clean run needs manual review.
        resolved_tables = []
        for use in table_uses:
            section = mapping(use['table_vram'], 4, use['section'])
            if section is not None:
                resolved_tables.append((use, section))
        bases = {(s['id'], use['table_vram']) for use, s in resolved_tables}
        for use, section in resolved_tables:
            base = use['table_vram']
            words, targets = 0, set()
            stop = 'scan-limit'
            for index in range(256):
                address = base + index * use['stride']
                offset = section['rom_offset'] + address - section['vram']
                if address + 4 > section['vram'] + section['size']:
                    stop = 'mapping-end'
                    break
                if index and (section['id'], address) in bases:
                    stop = 'next-dispatch-base'
                    break
                if offset in excluded_offsets or offset in owned_offsets:
                    stop = 'checked-switch-or-owned-code'
                    break
                value = int.from_bytes(rom[offset:offset+4], 'big')
                if value and mapping(value, preferred=use['section']) is None:
                    stop = 'non-pointer-word'
                    break
                words += 1
                if not value:
                    continue
                targets.add(value)
                record(value, dict(use, kind='indexed-table-call', table_section=section['id'],
                                   table_index=index, rom_offset=offset))
            if targets:
                table_candidates.append(dict(use, table_section=section['id'], scanned_words=words,
                    stop_reason=stop, bounds_verified=False, target_count=len(targets)))
    by_id = {s['id']: s for s in sections}

    def probe(sid, address):
        visited = set()
        body = probe_body(rom, by_id[sid], address, owners, entries, transfers, max_instructions,
                          shared_entries, sections, boundaries, visited=visited)
        for call in body['direct_calls']:
            if mapping(call['vram'], 4, sid) is None:
                body['issues'].append(dict(pc=call['pc'], reason='direct call target has no unique CPU mapping'))
                body['status'] = 'review'
        return body, visited

    # A return can be an early exit. Only follow a reachable JR RA whose
    # delay slot is the highest instruction reached by the whole body.
    successors = deque()

    def next_after_return(sid, return_pc):
        section = by_id[sid]
        end = pc = return_pc + 8
        offset = section['rom_offset'] + pc - section['vram']
        high = section['vram'] + section['size']
        # Between object files, up to three NOPs can align the next entry.
        # Keep the entire 16-byte candidate fingerprint in this overlay.
        while pc + 16 <= high and rom[offset:offset + 4] == NOP:
            if pc % 16 == 0 or (sid, pc) in owners:
                return
            pc += 4
            offset += 4
        if pc + 16 > high or (pc != end and pc % 16):
            return
        return pc

    def queue_successor(sid, start, pcs):
        if not pcs:
            return
        return_pc = max(pcs) - 4
        section = by_id[sid]
        offset = section['rom_offset'] + return_pc - section['vram']
        if return_pc < start or rom[offset:offset + 4] != JR_RA or return_pc not in pcs:
            return
        pc = next_after_return(sid, return_pc)
        if pc is None:
            return
        offset = section['rom_offset'] + pc - section['vram']
        # A compiler can leave an unreachable JR RA / NOP after the epilogue.
        # Distinguishing that from an empty function needs another reference.
        if rom[offset:offset + 8] != JR_RA + NOP:
            successors.append((sid, pc, start, return_pc))

    if not complete:
        for f in metadata['functions']:
            if not f.get('discovery_errors') and f.get('origin') not in ('native-replacement', 'unsupported-boundary'):
                queue_successor(f['section'], f['vram'], f['instruction_addresses'])
    # Remember which probe reached each word. Multiple probes reaching the
    # same word make its speculative owner ambiguous.
    interiors = {sid: {} for sid in by_id}

    def remember_probe(sid, start, visited):
        reached = interiors[sid]
        for pc in visited:
            reached[pc] = None if pc in reached else start

    def scheduled_frame(sid, pc, overlap):
        if len(overlap) != 1:
            return None
        frame = next(iter(overlap))
        if frame is None or not pc < frame <= pc + 28:
            return None
        refs = records[sid, frame]['references']
        if {ref['kind'] for ref in refs} != {'stack-prologue'}:
            return None
        section = by_id[sid]
        offset = section['rom_offset'] + pc - section['vram']
        # The earlier entry must fall straight through into the exact frame
        # instruction that seeded the competing probe.
        prefix = decode(rom[offset:offset + frame - pc], pc)
        if any(i.delay or i.opcode in ('syscall', 'break') for i in prefix):
            return None
        return frame

    probed_callers = []
    thread_calls = []
    pending = deque(records)
    checked = set()
    while pending:
        key = pending.popleft()
        if key in checked:
            continue
        checked.add(key)
        item = records[key]
        if item['registered']:
            item['body'] = dict(status='registered',instructions=len(entries[key]['instruction_addresses']))
            item['confidence'] = 'registered'
            continue
        body, visited = probe(*key)
        item['body'] = body
        kinds = {r['kind'] for r in item['references']}
        strong = bool(kinds & {'constant-indirect-call','call-argument','stack-argument','stored-address','candidate-direct-call',
                              'indexed-table-call'})
        item['confidence'] = ('likely' if body['status']=='closed' and (strong or
                              body['stack_prologue'] and kinds!={'stack-prologue'})
                              else 'review')
        if not complete and body['status'] == 'closed' and item['confidence'] == 'likely':
            probed_callers.append(dict(section=key[0], vram=key[1], rom_offset=item['rom_offset'],
                                       instruction_addresses=sorted(visited), size=max(visited) + 4 - key[1],
                                       indirect_transfers=body.get('local_switches', [])))
        if (not complete and 'stack-prologue' in kinds and len(body['issues']) == 1 and
                body['issues'][0]['reason'] == NO_TERMINAL):
            # A service thread may loop forever. Its fully explored graph can
            # still corroborate a callee independently reached by two JALs.
            sites = {}
            for call in body['direct_calls']:
                pc = call['pc']
                offset = item['rom_offset'] + pc - key[1]
                if int.from_bytes(rom[offset:offset + 4], 'big') >> 26 == 3:
                    sites.setdefault(call['vram'], set()).add(pc)
            for dest, pcs in sites.items():
                if len(pcs) >= 2:
                    thread_calls.append((dest, dict(kind='repeated-direct-call', section=key[0],
                                                    function_vram=key[1], pc=min(pcs), call_pcs=sorted(pcs))))
        # A candidate's direct calls can expose another layer of missing code.
        # Propagate only from bodies whose bounded flow check closed cleanly.
        if body['status']=='closed':
            if not complete:
                remember_probe(*key, visited)
                queue_successor(*key, visited)
            for call in body['direct_calls']:
                pending.extend(record(call['vram'],dict(kind='candidate-direct-call',pc=call['pc'],
                    section=item['section'],candidate=item['vram'],
                    rom_offset=item['rom_offset']+call['pc']-item['vram'])))
    # Adjacency is a review hint. Keep it out of call-tree propagation and do
    # not upgrade earlier candidates. A closed neighbour can anchor the next
    # leaf even when a whole run has no incoming references or stack frames.
    while successors:
        sid, pc, previous, return_pc = successors.popleft()
        key = sid, pc
        if key in checked or key in entries or key in owners or pc in interiors[sid]:
            continue
        checked.add(key)
        body, visited = probe(sid, pc)
        if body['status'] != 'closed':
            continue
        reached = interiors[sid]
        overlap = {reached[p] for p in visited if p in reached}
        frame = scheduled_frame(sid, pc, overlap)
        if overlap and frame is None:
            continue
        section = by_id[sid]
        reference = dict(kind='after-return', section=sid, function_vram=previous, pc=return_pc,
                         rom_offset=section['rom_offset'] + return_pc - section['vram'])
        if frame is not None:
            reference['stack_prologue_vram'] = frame
        record(pc, reference)
        records[key].update(body=body, confidence='review')
        remember_probe(sid, pc, visited)
        queue_successor(sid, pc, visited)

    def caller_mapping(address, sid, size=16):
        dest = mapping(address, size, preferred=sid)
        group = by_id[sid].get('overlay_group')
        if dest and dest['id'] != sid and group and dest.get('overlay_group') == group:
            return None
        return dest

    fallback_calls = []

    def sdk_sites(prefixes):
        # Native replacements hide the original guest call graph. Only use a
        # JAL at a named relocation in a full-size match of the caller.
        for f in metadata['functions']:
            if f.get('origin') != 'native-replacement':
                continue
            for call_pc, word, name in sdk_jal_references(rom, f, prefixes):
                address = ((call_pc + 4) & 0xf0000000) | ((word & 0x03ffffff) << 2)
                dest = mapping(address, preferred=f['section'])
                if dest:
                    fallback_calls.append((address, dict(kind='sdk-direct-call', section=f['section'],
                        function_vram=f['vram'], pc=call_pc, expected_word=word, relocation=name)))
                    yield dest, address, dest['rom_offset'] + dest['size'], dict(
                        caller_section=f['section'], function_vram=f['vram'], pc=call_pc,
                        rom_offset=f['rom_offset'] + call_pc - f['vram'], expected_word=word, expected_symbol=name)

        # The same relocation name must describe both halves of the loaded
        # pointer and a full signature at the resulting address.
        replacements = (f for f in metadata['functions'] if f.get('origin') == 'native-replacement')
        for f in (*replacements, *probed_callers):
            for pc, hi_pc, address, name in sdk_address_references(rom, f, prefixes):
                dest = caller_mapping(address, f['section'])
                if dest:
                    yield dest, address, dest['rom_offset'] + dest['size'], dict(
                        caller_section=f['section'], function_vram=f['vram'], pc=pc, hi_pc=hi_pc,
                        rom_offset=f['rom_offset'] + pc - f['vram'], expected_symbol=name)

        # A return and its delay slot can precede a gap by up to three NOPs.
        for gap in metadata.get('unvisited_ranges', ()):
            section = by_id[gap['section']]
            base = section['rom_offset']
            start = base + gap['vram'] - section['vram']
            end = start + gap['size']
            cursor = max(base, start - 20)
            while (offset := rom.find(JR_RA, cursor, end)) >= 0:
                cursor = offset + 4
                return_pc = section['vram'] + offset - base
                if return_pc % 4:
                    continue
                pc = next_after_return(section['id'], return_pc)
                if pc is not None and gap['vram'] <= pc < gap['vram'] + gap['size']:
                    yield section, pc, end, dict(pc=return_pc, rom_offset=offset)

            for pc, site in sdk_gap_call_sites(rom, section, start, end):
                if caller_mapping(site['preceding_jump_target'], section['id'], 4) is not None:
                    yield section, pc, end, site

    if not complete and metadata.get('unvisited_ranges'):
        prefixes, _ = signature_index(load_database(), set())
        for section, pc, end, site in sdk_sites(prefixes):
            sid = section['id']
            key = sid, pc
            if key in entries or key in owners:
                continue
            existing = records.get(key)
            # Preserve existing report rows. A failed raw-pointer-only probe
            # would otherwise be discarded below, so a signature can rescue it.
            if existing and not rejected_pointer_probe(existing):
                continue
            offset = section['rom_offset'] + pc - section['vram']
            matches = sdk_entry_matches(rom, offset, end, pc, prefixes, site.get('expected_symbol'))
            if not matches:
                continue
            size, symbols = matches
            if 'preceding_jump_target' in site and pc <= site['preceding_jump_target'] < pc + size:
                continue
            if any((sid, p) in owners for p in range(pc, pc + size, 4)):
                continue
            record(pc, dict(site, kind='sdk-body', section=sid, body_size=size, sdk_symbols=symbols,
                            body_sha256=hashlib.sha256(rom[offset:offset + size]).hexdigest()))
            if not existing:
                body, visited = probe(sid, pc)
                if body['status'] == 'closed' and max(visited) >= pc + size:
                    body['status'] = 'review'
                    body['issues'].append(dict(pc=pc, reason='control flow extends beyond the matched SDK body'))
                records[key].update(body=body, confidence='review')

    def add_at_return(address, ref):
        sid = ref['section']
        section = caller_mapping(address, sid)
        if section is None:
            return
        key = section['id'], address
        item = records.get(key)
        if key in entries or key in owners or key in switch_labels or (item and not rejected_pointer_probe(item)):
            return
        sid = section['id']
        for distance in (8, 12, 16, 20):
            pc = address - distance
            if pc < section['vram']:
                break
            offset = section['rom_offset'] + pc - section['vram']
            if rom[offset:offset + 4] != JR_RA or next_after_return(sid, pc) != address:
                continue
            slot = decode(rom[offset + 4:offset + 8], pc + 4)[0]
            if slot.delay or slot.opcode not in SUPPORTED:
                continue
            body, _ = probe(sid, address)
            if body['status'] == 'closed':
                record(address, dict(ref, section=sid, caller_section=ref['section'], return_pc=pc))
                records[sid, address].update(body=body, confidence='review')
            break

    if not complete and metadata.get('unvisited_ranges'):
        switch_labels = {(sid, pc) for (sid, _), t in transfers.items() if t['kind'] == 'jump'
                         for pc in t['targets']}
        switch_labels.update((item['section'], pc) for item in records.values()
                             for t in item['body'].get('local_switches', []) for pc in t['targets'])
        for address, ref in (*thread_calls, *fallback_calls):
            add_at_return(address, ref)
        callable_entries = entries.keys() | {(f['section'], f['vram']) for f in probed_callers
                                            if any(r['kind'] == 'candidate-direct-call'
                                                   for r in records[f['section'], f['vram']]['references'])}
        for f in probed_callers:
            sid = f['section']
            candidates = materialized_entries(rom, {'functions': []}, text_by_section[sid],
                                              source_function=f, shared_entries=shared_by_section[sid])
            for candidate in candidates:
                for use in candidate['address_uses']:
                    if use['kind'] not in ('call-argument', 'stack-argument') or 'callee' not in use:
                        continue
                    callee = caller_mapping(use['callee'], sid, 4)
                    if not callee:
                        continue
                    callee_key = callee['id'], use['callee']
                    if callee_key in callable_entries:
                        add_at_return(candidate['vram'], dict(use, kind='probed-call-argument',
                                      section=sid, function_vram=f['vram']))
    order = {'likely':0,'review':1,'registered':2}
    # Random asset words often resemble pointers. Retain only closed bodies,
    # stack-frame candidates, or addresses actually constructed by owned code.
    # Rejected pointer-only hits are counted without flooding the review list.
    rejected = Counter()
    candidates = []
    for item in records.values():
        body = item['body']
        if rejected_pointer_probe(item):
            rejected[body['issues'][0]['reason']] += 1
        else:
            candidates.append(item)
    candidates.sort(key=lambda c:(order[c['confidence']],
        not any(r['kind']=='indexed-table-call' for r in c['references']),c['section'],c['vram']))
    table_likely = Counter()
    for candidate in candidates:
        if candidate['confidence'] == 'likely':
            table_likely.update({(r['section'], r['pc'], r['table_vram'])
                                 for r in candidate['references'] if r['kind']=='indexed-table-call'})
    for table in table_candidates:
        table['likely_missing_targets'] = table_likely[table['section'], table['pc'], table['table_vram']]
    table_candidates.sort(key=lambda t: (-t['likely_missing_targets'], t['section'], t['pc']))
    return dict(format_version=1,game=metadata['game'],rom_sha1=metadata['rom_sha1'],
        candidates_only=True,complete_code_inventory=complete,
        summary=dict(Counter(c['confidence'] for c in candidates)),
        rejected_pointer_candidates=dict(rejected),
        excluded_owned_interiors=skipped_interior,
        warning='Heuristics do not prove callback ABI, table bounds, code ownership or playability. Review sources and bodies before adding checked entries.',
        indirect_tables=table_candidates, indirect_fields=field_uses, candidates=candidates)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--metadata',required=True,type=Path)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--game',choices=GAMES)
    source.add_argument('--rom',type=Path,help='exact normalized/prepared analysis image')
    parser.add_argument('--include-registered',action='store_true',help='also show evidence for existing entries')
    parser.add_argument('--max-instructions',type=int,default=4096,help='bounded body probe, 16..32768 words')
    parser.add_argument('--output',required=True,type=Path,help='private JSON report, normally under build/')
    args = parser.parse_args()
    if not 16<=args.max_instructions<=32768:
        raise RecompError('--max-instructions must be 16..32768')
    if args.rom:
        rom = args.rom.read_bytes()
        metadata = load_metadata(args.metadata,rom)
    else:
        rom,metadata = load_analysis_rom(args.game,args.metadata)
    report = inspect_candidates(rom,metadata,include_registered=args.include_registered,
                                max_instructions=args.max_instructions)
    write_json(args.output,report)
    print(f"{metadata['game']}: {report['summary']}; {args.output}")
    for table in [t for t in report['indirect_tables'] if t['likely_missing_targets']][:10]:
        print(f"  Table {table['table_vram']:#010x} -> call {table['pc']:#010x}: "
              f"{table['likely_missing_targets']} likely missing targets; "
              f"scanned {table['scanned_words']} words, {table['stop_reason']} (bounds unverified)")
    for candidate in [c for c in report['candidates'] if c['confidence']=='likely'][:30]:
        tables = [r for r in candidate['references'] if r['kind']=='indexed-table-call']
        evidence = (f"; table {tables[0]['table_vram']:#010x}[{tables[0]['table_index']}]"
                    f" -> call {tables[0]['pc']:#010x} (bounds unverified)" if tables else '')
        print(f"  {candidate['vram']:#010x} s{candidate['section']}: "
              f"{candidate['body']['instructions']} instructions, {candidate['reference_count']} references{evidence}")
    print('Candidates only; no profiles, inventories or runtime registrations were changed.')


if __name__=='__main__':
    try:
        main()
    except (RecompError,OSError,ValueError,KeyError,TypeError,ImportError) as error:
        print(f'error: {error}',file=sys.stderr)
        sys.exit(1)
