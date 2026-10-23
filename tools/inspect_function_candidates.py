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
from emit import SUPPORTED, decode, target
from export_analysis_rom import load_analysis_rom
from generate_game import GAMES
from metadata_candidates import indexed_call_tables, materialized_entries, owned_ranges


def probe_body(rom, section, address, owners, entries, transfers, limit):
    """A bounded control-flow check, not proof of function ownership or ABI."""
    pending, visited, errors, calls = deque([address]), set(), [], []
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
        if (sid, destination) in entries and destination != address:
            terminals += 1
            calls.append(dict(pc=pc,vram=destination))
        elif destination < address:
            error(pc, 'branch below candidate entry needs ownership review')
        else:
            pending.append(destination)

    while pending and len(visited) < limit and len(errors) < 8:
        pc = pending.popleft()
        if pc in visited:
            continue
        if (sid, pc) in owners:
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
                error(pc, 'computed jump needs checked targets or return metadata')
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
            calls.append(dict(pc=pc,vram=target(i)))
            pending.append(pc+8)
        elif i.opcode == 'j':
            local(target(i),pc)
        else:
            always = i.opcode=='b' or i.opcode in ('beq','beql') and i.rs==i.rt
            never = i.opcode in ('bne','bnel') and i.rs==i.rt
            if not never:
                local(target(i),pc)
            if not always:
                pending.append(pc+8)
    if pending:
        error(address,'bounded probe stopped before all paths were checked')
    if not errors and not terminals:
        error(address,'no return, tail transfer or trap was established')
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
                direct_calls=calls, issues=errors)


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
    records = {}
    table_uses = []
    table_candidates = []
    skipped_interior = 0

    def mappings(address, size=16, preferred=None):
        matches = [s for s in sections if not address % 4
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
            if key in owners and key not in entries:
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
            elif reference.get('kind') == 'indexed-table-call' and reference not in item['references']:
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
            text = [dict(s,section=s['id']) for s in sections
                    if s['id'] == function['section'] or not any(
                        s['id'] != other['id'] and s['vram'] < other['vram'] + other['size']
                        and other['vram'] < s['vram'] + s['size'] for other in sections)]
            if include_registered:
                for pc in function['instruction_addresses']:
                    offset = function['rom_offset']+pc-function['vram']
                    word = int.from_bytes(rom[offset:offset+4],'big')
                    if word>>26==3:
                        address = ((pc+4)&0xf0000000)|((word&0x3ffffff)<<2)
                        record(address,dict(kind='direct-call',pc=pc,expected_word=word,
                            rom_offset=offset,section=function['section'],function=function['name']))
            candidates = materialized_entries(rom,{'functions':[]},text,
                                               source_ranges=owned_ranges(function))
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
                address = base + index * 4
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
        body = probe_body(rom,mapping(item['vram'],preferred=item['section']),item['vram'],owners,entries,transfers,max_instructions)
        for call in body['direct_calls']:
            if mapping(call['vram'],4,item['section']) is None:
                body['issues'].append(dict(pc=call['pc'],reason='direct call target has no unique CPU mapping'))
                body['status'] = 'review'
        item['body'] = body
        kinds = {r['kind'] for r in item['references']}
        strong = bool(kinds & {'constant-indirect-call','call-argument','stored-address','candidate-direct-call',
                              'indexed-table-call'})
        item['confidence'] = ('likely' if body['status']=='closed' and (strong or
                              body['stack_prologue'] and kinds!={'stack-prologue'})
                              else 'review')
        # A candidate's direct calls can expose another layer of missing code.
        # Propagate only from bodies whose bounded flow check closed cleanly.
        if body['status']=='closed':
            for call in body['direct_calls']:
                pending.extend(record(call['vram'],dict(kind='candidate-direct-call',pc=call['pc'],
                    section=item['section'],candidate=item['vram'],
                    rom_offset=item['rom_offset']+call['pc']-item['vram'])))
    order = {'likely':0,'review':1,'registered':2}
    # Random asset words often resemble pointers. Retain only closed bodies,
    # stack-frame candidates, or addresses actually constructed by owned code.
    # Rejected pointer-only hits are counted without flooding the review list.
    rejected = Counter()
    candidates = []
    for item in records.values():
        body = item['body']
        pointer_only = all(r['kind']=='image-pointer' for r in item['references'])
        if pointer_only and body['status']=='review' and not body['stack_prologue']:
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
        indirect_tables=table_candidates, candidates=candidates)


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
