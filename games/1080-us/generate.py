"""Prepare the pinned Kyoto USO text and emit 1080 bootstrap constants."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import struct
from common import RecompError
from uso import sections_at, section_bytes
from uso_symbols import symbol_inventory, merge_entries


def prepare_text(rom: bytes, uso: dict) -> tuple[bytearray, list[int], dict, dict]:
    inventory = json.loads(Path(__file__).with_name(uso['symbols']).read_text())
    module = uso['module_rom_offset']
    text_rom = uso['text_rom_offset']
    text_size = uso['text_size']
    reloc_rom = uso['reloc_rom_offset']
    if (inventory['format_version'] != 1 or inventory['rom_sha1'] != hashlib.sha1(rom).hexdigest()
            or inventory['module_offset'] != module or inventory['text_rom_offset'] != text_rom
            or inventory['text_base'] != uso['text_vram'] or inventory['text_size'] != text_size
            or inventory['reloc_rom_offset'] != reloc_rom or inventory['reloc_count'] != uso['reloc_count']
            or inventory.get('header_word', 0x12345678) != uso.get('header_word', 0x12345678)
            or inventory['prepared_text_sha256'] != uso['sha256']):
        raise RecompError('1080 USO symbol inventory identity mismatch')
    sections = sections_at(rom, module, uso.get('header_word', 0x12345678))
    if 5 not in sections or 2 not in sections:
        raise RecompError('1080 missing USO Text or relocation section')
    stored_size = uso.get('text_rom_size', text_size)
    compressed = uso.get('compression') == 'yay0'
    if (sections[5][:2] != (text_rom, stored_size) or sections[2][0] != reloc_rom
            or bool(sections[5][2] & 0x10000) != compressed):
        raise RecompError('1080 USO text/relocation layout mismatch')
    reloc_size = sections[2][1]
    if reloc_size != uso['reloc_count'] * 12 or hashlib.sha1(rom).hexdigest() != inventory['rom_sha1']:
        raise RecompError('1080 USO relocation count mismatch')
    raw_text = section_bytes(rom, sections[5])
    if (len(raw_text) != text_size or compressed and (
            inventory.get('text_rom_size') != stored_size
            or inventory.get('stored_text_sha256') != hashlib.sha256(rom[text_rom:text_rom + stored_size]).hexdigest())):
        raise RecompError('1080 compressed USO source fingerprint mismatch')
    reloc_bytes = rom[reloc_rom:reloc_rom + reloc_size]
    if (hashlib.sha256(raw_text).hexdigest() != inventory['raw_text_sha256']
            or hashlib.sha256(reloc_bytes).hexdigest() != inventory['reloc_sha256']):
        raise RecompError('1080 USO source fingerprint mismatch')
    values = {int(key): value for key, value in inventory['symbol_values'].items()}
    if not values or any(not 0 <= value <= 0xffffffff for value in values.values()):
        raise RecompError('1080 USO symbol inventory malformed')

    image = bytearray(raw_text)
    sites = []
    pending_hi = None
    for index in range(uso['reloc_count']):
        encoded, offset, saved = struct.unpack_from('>3I', reloc_bytes, index * 12)
        symbol, kind = encoded >> 4, encoded & 7
        if kind not in (1, 2, 3) or offset & 3 or offset + 4 > text_size:
            raise RecompError('1080 invalid USO relocation record')
        if symbol not in values:
            raise RecompError('1080 unresolved USO symbol')
        value = values[symbol]
        word = struct.unpack_from('>I', raw_text, offset)[0]
        if kind == 1:
            patched = (word & 0xfc000000) | (((word & 0x03ffffff) + (value >> 2)) & 0x03ffffff)
            struct.pack_into('>I', image, offset, patched)
        elif kind == 3:
            if pending_hi is not None:
                raise RecompError('1080 overlapping USO HI16 relocations')
            pending_hi = (symbol, offset, word)
        else:
            struct.pack_into('>I', image, offset, (word & 0xffff0000) | ((word + value) & 0xffff))
            if pending_hi is not None:
                hi_symbol, hi_offset, hi_word = pending_hi
                if hi_symbol != symbol:
                    raise RecompError('1080 unmatched USO HI16/LO16 relocation')
                low = (word & 0xffff) - (0x10000 if word & 0x8000 else 0)
                full = ((hi_word & 0xffff) << 16) + low + value
                patched_hi = (hi_word & 0xffff0000) | (((full + 0x8000) >> 16) & 0xffff)
                struct.pack_into('>I', image, hi_offset, patched_hi)
                pending_hi = None
        # Unbound J26 imports must read the instruction written by the guest
        # linker at call time. Resolved calls retain their checked static target.
        if kind in (2, 3) or (kind == 1 and word >> 26 == 3 and value & 0x0fffffff == 0):
            sites.append(uso['text_vram'] + offset)
    if pending_hi is not None or hashlib.sha256(image).hexdigest() != uso['sha256']:
        raise RecompError('1080 prepared USO fingerprint mismatch')
    return image, sites, sections, values


def prepare_rodata(rom: bytes, config: dict, sections: dict, values: dict) -> bytes:
    """Resolve a module's checked ROD section, including local switch tables."""
    rod_offset, rod_size, _ = sections[7]
    reloc_offset, reloc_size, _ = sections[4]
    if (rod_offset, rod_size, reloc_offset, reloc_size) != (
            config['rom_offset'], config['size'],
            config['reloc_rom_offset'], config['reloc_count'] * 12):
        raise RecompError('1080 USO ROD layout mismatch')
    image = bytearray(rom[rod_offset:rod_offset + rod_size])
    for index in range(config['reloc_count']):
        encoded, offset, _ = struct.unpack_from('>3I', rom, reloc_offset + index * 12)
        if encoded & 7 != 4 or offset & 3 or offset + 4 > len(image) or encoded >> 4 not in values:
            raise RecompError('1080 invalid USO ROD relocation')
        word = struct.unpack_from('>I', image, offset)[0]
        struct.pack_into('>I', image, offset, (word + values[encoded >> 4]) & 0xffffffff)
    if hashlib.sha256(image).hexdigest() != config['sha256']:
        raise RecompError('1080 prepared USO ROD fingerprint mismatch')
    return bytes(image)


