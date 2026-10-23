"""Conservative callback-entry candidates from known data and CPU text ranges.

Candidates are evidence for discovery, not a complete code classification.
Known local switch tables and already-owned instructions are excluded.
"""
from bisect import bisect_right
from collections import deque
import hashlib
import struct

from emit import decode, decode_function, SUPPORTED, target


def owned_ranges(function: dict) -> list[dict]:
    """Contiguous owned runs, excluding gaps inside a function's span."""
    runs = []
    for pc in function['instruction_addresses']:
        if runs and runs[-1]['vram']+runs[-1]['size']==pc:
            runs[-1]['size'] += 4
        else:
            runs.append(dict(vram=pc,size=4,rom_offset=function['rom_offset']+pc-function['vram']))
    return runs


def indirect_parameter_uses(rom: bytes, function: dict) -> dict[int,list[dict]]:
    """Trace entry arguments through register copies to JALR along owned CFG.

    Masks describe possible argument origins, not constant runtime values. Loads
    and arithmetic kill origins; calls kill volatile registers after their delay
    slots. No mutable memory or guessed table bounds enter this analysis.
    """
    instructions = {i.pc:i for i in decode_function(rom,function)}
    transfers = {t['pc']:t for t in function.get('indirect_transfers',[])}
    initial = tuple(1<<(r-4) if 4<=r<8 else 0 for r in range(32))
    states, pending, uses = {function['vram']:initial}, deque([function['vram']]), {}

    def advance(i,state):
        result = list(state)
        for r in range(1,32):
            if i.gpr_write_mask&(1<<r):
                result[r] = 0
        source, destination = None, i.rd
        if i.opcode in ('addu','daddu','or','xor'):
            if i.rs==0:
                source = i.rt
            elif i.rt==0:
                source = i.rs
        elif i.opcode in ('addiu','daddiu','ori') and i.immediate==0:
            source, destination = i.rs,i.rt
        elif i.opcode in ('sll','srl') and i.shift==0:
            source = i.rt
        if source is not None and destination:
            result[destination] = state[source]
        result[0] = 0
        return tuple(result)

    def enqueue(pc,state):
        if pc not in instructions:
            return
        old = states.get(pc)
        merged = tuple(a|b for a,b in zip(old,state)) if old is not None else state
        if merged != old:
            states[pc] = merged
            pending.append(pc)

    while pending:
        pc = pending.popleft()
        i, state = instructions[pc],states[pc]
        if i.opcode not in SUPPORTED or i.opcode in ('break','syscall'):
            continue
        if i.opcode=='jalr' and i.rd==31:
            for r in range(4,8):
                if state[i.rs]&(1<<(r-4)):
                    uses.setdefault(r,{})[pc] = dict(pc=pc,expected_word=i.word)
        after = advance(i,state)
        if not i.delay:
            enqueue(pc+4,after)
            continue
        slot = instructions.get(pc+4)
        if slot is None or slot.delay or slot.opcode not in SUPPORTED:
            continue
        delayed = advance(slot,after)
        if i.opcode in ('jal','jalr','bltzal','bgezal','bltzall','bgezall'):
            delayed = tuple(v if r in (*range(16,24),28,29,30) else 0
                            for r,v in enumerate(delayed))
            enqueue(pc+8,delayed)
        elif i.opcode=='jr':
            checked = transfers.get(pc)
            if checked and checked['kind']=='jump':
                for address in checked['targets']:
                    enqueue(address,delayed)
        elif i.opcode=='j':
            enqueue(target(i),delayed)
        else:
            always = i.opcode=='b' or i.opcode in ('beq','beql') and i.rs==i.rt
            never = i.opcode in ('bne','bnel') and i.rs==i.rt
            if not never:
                enqueue(target(i),delayed)
            if not always:
                enqueue(pc+8,after if i.likely else delayed)
    return {r:[sites[pc] for pc in sorted(sites)] for r,sites in sorted(uses.items())}


