"""Conservative callback-entry candidates from known data and CPU text ranges.

Candidates are evidence for discovery, not a complete code classification.
Known local switch tables and already-owned instructions are excluded.
"""
from bisect import bisect_right
from collections import deque
import hashlib
import struct

from emit import constant_condition, decode, decode_function, SUPPORTED, target

STORE_BYTES = {'sb':1,'sh':2,'sw':4,'sd':8,'swl':4,'swr':4,'sdl':8,'sdr':8,
               'swc1':4,'sdc1':8,'sc':4,'scd':8}


def section_pages(sections):
    """Index possible mappings by guest page, retaining overlapping overlays."""
    pages = {}
    for section in sections:
        start = section['vram']
        for page in range(start >> 12, ((start + section['size'] - 1) >> 12) + 1):
            pages.setdefault(page, []).append(section)
    return pages


def visible_shared_slots(sections, overlapping, slots):
    resident = {pc for sid, pc in slots if sid not in overlapping}
    local = {sid: set() for sid in overlapping}
    for sid, pc in slots:
        if sid in local:
            local[sid].add(pc)
    return {s['id']: resident | local[s['id']] if s['id'] in local else resident
            for s in sections}


def shared_entry_slots(rom: bytes, functions: list[dict]) -> set[tuple[int,int]]:
    """Potential entries sharing only a translated terminal JR RA delay word.

    Entry evidence is still required. This only keeps such a target visible to
    callback discovery; ordinary interior instructions remain excluded.
    """
    entries = {(f['section'],f['vram']) for f in functions}
    slots = set()
    for f in functions:
        if f['origin'] in ('native-replacement','unsupported-boundary'):
            continue
        owned = f['instruction_addresses']
        pc = max(owned)
        if (f['section'],pc) in entries or pc-4 not in owned or pc <= f['vram']:
            continue
        offset = f['rom_offset'] + pc-4-f['vram']
        if rom[offset:offset+4] == bytes.fromhex('03e00008'):
            slots.add((f['section'],pc))
    return slots


def indirect_field_calls(rom: bytes, function: dict) -> list[dict]:
    """Report local object-field loads consumed by JALR, without guessing types.

    Matching field offsets in unrelated objects do not establish an ABI. These
    sites only group stored-pointer candidates for a shared manual review.
    """
    result = []
    for run in owned_ranges(function):
        instructions = decode(rom[run['rom_offset']:run['rom_offset']+run['size']],run['vram'])
        for n,i in enumerate(instructions):
            if i.opcode != 'jalr':
                continue
            register = i.rs
            for previous in reversed(instructions[max(0,n-8):n]):
                if previous.delay:
                    # A null check may skip this call without changing the
                    # loaded pointer on its fallthrough path. Other transfers
                    # still end the local provenance search.
                    if not (previous.opcode == 'beq' and
                            {previous.rs,previous.rt} == {0,register} and target(previous) >= i.pc + 8):
                        break
                if not previous.gpr_write_mask & (1<<register):
                    continue
                if previous.opcode == 'lw' and previous.rs != 29 and previous.immediate >= 0:
                    result.append(dict(pc=i.pc,expected_word=i.word,load_pc=previous.pc,
                                       field_offset=previous.immediate,base_register=previous.rs))
                break
    return result


def owned_ranges(function: dict) -> list[dict]:
    """Contiguous owned runs, excluding gaps inside a function's span."""
    runs = []
    for pc in function['instruction_addresses']:
        if runs and runs[-1]['vram']+runs[-1]['size']==pc:
            runs[-1]['size'] += 4
        else:
            runs.append(dict(vram=pc,size=4,rom_offset=function['rom_offset']+pc-function['vram']))
    return runs


