"""Conservative callback-entry candidates from known data and CPU text ranges.

Candidates are evidence for discovery, not a complete code classification.
Known local switch tables and already-owned instructions are excluded.
"""
from bisect import bisect_right
import hashlib
import struct

from emit import decode, SUPPORTED


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
                         source_ranges: list[dict] | None = None) -> list[dict]:
    """Track LUI/address pairs used to pass or store callback pointers.

    Optional source ranges let reports scan owned runs in other sections while
    keeping candidate targets restricted to the supplied text ranges.
    """
    text = sorted(text_ranges, key=lambda r: r['vram'])
    starts = [r['vram'] for r in text]
    owned = {pc for f in metadata['functions'] for pc in f['instruction_addresses']}
    found = {}
    for region in source_ranges if source_ranges is not None else text:
        registers = {0: 0}
        pending = None
        for offset in range(0, region['size'], 4):
            pc = region['vram']+offset
            word = struct.unpack_from('>I', rom, region['rom_offset']+offset)[0]
            op, rs, rt, rd = word>>26, word>>21&31, word>>16&31, word>>11&31
            previous, pending = pending, None
            if op == 15:
                registers[rt] = (word&65535)<<16
            elif op in (9, 13):
                value = registers.get(rs)
                if value is None:
                    registers.pop(rt, None)
                else:
                    immediate = word&65535
                    if op == 9 and immediate&32768:
                        immediate -= 65536
                    address = ((value+immediate) if op==9 else (value|immediate))&0xffffffff
                    registers[rt] = address
                    index = bisect_right(starts, address)-1
                    if address % 4 == 0 and address not in owned and index >= 0:
                        target = text[index]
                        if address+16 <= target['vram']+target['size']:
                            raw = target['rom_offset']+address-target['vram']
                            if decode(rom[raw:raw+4], address)[0].opcode in SUPPORTED:
                                found.setdefault(address, dict(vram=address, origin='checked-root',
                                    sha256=hashlib.sha256(rom[raw:raw+16]).hexdigest(),
                                    materialized_sources=[]))['materialized_sources'].append(pc)
            elif op == 0:
                function = word&63
                if function in (8, 9):
                    pending = 'all' if function==8 else 'volatile'
                elif function not in (12, 13, 24, 25, 26, 27, 28, 29, 30, 31):
                    registers.pop(rd, None)
            elif op in (2, 3):
                pending = 'all' if op==2 else 'volatile'
            elif op in (1, 4, 5, 6, 7, 20, 21, 22, 23):
                # A linear scan cannot establish values at a control-flow merge.
                pending = 'all'
            elif op in (8, 10, 11, 12, 14, 16, 17, 24, 25, 26, 27, 32, 33, 34, 35, 36, 37, 38, 39, 48, 52, 55):
                registers.pop(rt, None)
            if previous == 'all':
                registers = {0: 0}
            elif previous == 'volatile':
                for register in (*range(1,16), 24, 25, 31):
                    registers.pop(register, None)
            registers[0] = 0
    return [found[address] for address in sorted(found)]