def indexed_call_tables(rom: bytes, function: dict) -> list[dict]:
    """Find owned LW -> JALR flows through constant-base, word-indexed arrays.

    This proves an address expression, not table bounds or immutable contents.
    At joins keep only agreeing expressions; calls kill volatile registers.
    Collect uses after convergence so an optimistic early path cannot survive
    a conflicting predecessor. Delay slots cannot change an already read target.
    """
    instructions = {i.pc: i for i in decode_function(rom, function)}
    if not any(i.opcode == 'jalr' for i in instructions.values()):
        return []
    transfers = {t['pc']: t for t in function.get('indirect_transfers', [])}
    # Expressions: constant(value), indexed(base), loaded(base, load_pc).
    # indexed means base + 4 * an unknown integer; no index bound is implied.
    initial = (('constant', 0),) + (None,) * 31
    states, pending = {function['vram']: initial}, deque([function['vram']])

    def add(left, right):
        if left is None or right is None:
            return None
        if left[0] == 'constant' and right[0] == 'constant':
            return ('constant', (left[1] + right[1]) & 0xffffffff)
        if left[0] == 'constant' and right[0] == 'indexed':
            return ('indexed', (left[1] + right[1]) & 0xffffffff)
        if left[0] == 'indexed' and right[0] == 'constant':
            return add(right, left)
        return None

    def advance(i, state):
        result = list(state)
        for r in range(1, 32):
            if i.gpr_write_mask & (1 << r):
                result[r] = None
        left, right = state[i.rs], state[i.rt]
        value, destination = None, i.rt
        if i.opcode == 'lui':
            value = ('constant', (i.word & 65535) << 16)
        elif i.opcode in ('addiu', 'addi'):
            value = add(left, ('constant', i.immediate))
        elif i.opcode == 'ori' and left and left[0] == 'constant':
            value = ('constant', left[1] | (i.word & 65535))
        elif i.opcode in ('addu', 'or'):
            destination = i.rd
            if i.rs == 0:
                value = right
            elif i.rt == 0:
                value = left
            elif i.opcode == 'addu':
                value = add(left, right)
        elif i.opcode == 'sll':
            destination = i.rd
            if right and right[0] == 'constant':
                value = ('constant', (right[1] << i.shift) & 0xffffffff)
            elif i.shift == 2:
                value = ('indexed', 0)
            elif i.shift == 0:
                value = right
        elif i.opcode == 'lw' and left and left[0] == 'indexed':
            base = (left[1] + i.immediate) & 0xffffffff
            if base % 4 == 0:
                value = ('loaded', base, i.pc)
        if destination and value is not None:
            result[destination] = value
        result[0] = ('constant', 0)
        return tuple(result)

    def enqueue(pc, state):
        if pc not in instructions:
            return
        old = states.get(pc)
        merged = tuple(a if a == b else None for a, b in zip(old, state)) if old is not None else state
        if merged != old:
            states[pc] = merged
            pending.append(pc)

    while pending:
        pc = pending.popleft()
        i, state = instructions[pc], states[pc]
        if i.opcode not in SUPPORTED or i.opcode in ('break', 'syscall'):
            continue
        after = advance(i, state)
        if not i.delay:
            enqueue(pc + 4, after)
            continue
        slot = instructions.get(pc + 4)
        if slot is None or slot.delay or slot.opcode not in SUPPORTED:
            continue
        delayed = advance(slot, after)
        if i.opcode in ('jal', 'jalr', 'bltzal', 'bgezal', 'bltzall', 'bgezall'):
            enqueue(pc + 8, tuple(v if r in (0, *range(16, 24), 28, 29, 30) else None
                                  for r, v in enumerate(delayed)))
        elif i.opcode == 'jr':
            checked = transfers.get(pc)
            if checked and checked['kind'] == 'jump':
                for address in checked['targets']:
                    enqueue(address, delayed)
        elif i.opcode == 'j':
            enqueue(target(i), delayed)
        else:
            always = i.opcode == 'b' or i.opcode in ('beq', 'beql') and i.rs == i.rt
            never = i.opcode in ('bne', 'bnel') and i.rs == i.rt
            if not never:
                enqueue(target(i), delayed)
            if not always:
                enqueue(pc + 8, after if i.likely else delayed)
    uses = []
    for pc, state in sorted(states.items()):
        i = instructions[pc]
        value = state[i.rs]
        if i.opcode == 'jalr' and i.rd == 31 and value and value[0] == 'loaded':
            uses.append(dict(pc=pc, expected_word=i.word, table_vram=value[1], load_pc=value[2]))
    return uses


def pointer_entries(rom: bytes, metadata: dict, text_ranges: list[dict],
                    data_ranges: list[dict], jump_tables: list[dict]) -> list[dict]:
    text = sorted(text_ranges, key=lambda r: r['vram'])
    starts = [r['vram'] for r in text]
    owned = {pc for f in metadata['functions'] for pc in f['instruction_addresses']}
    excluded = {table['table_vram']+i*4 for table in jump_tables for i in range(table['count'])}
    found = {}
    for region in data_ranges:
        start, offset, size = (region[k] for k in ('vram', 'rom_offset', 'size'))
        for i in range(0, size-3, 4):
            site = start+i
            if site in excluded:
                continue
            address = struct.unpack_from('>I', rom, offset+i)[0]
            if address % 4 or address in owned:
                continue
            index = bisect_right(starts, address)-1
            if index < 0:
                continue
            target = text[index]
            if address+16 > target['vram']+target['size']:
                continue
            raw_offset = target['rom_offset']+address-target['vram']
            if decode(rom[raw_offset:raw_offset+4], address)[0].opcode not in SUPPORTED:
                continue
            found.setdefault(address, dict(vram=address, origin='checked-root',
                sha256=hashlib.sha256(rom[raw_offset:raw_offset+16]).hexdigest(),
                pointer_sources=[]))['pointer_sources'].append(site)
    return [found[address] for address in sorted(found)]


