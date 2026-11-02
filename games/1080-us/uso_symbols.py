"""ROM-declared Kyoto Text symbols, independent of heuristic code scanning.

The resident loader at 0x800025a4 walks 12-byte symbol records. At
0x800025d4..0x80002624 it resolves kind 2/4 records as section_base + value;
the section number is flags >> 4. 0x52 therefore declares a defined Text
symbol; 0x54 declares the Text section base, not another function.
"""
import hashlib
import struct

from common import RecompError
from uso import sections_at


def symbol_inventory(rom: bytes, prepared: bytes, profile: dict) -> dict:
    modules = [dict(profile['discovery']['uso'], name='.bootup'),
               *profile['discovery'].get('uso_modules', [])]
    inventories, entries, excluded = [], [], []
    for module in modules:
        sections = sections_at(rom, module['module_rom_offset'], module.get('header_word', 0x12345678))
        offset, size, flags = sections[1]
        if size % 12 or flags & 0x10000:
            raise RecompError('1080 invalid symbol-table layout')
        region = next(r for r in profile['discovery']['regions'] if r['name'] == module['name'])
        text = prepared[region['rom_offset']:region['rom_offset'] + region['size']]
        if (region['vram'], region['size'], hashlib.sha256(text).hexdigest()) != (
                module['text_vram'], module['text_size'], module['sha256']):
            raise RecompError('1080 symbol Text mapping mismatch')
        ranges = [r for r in profile['discovery'].get('uso_non_cpu_ranges', [])
                  if r['section'] == module['name']]
        for item in ranges:
            start = item['vram'] - module['text_vram']
            end = start + item['size']
            if (item['kind'] not in ('rsp', 'padding') or not item.get('reason')
                    or start < 0 or end > len(text) or item['size'] <= 0
                    or hashlib.sha256(text[start:end]).hexdigest() != item['sha256']
                    or item['kind'] == 'padding' and any(text[start:end])):
                raise RecompError('1080 invalid non-CPU symbol classification')
        symbols = {}
        for index in range(size // 12):
            kind, _, value, _ = struct.unpack_from('>HHII', rom, offset + index * 12)
            if kind != 0x52:
                continue
            if value % 4 or value + 16 > len(text):
                raise RecompError(f'1080 defined Text symbol outside module: {module["name"]}:{index}')
            symbols.setdefault(value, []).append(index)
        inventories.append(dict(section=module['name'], module_rom_offset=module['module_rom_offset'],
                                symbols_rom_offset=offset, symbols_size=size,
                                symbols_sha256=hashlib.sha256(rom[offset:offset + size]).hexdigest(),
                                defined_text_symbols=sum(map(len, symbols.values()))))
        for value, indices in sorted(symbols.items()):
            address = module['text_vram'] + value
            classification = [r for r in ranges if r['vram'] <= address < r['vram'] + r['size']]
            if len(classification) > 1:
                raise RecompError('1080 overlapping symbol classifications')
            if classification:
                excluded.append(dict(section=module['name'], vram=address, symbol_indices=indices,
                                     kind=classification[0]['kind'], reason=classification[0]['reason']))
                continue
            entries.append(dict(section=module['name'], vram=address,
                                origin='checked-root', symbol_indices=indices,
                                sha256=hashlib.sha256(text[value:value + 16]).hexdigest(),
                                evidence='USO symbol flags 0x52: defined Text offset; '
                                         'loader resolution checked at 0x800025d4..0x80002624. '
                                         'Normal discovery must validate the body and its transfers.'))
    return dict(format_version=1, rom_sha1=hashlib.sha1(rom).hexdigest(), complete=False,
                modules=inventories, entries=entries, excluded_symbols=excluded)


def merge_entries(existing: dict, symbols: dict) -> dict:
    entries = list(existing['entries'])
    for entry in symbols['entries']:
        matches = [e for e in entries if e['vram'] == entry['vram'] and (
            e.get('section') == entry['section'] or
            'section' not in e and e['sha256'] == entry['sha256'])]
        if len(matches) > 1 or matches and matches[0]['sha256'] != entry['sha256']:
            raise RecompError(f'1080 conflicting symbol entry: {entry["vram"]:#x}')
        if not matches:
            entries.append(entry)
    return dict(existing, entries=entries)