def resolve_overlay_tables(rom: bytes, discovery: dict) -> None:
    """Keep the owner Text and reused ROD mappings explicit for USO switches."""
    retained, resolved = [], []
    for table in discovery.get('jump_tables', []):
        if 'table_section' not in table:
            retained.append(table)
            continue
        region = next(r for r in discovery['regions'] if r['name'] == table['table_section'])
        address, count = table['table_vram'], table['count']
        if (type(count) is not int or not 0 < count <= 8192 or address % 4
                or address < region['vram'] or address + count * 4 > region['vram'] + region['size']
                or table.get('section') not in {r['name'] for r in discovery['regions']}):
            raise RecompError('1080 invalid overlay switch mapping')
        offset = region['rom_offset'] + address - region['vram']
        raw = rom[offset:offset + count * 4]
        indices = table.get('indices', list(range(count)))
        if (hashlib.sha256(raw).hexdigest() != table['sha256'] or not indices
                or len(set(indices)) != len(indices)
                or any(type(i) is not int or not 0 <= i < count for i in indices)):
            raise RecompError('1080 overlay switch fingerprint/indices mismatch')
        targets = sorted({struct.unpack_from('>I', raw, index * 4)[0] for index in indices})
        resolved.append(dict(table, kind='jump', targets=targets, table_count=count,
                             table_indices=indices, table_sha256=table['sha256']))
    discovery['jump_tables'] = retained
    discovery['transfers'] = [*discovery.get('transfers', []), *resolved]