def materialized_entries(rom: bytes, metadata: dict, text_ranges: list[dict], *,
                         source_ranges: list[dict] | None = None,
                         owned_addresses: set[int] | None = None) -> list[dict]:
    """Track address arithmetic, register copies, stores and callback arguments.

    Optional source ranges let reports scan owned runs in other sections while
    keeping candidate targets restricted to the supplied text ranges. This is a
    conservative local scan: joins discard constants and calls discard volatile
    registers after their delay slot. It does not read mutable RAM as constants.
    """
    text = sorted(text_ranges, key=lambda r: r['vram'])
    starts = [r['vram'] for r in text]
    owned = (owned_addresses if owned_addresses is not None else
             {pc for f in metadata['functions'] for pc in f['instruction_addresses']})
    found = {}

    def record(address, pc, kind, register, **details):
        index = bisect_right(starts, address)-1
        if address % 4 or address in owned or index < 0:
            return
        region = text[index]
        if address+16 > region['vram']+region['size']:
            return
        raw = region['rom_offset']+address-region['vram']
        if decode(rom[raw:raw+4], address)[0].opcode not in SUPPORTED:
            return
        item = found.setdefault(address, dict(vram=address, origin='checked-root',
            sha256=hashlib.sha256(rom[raw:raw+16]).hexdigest(),
            materialized_sources=[], address_uses=[]))
        if pc not in item['materialized_sources']:
            item['materialized_sources'].append(pc)
        use = dict(pc=pc, kind=kind, register=register, **details)
        if use not in item['address_uses']:
            item['address_uses'].append(use)

    for region in source_ranges if source_ranges is not None else text:
        registers = {0: 0}
        pending = None
        offset = region['rom_offset']
        instructions = decode(rom[offset:offset+region['size']], region['vram'])
        joins = {target(i) for i in instructions if i.delay and i.opcode not in ('jr','jalr','jal')}
        for i in instructions:
            if i.opcode not in SUPPORTED:
                registers, pending = {0:0}, None
                continue
            if i.pc in joins:
                registers = {0: 0}
            previous, pending = pending, None
            left, right = registers.get(i.rs), registers.get(i.rt)
            value, destination = None, i.rt
            if i.opcode == 'lui':
                value = (i.word&65535)<<16
            elif i.opcode in ('addiu','addi') and left is not None:
                value = left+i.immediate
            elif i.opcode in ('ori','andi','xori') and left is not None:
                immediate = i.word&65535
                value = {'ori': left|immediate, 'andi': left&immediate,
                         'xori': left^immediate}[i.opcode]
            elif i.opcode in ('addu','or','and','xor','subu'):
                destination = i.rd
                if left is not None and right is not None:
                    value = {'addu': left+right, 'or': left|right, 'and': left&right,
                             'xor': left^right, 'subu': left-right}[i.opcode]
            elif i.opcode in ('sll','srl') and right is not None:
                destination = i.rd
                value = right<<i.shift if i.opcode=='sll' else right>>i.shift
            if i.opcode in ('sw','sd') and right is not None:
                details = {'destination': (left+i.immediate)&0xffffffff} if left is not None else {}
                record(right,i.pc,'stored-address',i.rt,**details)
            if i.opcode == 'jalr' and left is not None:
                record(left,i.pc,'constant-indirect-call',i.rs)
            for register in list(registers):
                if i.gpr_write_mask & (1<<register):
                    registers.pop(register)
            if value is not None and destination:
                value &= 0xffffffff
                registers[destination] = value
                record(value,i.pc,'materialized-address',destination)
            if i.delay:
                pending = i
            if previous is not None and previous.opcode in ('jal','jalr'):
                for register in range(4,8):
                    if register in registers:
                        details = {'callee': target(previous)} if previous.opcode=='jal' else {}
                        record(registers[register],previous.pc,'call-argument',register,**details)
            if previous is not None and previous.opcode not in ('jal','jalr'):
                registers = {0: 0}
            elif previous is not None:
                registers = {r:v for r,v in registers.items() if r in (0, *range(16,24),28,29,30)}
            registers[0] = 0
    return [found[address] for address in sorted(found)]
