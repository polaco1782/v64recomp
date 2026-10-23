"""Prepare the pinned Kyoto USO text and emit 1080 bootstrap constants."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import struct
from common import RecompError


def prepare_text(rom: bytes, uso: dict) -> tuple[bytearray, list[int], dict, dict]:
    inventory = json.loads(Path(__file__).with_name(uso['symbols']).read_text())
    module = uso['module_rom_offset']
    text_rom = uso['text_rom_offset']
    text_size = uso['text_size']
    reloc_rom = uso['reloc_rom_offset']
    u32 = lambda offset: struct.unpack_from('>I', rom, offset)[0]
    if (inventory['format_version'] != 1 or inventory['rom_sha1'] != hashlib.sha1(rom).hexdigest()
            or inventory['module_offset'] != module or inventory['text_rom_offset'] != text_rom
            or inventory['text_base'] != uso['text_vram'] or inventory['text_size'] != text_size
            or inventory['reloc_rom_offset'] != reloc_rom or inventory['reloc_count'] != uso['reloc_count']
            or inventory['prepared_text_sha256'] != uso['sha256']):
        raise RecompError('1080 USO symbol inventory identity mismatch')
    if u32(module) != 0x12345678:
        raise RecompError('1080 USO header mismatch')
    sections = {}
    position = module + 4
    for _ in range(16):
        if position + 12 > len(rom):
            raise RecompError('1080 truncated USO section directory')
        kind, size, flags = struct.unpack_from('>3I', rom, position)
        if kind == 11:
            break
        if kind in sections or kind > 13 or (kind != 8 and position + 12 + size > len(rom)):
            raise RecompError('1080 malformed USO section directory')
        sections[kind] = (position + 12, size, flags)
        position += 12 + (0 if kind == 8 else (size + 3) & ~3)
    else:
        raise RecompError('1080 unterminated USO directory')
    if 5 not in sections or 2 not in sections:
        raise RecompError('1080 missing USO Text or relocation section')
    if sections[5][:2] != (text_rom, text_size) or sections[2][0] != reloc_rom:
        raise RecompError('1080 USO text/relocation layout mismatch')
    reloc_size = sections[2][1]
    if reloc_size != uso['reloc_count'] * 12 or hashlib.sha1(rom).hexdigest() != inventory['rom_sha1']:
        raise RecompError('1080 USO relocation count mismatch')
    raw_text = rom[text_rom:text_rom + text_size]
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
        if kind in (2, 3):
            sites.append(uso['text_vram'] + offset)
    if pending_hi is not None or hashlib.sha256(image).hexdigest() != uso['sha256']:
        raise RecompError('1080 prepared USO fingerprint mismatch')
    return image, sites, sections, values


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
    rod_offset, rod_size, _ = sections[7]
    rod_reloc, rod_reloc_size, _ = sections[4]
    rod = bytearray(rom[rod_offset:rod_offset + rod_size])
    if (rod_offset, rod_size, rod_reloc, rod_reloc_size) != (
            rod_config['rom_offset'], rod_config['size'],
            rod_config['reloc_rom_offset'], rod_config['reloc_count'] * 12):
        raise RecompError('1080 bootup ROD layout mismatch')
    for index in range(rod_reloc_size // 12):
        encoded, offset, saved = struct.unpack_from('>3I', rom, rod_reloc + index * 12)
        if encoded & 7 != 4 or offset & 3 or offset + 4 > len(rod):
            raise RecompError('1080 invalid bootup ROD relocation')
        word = struct.unpack_from('>I', rod, offset)[0]
        struct.pack_into('>I', rod, offset, (word + values[encoded >> 4]) & 0xffffffff)
    if hashlib.sha256(rod).hexdigest() != rod_config['sha256']:
        raise RecompError('1080 prepared bootup ROD fingerprint mismatch')
    rod_region = next(r for r in profile['discovery']['regions'] if r['name'] == '.bootup_rodata')
    if (rod_region['rom_offset'] != len(rom) + len(image) or rod_region['size'] != len(rod)
            or rod_region['vram'] != rod_config['vram']):
        raise RecompError('1080 bootup ROD analysis mapping mismatch')
    prepared_rom = rom + image + rod
    relocation_pcs = {'.bootup': set(sites)}
    for module in profile['discovery'].get('uso_modules', []):
        text, module_sites, _, _ = prepare_text(rom, module)
        region = next(r for r in profile['discovery']['regions'] if r['name'] == module['name'])
        if (region['rom_offset'] != len(prepared_rom) or region['vram'] != module['text_vram']
                or region['size'] != len(text)):
            raise RecompError('1080 USO module analysis mapping mismatch')
        prepared_rom += text
        relocation_pcs[module['name']] = set(module_sites)
    entries = json.loads(profile['discovery']['entry_inventory'].read_text())
    if entries.get('rom_sha1') != profile['game']['rom_sha1'] or entries.get('format_version') != 1:
        raise RecompError('1080 function entry inventory identity mismatch')
    for entry in entries['entries']:
        address = entry['vram']
        mapped = [r for r in profile['discovery']['regions']
                  if r['vram'] <= address <= r['vram'] + r['size'] - 16]
        if len(mapped) != 1:
            raise RecompError(f'1080 function entry outside analysis image: {address:#x}')
        mapped = mapped[0]
        offset = mapped['rom_offset'] + address - mapped['vram']
        if hashlib.sha256(prepared_rom[offset:offset + 16]).hexdigest() != entry['sha256']:
            raise RecompError(f'1080 function entry fingerprint mismatch: {address:#x}')
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