def prepare(rom: bytes, profile: dict, output: Path) -> tuple[bytes, dict]:
    profile = deepcopy(profile)
    uso = profile['discovery']['uso']
    image, sites, sections, values = prepare_text(rom, uso)
    text_size = uso['text_size']
    region = next(r for r in profile['discovery']['regions'] if r['name'] == '.bootup')
    if region['rom_offset'] != len(rom) or region['vram'] != uso['text_vram'] or region['size'] != text_size:
        raise RecompError('1080 bootup USO analysis mapping mismatch')
    # Section headers are type/size/flags; relocation offsets start at the
    # following payload. ROD records use ABS32 additions.
    rod_config = profile['discovery']['uso_rodata']
    rod = prepare_rodata(rom, rod_config, sections, values)
    rod_region = next(r for r in profile['discovery']['regions'] if r['name'] == '.bootup_rodata')
    if (rod_region['rom_offset'] != len(rom) + len(image) or rod_region['size'] != len(rod)
            or rod_region['vram'] != rod_config['vram']):
        raise RecompError('1080 bootup ROD analysis mapping mismatch')
    prepared_rom = rom + image + rod
    relocation_pcs = {'.bootup': set(sites)}
    for module in profile['discovery'].get('uso_modules', []):
        text, module_sites, module_sections, module_values = prepare_text(rom, module)
        region = next(r for r in profile['discovery']['regions'] if r['name'] == module['name'])
        if (region['rom_offset'] != len(prepared_rom) or region['vram'] != module['text_vram']
                or region['size'] != len(text)):
            raise RecompError('1080 USO module analysis mapping mismatch')
        prepared_rom += text
        relocation_pcs[module['name']] = set(module_sites)
        if 'rodata' in module:
            config = module['rodata']
            rod = prepare_rodata(rom, config, module_sections, module_values)
            region = next(r for r in profile['discovery']['regions'] if r['name'] == config['name'])
            if (region['rom_offset'] != len(prepared_rom) or region['vram'] != config['vram']
                    or region['size'] != len(rod)):
                raise RecompError('1080 USO module ROD analysis mapping mismatch')
            prepared_rom += rod
    resolve_overlay_tables(prepared_rom, profile['discovery'])
    entries = json.loads(profile['discovery']['entry_inventory'].read_text())
    if entries.get('rom_sha1') != profile['game']['rom_sha1'] or entries.get('format_version') != 1:
        raise RecompError('1080 function entry inventory identity mismatch')
    if 'uso_entry_inventory' in profile['discovery']:
        path = Path(__file__).with_name(profile['discovery']['uso_entry_inventory'])
        symbols = json.loads(path.read_text())
        if symbols != symbol_inventory(rom, prepared_rom, profile):
            raise RecompError('1080 declared USO symbol inventory mismatch; rerun discover_symbols.py')
        entries = merge_entries(entries, symbols)
    for entry in entries['entries']:
        address = entry['vram']
        mapped = [r for r in profile['discovery']['regions']
                  if r['vram'] <= address <= r['vram'] + r['size'] - 16
                  and ('section' not in entry or entry['section'] == r['name'])]
        if len(mapped) > 1:
            # Older resident inventories predate overlay section annotations.
            # Resolve them by their pinned entry bytes, never by region order.
            mapped = [r for r in mapped if hashlib.sha256(prepared_rom[
                r['rom_offset'] + address - r['vram']:
                r['rom_offset'] + address - r['vram'] + 16]).hexdigest() == entry['sha256']]
        if len(mapped) != 1:
            raise RecompError(f'1080 function entry outside analysis image: {address:#x}')
        mapped = mapped[0]
        offset = mapped['rom_offset'] + address - mapped['vram']
        if hashlib.sha256(prepared_rom[offset:offset + 16]).hexdigest() != entry['sha256']:
            raise RecompError(f'1080 function entry fingerprint mismatch: {address:#x}')
        entry['section'] = mapped['name']
    entries['rom_sha1'] = hashlib.sha1(prepared_rom).hexdigest()
    prepared_entries = output / 'prepared-entries.json'
    prepared_entries.write_text(json.dumps(entries, indent=2) + '\n')
    profile['discovery']['entry_inventory'] = prepared_entries
    profile['uso_relocation_pcs'] = relocation_pcs
    return prepared_rom, profile


def finalize_metadata(rom: bytes, metadata: dict, profile: dict) -> None:
    sites = profile['uso_relocation_pcs']
    for function in metadata['functions']:
        name = metadata['sections'][function['section']]['name']
        if name in sites:
            function['relocatable'] = True
            function['relocation_pcs'] = sorted(sites[name].intersection(function['instruction_addresses']))


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    constants['BOOTUP_SECTION'] = next(s['id'] for s in metadata['sections'] if s['name'] == '.bootup')
    constants['BOOTUP_ADDRESS'] = profile['discovery']['uso']['text_vram']
    constants['BOOTUP_SIZE'] = profile['discovery']['uso']['text_size']
    constants['STARTUP'] = int(startup)
    lines = ['/* Generated from the pinned 1080 Snowboarding JU revision 0 profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint8_t bootup_sha256[32] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(boot['initial_bootup_sha256'])) + '};')
    inventory = json.loads(Path(__file__).with_name(profile['discovery']['uso']['symbols']).read_text())
    linked = {int(k): v for k, v in inventory.get('initial_symbol_values', {}).items()}
    lines.append('static const struct { uint32_t pc, target, word; } uso_import_calls[] = {')
    uso = profile['discovery']['uso']
    rom = profile['game']['rom'].read_bytes()
    for index in range(uso['reloc_count']):
        encoded, offset, _ = struct.unpack_from('>3I', rom, uso['reloc_rom_offset'] + index * 12)
        symbol = encoded >> 4
        if encoded & 7 != 1 or symbol not in linked:
            continue
        raw = struct.unpack_from('>I', rom, uso['text_rom_offset'] + offset)[0]
        word = (raw & 0xfc000000) | (((raw & 0x03ffffff) + (inventory['symbol_values'][str(symbol)] >> 2)) & 0x03ffffff)
        target = 0x80000000 | ((word & 0x03ffffff) << 2)
        lines.append(f'    {{0x{uso["text_vram"] + offset:x}, 0x{target:x}, 0x{word:x}}},')
    lines.append('};')
    lines.append('static const struct { uint32_t address, size, section; uint8_t sha256[32]; } uso_modules[] = {')
    for module in profile['discovery'].get('uso_modules', []):
        section = next(s['id'] for s in metadata['sections'] if s['name'] == module['name'])
        digest = ','.join(f'0x{b:02x}' for b in bytes.fromhex(module['sha256']))
        lines.append(f"    {{0x{module['text_vram']:x}, 0x{module['text_size']:x}, {section}, {{{digest}}}}},")
    lines.append('};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    (output / 'snowboarding_config.h').write_text('\n'.join(lines))
