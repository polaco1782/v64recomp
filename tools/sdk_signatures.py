"""Match complete SDK routine signatures against a bounded region of a ROM."""
from collections import defaultdict
import zlib

from emit import SUPPORTED, decode, target

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


def matching_sdk_bodies(rom, offset, end, prefixes):
    """Yield relocation-masked SDK body matches wholly inside [offset, end)."""
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
            yield symbol, signature


def sdk_jal_references(rom, function, prefixes):
    """Find named direct calls in catalogued original bytes of a replacement."""
    start = function['rom_offset']
    end = start + function['size']
    callees = defaultdict(set)
    for _, signature in matching_sdk_bodies(rom, start, end, prefixes):
        if signature['body_size'] != function['size']:
            continue
        for kind, name, offsets in signature['references']:
            if kind == 'targ26':
                for offset in offsets:
                    callees[offset].add(name)
    for offset, names in sorted(callees.items()):
        word = int.from_bytes(rom[start + offset:start + offset + 4], 'big')
        if len(names) == 1 and word >> 26 == 3:
            yield function['vram'] + offset, word, next(iter(names))


def sdk_address_references(rom, function, prefixes):
    """Resolve same-symbol HI16/LO16 address constructions in a matched SDK body."""
    start, size = function['rom_offset'], function['size']
    matches = [sig for _, sig in matching_sdk_bodies(rom, start, start + size, prefixes)
               if sig['body_size'] == size]
    if not matches:
        return
    # Byte-identical variants must agree on both relocation names.
    common = set.intersection(*({(kind, offset, name) for kind, name, offsets in sig['references']
                                 for offset in offsets if kind in ('hi16', 'lo16')} for sig in matches))
    labels = defaultdict(set)
    for kind, offset, name in common:
        labels[kind, offset].add(name)
    if not labels:
        return
    pc = function['vram']
    instructions = decode(rom[start:start + size], pc)
    joins = {target(i) for i in instructions if i.delay and i.opcode not in ('jr', 'jalr', 'jal')}
    reached = (None if function.get('origin') == 'native-replacement'
               else set(function['instruction_addresses']))
    high = {}
    delay_slot = False
    for i in instructions:
        skip_slot, delay_slot = delay_slot, i.delay
        if (skip_slot or i.delay or i.opcode not in SUPPORTED or i.opcode in ('break', 'syscall')
                or reached is not None and i.pc not in reached):
            high.clear()
            continue
        if i.pc in joins:
            high.clear()
        offset = i.pc - pc
        upper = high.get(i.rs)
        if (upper and i.rt and i.opcode in ('addiu', 'ori')
                and labels.get(('lo16', offset)) == {upper[0]}):
            low = i.immediate if i.opcode == 'addiu' else i.word & 0xffff
            yield i.pc, upper[1], (upper[2] + low) & 0xffffffff, upper[0]
        for register in tuple(high):
            if i.gpr_write_mask & (1 << register):
                del high[register]
        names = labels.get(('hi16', offset))
        if i.opcode == 'lui' and i.rt and names and len(names) == 1:
            high[i.rt] = (next(iter(names)), i.pc, (i.word & 0xffff) << 16)