def preserves_stack_words(rom: bytes, function: dict) -> bool:
    """A fully translated leaf with no stores, stack changes or external exits."""
    if (function['discovery_errors'] or function['origin'] in ('native-replacement', 'unsupported-boundary')
            or function.get('fallthrough_transfers') or function.get('tail_continuations')):
        return False
    owned = set(function['instruction_addresses'])
    for i in decode_function(rom, function):
        if (i.opcode not in SUPPORTED or i.opcode in STORE_BYTES or i.gpr_write_mask & (1 << 29)
                or i.opcode in ('break', 'syscall', 'cache', 'jal', 'jalr',
                                'bltzal', 'bgezal', 'bltzall', 'bgezall')):
            return False
        if i.opcode == 'jr' and i.rs != 31:
            return False
        if i.delay and i.opcode != 'jr' and target(i) not in owned:
            return False
    return True


def indirect_parameter_uses(rom: bytes, function: dict, preserves_stack=None, called_parameters=None) -> dict:
    """Trace entry arguments through copies, stack saves and direct wrappers to JALR.

    Masks describe possible argument origins, not constant runtime values. Track
    o32 incoming stack words through exact SP adjustments in the straight entry
    prologue. A nested call preserves stack origins only when its entire body
    is a read-only leaf. The call still kills volatile registers after its delay
    slot. Unknown stores and alternate prologue entries end the proof.
    """
    instructions = {i.pc:i for i in decode_function(rom,function)}
    transfers = {t['pc']:t for t in function.get('indirect_transfers',[])}
    # Prove stack loads before the CFG origin union. Deriving them during that
    # union could retain an optimistic load after conflicting frame paths join.
    stack_loads, stack_offset, pc = {}, 0, function['vram']
    # Offsets are relative to SP on entry. Initially only o32 stack arguments
    # have defined origins. Register home slots need an actual store.
    slots = {offset: offset // 4 for offset in range(16, 253, 4)}
    original_args = set(range(4, 8))
    call_slot = False
    while pc in instructions:
        i = instructions[pc]
        if i.delay:
            if call_slot or i.opcode != 'jal' or preserves_stack is None or not preserves_stack(target(i)):
                break
            call_slot = True
            pc += 4
            continue
        if i.opcode not in SUPPORTED or i.opcode in ('break', 'syscall'):
            break
        offset = stack_offset + i.immediate
        if i.opcode in STORE_BYTES:
            if i.rs != 29:
                break
            size = STORE_BYTES[i.opcode]
            low = offset & -size if i.opcode in ('swl', 'swr', 'sdl', 'sdr') else offset
            for word in range(low & ~3, low + size, 4):
                slots.pop(word, None)
            if i.opcode == 'sw' and offset % 4 == 0 and i.rt in original_args:
                slots[offset] = i.rt - 4
        if i.opcode == 'lw' and i.rs == 29 and offset in slots:
            stack_loads[pc] = slots[offset]
        if i.gpr_write_mask & (1 << 29):
            if i.opcode not in ('addi', 'addiu', 'daddiu') or i.rs != 29 or i.rt != 29:
                break
            stack_offset += i.immediate
        original_args = {r for r in original_args if not i.gpr_write_mask & (1 << r)}
        if call_slot:
            original_args.clear()
            call_slot = False
        pc += 4
    # A local back edge into the prologue would execute a load with a different
    # frame/argument lifetime. Treat it as unproved rather than reusing the
    # initial invocation's stack origin.
    prologue_end = max(stack_loads,default=function['vram']-4)
    for i in instructions.values():
        destinations = (transfers.get(i.pc,{}).get('targets',[]) if i.opcode == 'jr' else
                        [target(i)] if i.delay and i.opcode not in
                        ('jal','jalr','bltzal','bgezal','bltzall','bgezall') else [])
        if any(function['vram'] <= address <= prologue_end for address in destinations):
            stack_loads.clear()
            break
    if any(function['vram'] <= site.get('resume_pc', -1) <= prologue_end
           for site in (*function.get('fallthrough_transfers', []), *function.get('tail_continuations', []))):
        stack_loads.clear()
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
        if i.pc in stack_loads:
            result[i.rt] = 1 << stack_loads[i.pc]
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

    def record(origins, site, producer_pc):
        while origins:
            bit = (origins & -origins).bit_length() - 1
            argument = bit + 4 if bit < 4 else ('stack', bit * 4)
            uses.setdefault(argument, {})[(producer_pc, site['pc'])] = site
            origins &= origins - 1

    # Follow an argument through a direct wrapper to a proven indirect call.
    # Restrict this summary to straight-line wrappers with ordinary returns.
    forwards = called_parameters is not None and all(
        not i.delay or i.opcode == 'jal' or (i.opcode == 'jr' and i.rs == 31)
        for i in instructions.values())
    while pending:
        pc = pending.popleft()
        i, state = instructions[pc],states[pc]
        if i.opcode not in SUPPORTED or i.opcode in ('break','syscall'):
            continue
        if i.opcode=='jalr' and i.rd==31:
            record(state[i.rs], dict(pc=pc, expected_word=i.word), pc)
        after = advance(i,state)
        if not i.delay:
            enqueue(pc+4,after)
            continue
        slot = instructions.get(pc+4)
        if slot is None or slot.delay or slot.opcode not in SUPPORTED:
            continue
        delayed = advance(slot,after)
        if forwards and i.opcode == 'jal' and any(delayed[4:8]):
            for register, sites in called_parameters(target(i)).items():
                if type(register) is int and delayed[register]:
                    for site in sites:
                        record(delayed[register], dict(site, forward_pc=pc, forwarded_to=target(i),
                                                       forwarded_register=register), pc)
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
    return {r:[sites[pc] for pc in sorted(sites)] for r,sites in uses.items()}


def indexed_call_tables(rom: bytes, function: dict, *, fixed_slots=False) -> list[dict]:
    """Find owned LW -> JALR flows through constant-base, scaled-index arrays.

    This proves an address expression, not table bounds or immutable contents.
    At joins keep only agreeing expressions; calls kill volatile registers.
    Collect uses after convergence so an optimistic early path cannot survive
    a conflicting predecessor. Delay slots cannot change an already read target.
    """
    if fixed_slots and (function.get('fallthrough_transfers') or function.get('tail_continuations')):
        return []
    instructions = {i.pc: i for i in decode_function(rom, function)}
    if not any(i.opcode == 'jalr' for i in instructions.values()):
        return []
    transfers = {t['pc']: t for t in function.get('indirect_transfers', [])}
    # Expressions: constant(value), indexed(base, stride), loaded(base, load_pc, stride).
    # Scaling an unknown integer does not establish a selector bound.
    initial = (('constant', 0),) + (None,) * 31
    states, pending = {function['vram']: initial}, deque([function['vram']])

    def add(left, right):
        if left is None or right is None:
            return None
        if left[0] == 'constant' and right[0] == 'constant':
            return ('constant', (left[1] + right[1]) & 0xffffffff)
        if left[0] == 'constant' and right[0] == 'indexed':
            return ('indexed', (left[1] + right[1]) & 0xffffffff, right[2])
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
            elif 2 <= i.shift <= 5:
                value = ('indexed', 0, 1 << i.shift)
            elif i.shift == 0:
                value = right
        elif i.opcode == 'lw' and left and left[0] == ('constant' if fixed_slots else 'indexed'):
            base = (left[1] + i.immediate) & 0xffffffff
            if base % 4 == 0:
                value = (('fixed', base, frozenset({i.pc})) if fixed_slots
                         else ('loaded', base, i.pc, left[2]))
        if destination and value is not None:
            result[destination] = value
        result[0] = ('constant', 0)
        return tuple(result)

    def agree(a, b):
        if a == b:
            return a
        # Separate branches may load the same global at different PCs.
        if fixed_slots and a and b and a[0] == b[0] == 'fixed' and a[1] == b[1]:
            return ('fixed', a[1], a[2] | b[2])
        return None

    def enqueue(pc, state):
        if pc not in instructions:
            return
        old = states.get(pc)
        merged = tuple(agree(a, b) for a, b in zip(old, state)) if old is not None else state
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
        if i.opcode != 'jalr' or i.rd != 31 or not value:
            continue
        if value[0] == 'fixed':
            uses.append(dict(pc=pc, expected_word=i.word, slot_vram=value[1], load_pcs=sorted(value[2])))
        elif value[0] == 'loaded':
            uses.append(dict(pc=pc, expected_word=i.word, table_vram=value[1], load_pc=value[2], stride=value[3]))
    return uses


def fixed_callback_stores(rom: bytes, function: dict, called_slots: dict) -> dict:
    """Trace a straight-line setter's incoming registers to proven global call slots."""
    if not called_slots or function.get('fallthrough_transfers') or function.get('tail_continuations'):
        return {}
    constants, arguments, stores = {0: 0}, set(range(4, 8)), {}
    returning = False
    for i in decode_function(rom, function):
        if i.delay and (i.opcode != 'jr' or i.rs != 31 or returning):
            return {}
        if i.opcode not in SUPPORTED or i.opcode in ('break', 'syscall'):
            return {}
        base = constants.get(i.rs)
        if i.opcode in STORE_BYTES:
            if i.opcode != 'sw' or base is None or (base + i.immediate) % 4:
                return {}
            address = (base + i.immediate) & 0xffffffff
            stores.pop(address, None)
            if address in called_slots and i.rt in arguments:
                stores[address] = (i.rt, i.pc)
        constants = {r: v for r, v in constants.items() if not i.gpr_write_mask & (1 << r)}
        arguments = {r for r in arguments if not i.gpr_write_mask & (1 << r)}
        if i.rt and i.opcode == 'lui':
            constants[i.rt] = (i.word & 0xffff) << 16
        elif i.rt and i.opcode in ('addiu', 'ori') and base is not None:
            constants[i.rt] = ((base + i.immediate) & 0xffffffff if i.opcode == 'addiu'
                               else base | (i.word & 0xffff))
        if returning:
            matches = {}
            for address, (reg, pc) in stores.items():
                matches.setdefault(reg, []).extend(dict(site, store_pc=pc) for site in called_slots[address])
            return matches
        returning = i.opcode == 'jr'
    return {}


def initializer_callback_stores(rom: bytes, functions: list[dict], contracts: dict) -> dict:
    """Link a leaf initializer's store to a consumer on the same object.

    The consumer's first call must pass a field of its incoming object to a
    checked callback parameter. A caller must pass the same object through
    adjacent initializer and consumer calls. Keep section identity at each
    step; matching offsets on unrelated objects never establishes a method.
    """
    if not contracts:
        return {}
    by_address = {}
    for f in functions:
        by_address.setdefault(f['vram'], []).append(f)

    def resolve(address, sid):
        matches = by_address.get(address, [])
        return next((f for f in matches if f['section'] == sid), matches[0] if len(matches) == 1 else None)

    def translated(f):
        return (not f['discovery_errors'] and f['origin'] not in ('native-replacement', 'unsupported-boundary')
                and not f.get('fallthrough_transfers') and not f.get('tail_continuations')
                and not f.get('registered_direct_calls'))

    def callers(addresses):
        words = {(0x0c000000 | ((a >> 2) & 0x03ffffff)).to_bytes(4, 'big') for a in addresses}
        for f in functions:
            start = f['rom_offset']
            if translated(f) and any(rom.find(w, start, start + f['size']) >= 0 for w in words):
                yield f

    def prefix_calls(f):
        # Track values as (argument register, byte offset, loaded field).
        # Examine the straight fallthrough path before the first call and
        # optionally its immediate successor. No stack values are assumed.
        values = {r: (r, 0, False) for r in range(4, 8)}
        sp_offset = 0
        pending, first_call, previous_pc = None, None, f['vram'] - 4
        for i in decode_function(rom, f):
            if i.pc != previous_pc + 4 or i.opcode not in SUPPORTED or i.opcode in ('break', 'syscall'):
                return
            previous_pc = i.pc
            if first_call is not None and i.pc == first_call + 8 and i.opcode != 'jal':
                return
            if i.delay:
                if pending is not None or i.opcode in ('j', 'jr', 'jalr', 'bltzal', 'bgezal', 'bltzall', 'bgezall'):
                    return
                if i.opcode != 'jal' and (constant_condition(i) is True or target(i) <= i.pc + 4):
                    return
                pending = i
                continue
            if pending is None or not pending.likely:
                if i.gpr_write_mask & (1 << 29):
                    if i.opcode not in ('addi', 'addiu', 'daddiu') or i.rt != 29 or i.rs != 29:
                        return
                    sp_offset += i.immediate
                if i.opcode in STORE_BYTES:
                    if (i.rs != 29 or i.immediate < 0 or sp_offset + i.immediate + STORE_BYTES[i.opcode] > 0
                            or i.opcode in ('swl', 'swr', 'sdl', 'sdr')):
                        return
                source = values.get(i.rs)
                copied = None
                dest = i.rt
                if i.opcode in ('addu', 'or') and 0 in (i.rs, i.rt):
                    dest, copied = i.rd, values.get(i.rs or i.rt)
                elif source and not source[2] and i.opcode in ('addiu', 'lw'):
                    copied = (source[0], source[1] + i.immediate, i.opcode == 'lw')
                for r in list(values):
                    if i.gpr_write_mask & (1 << r):
                        del values[r]
                if dest and copied:
                    values[dest] = copied
            if pending and pending.opcode == 'jal':
                yield pending.pc, target(pending), dict(values)
                if first_call is not None:
                    return
                first_call = pending.pc
                values = {r: v for r, v in values.items() if 16 <= r <= 23 or r == 30}
            pending = None

    readers = {}
    for f in callers({address for _, address, _ in contracts}):
        first = next(prefix_calls(f), None)
        if first is None:
            continue
        pc, address, values = first
        callee = resolve(address, f['section'])
        if callee is None:
            continue
        for r, (arg, offset, loaded) in values.items():
            contract = contracts.get((callee['section'], address, r))
            if loaded and offset >= 0 and offset % 4 == 0 and contract and callee['sha256'] == contract['sha256']:
                readers.setdefault((f['section'], f['vram']), []).append(
                    (arg, offset, dict(registrar=address, register_pc=pc, argument_register=r,
                                       indirect_pc=contract['call_pc'])))
    result = {}
    for f in callers({address for _, address in readers}):
        calls = list(prefix_calls(f))
        if len(calls) != 2:
            continue
        init_pc, init_addr, init_args = calls[0]
        consume_pc, consume_addr, consume_args = calls[1]
        init, consumer = resolve(init_addr, f['section']), resolve(consume_addr, f['section'])
        if init is None or consumer is None or not translated(init):
            continue
        for arg, offset, evidence in readers.get((consumer['section'], consume_addr), []):
            obj = consume_args.get(arg)
            if obj is None or obj[1:] != (0, False):
                continue
            for r in range(4, 8):
                if init_args.get(r) != obj:
                    continue
                pc = final_argument_store(rom, init, r, offset)
                if pc is not None:
                    result.setdefault((init['section'], pc), []).append(dict(evidence,
                        caller=f['vram'], caller_section=f['section'], init_pc=init_pc, consumer=consume_addr,
                        consume_pc=consume_pc, object_register=r, field_offset=offset))
    return result


def final_argument_store(rom: bytes, f: dict, arg: int, offset: int) -> int | None:
    """Find the final full-word write to an unchanged argument in a leaf."""
    store, returning = None, False
    # The wrapper keeps the object in s0..s7 or fp across the initializer.
    preserved = (1 << arg) | (0xff << 16) | (1 << 30) | (1 << 29) | (1 << 31)
    for i in decode_function(rom, f):
        if (i.opcode not in SUPPORTED or i.opcode in ('break', 'syscall')
                or i.gpr_write_mask & preserved
                or (i.delay and (returning or i.opcode != 'jr' or i.rs != 31))):
            return None
        if i.opcode in STORE_BYTES:
            if i.rs != arg or i.opcode in ('swl','swr','sdl','sdr'):
                return None
            if i.immediate < offset + 4 and offset < i.immediate + STORE_BYTES[i.opcode]:
                store = i.pc if i.immediate == offset and i.opcode == 'sw' else None
        if returning:
            return store
        returning = i.opcode == 'jr'
    return None


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
                         source_function: dict | None = None,
                         source_ranges: list[dict] | None = None,
                         owned_addresses: set[int] | None = None,
                         shared_entries: set[int] | None = None) -> list[dict]:
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
        if address % 4 or (address in owned and address not in (shared_entries or ())) or index < 0:
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

    if source_function is not None:
        source_ranges = owned_ranges(source_function)
    ranges = source_ranges if source_ranges is not None else text
    def instructions_in(region):
        offset = region['rom_offset']
        return decode(rom[offset:offset + region['size']], region['vram'])

    # Only propagate across branches when all incoming paths of the caller
    # are known. Local jump tables and explicit continuations are joins too.
    follow_branch = source_function is not None and not source_function.get('discovery_errors')
    runs = (instructions_in(region) for region in ranges)
    joins = set()
    if follow_branch:
        runs = list(runs)
        joins.update(target(i) for run in runs for i in run if i.delay and i.opcode not in ('jr','jalr','jal'))
        joins.update(pc for t in source_function.get('indirect_transfers', []) if t['kind'] == 'jump' for pc in t['targets'])
        for site in [*source_function.get('fallthrough_transfers', []), *source_function.get('tail_continuations', [])]:
            if 'resume_pc' in site:
                joins.add(site['resume_pc'])

    conditional = {'beq','bne','blez','bgtz','bltz','bgez','bc1t','bc1f',
                   'beql','bnel','blezl','bgtzl','bltzl','bgezl','bc1tl','bc1fl'}
    for instructions in runs:
        registers = {0: 0}
        stack_arguments = {}
        pending = None
        before_likely_slot = None
        if not follow_branch:
            joins = {target(i) for i in instructions if i.delay and i.opcode not in ('jr','jalr','jal')}
        for i in instructions:
            if i.opcode not in SUPPORTED:
                registers, pending = {0:0}, None
                stack_arguments.clear()
                continue
            if i.pc in joins:
                registers = {0: 0}
                stack_arguments.clear()
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
                if left is None and i.rs != 29 and i.opcode == 'sw' and i.immediate >= 0:
                    details.update(base_register=i.rs,field_offset=i.immediate)
                record(right,i.pc,'stored-address',i.rt,**details)
            if i.opcode in STORE_BYTES and i.rs != 29:
                # An unproved alias may overwrite the outgoing argument area.
                stack_arguments.clear()
            if i.rs == 29 and i.opcode in STORE_BYTES:
                # o32 arguments beyond a3 are words in the caller's outgoing
                # stack area. Preserve their producer sites for batch review.
                # Partial/unknown stores invalidate every overlapping word.
                size = STORE_BYTES[i.opcode]
                low = i.immediate & ~(size-1) if i.opcode in ('swl','swr','sdl','sdr') else i.immediate
                for slot in list(stack_arguments):
                    if slot < low + size and low < slot + 4:
                        stack_arguments.pop(slot)
                if i.opcode == 'sw' and right is not None and 16 <= i.immediate <= 252 and i.immediate % 4 == 0:
                    stack_arguments[i.immediate] = (right,i.pc,i.rt)
            if i.opcode == 'jalr' and left is not None:
                record(left,i.pc,'constant-indirect-call',i.rs)
            for register in list(registers):
                if i.gpr_write_mask & (1<<register):
                    registers.pop(register)
            if i.gpr_write_mask & (1<<29):
                stack_arguments.clear()
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
                for slot,(address,store_pc,register) in sorted(stack_arguments.items()):
                    details = {'callee':target(previous)} if previous.opcode == 'jal' else {}
                    record(address,previous.pc,'stack-argument',register,
                           stack_offset=slot,store_pc=store_pc,**details)
            if previous is not None:
                if previous.opcode in ('jal', 'jalr'):
                    registers = {r:v for r,v in registers.items() if r in (0, *range(16,24),28,29,30)}
                    stack_arguments.clear()
                elif (follow_branch and previous.opcode in conditional and constant_condition(previous) is not True
                      and i.pc not in joins):
                    if previous.likely:
                        registers, stack_arguments = before_likely_slot
                else:
                    registers = {0: 0}
                    stack_arguments.clear()
            if follow_branch and i.likely and i.opcode in conditional:
                before_likely_slot = (registers.copy(), stack_arguments.copy())
            registers[0] = 0
    return [found[address] for address in sorted(found)]
