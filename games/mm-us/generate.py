"""Resolve Majora's Mask's DMADATA records and its relocating overlay loader."""
import hashlib
from pathlib import Path
import struct

from common import RecompError
from yaz0 import yaz0


def file_image(rom, dma_table, file):
    start, end, physical, compressed_end = struct.unpack_from('>4I', rom, dma_table + 16 * file['index'])
    if start != file['vrom'] or end - start != file['size']:
        raise RecompError(f'MM DMA record mismatch at {start:#x}')
    raw = (yaz0(rom[physical:compressed_end], end - start) if compressed_end
           else rom[physical:physical + end - start])
    if hashlib.sha256(raw).hexdigest() != file['sha256']:
        raise RecompError(f'MM CPU image fingerprint mismatch at {start:#x}')
    return raw


def prepare(rom, profile, output):
    discovery = profile['discovery']
    regions = list(discovery['regions'])
    image = bytearray(rom)
    relocations = {}
    lines = ['static MmOverlay overlays[] = {']
    for file in [dict(discovery['code'], name='.code'), *discovery['overlays']]:
        raw = file_image(rom, discovery['dma_table'], file)
        section = len(regions)
        regions.append(dict(name=file['name'], vram=file['vram'], rom_offset=len(image), size=len(raw)))
        image.extend(raw)
        if file['name'] == '.code':
            for fn in profile['controls']['functions']:
                offset = fn['vram'] - file['vram']
                if hashlib.sha256(raw[offset:offset + fn['size']]).hexdigest() != fn['sha256']:
                    raise RecompError(f"MM camera/visibility function mismatch at {fn['vram']:#x}")
            continue
        offset = file['reloc_offset']
        text, data, rodata, bss, count = struct.unpack_from('>5I', raw, offset)
        if (text != file['text_size'] or bss != file['bss_size'] or offset != text + data + rodata
                or offset + 20 + 4 * count > len(raw) - 4):
            raise RecompError(f'MM overlay relocation header mismatch for {file["name"]}')
        sites = set()
        for i in range(count):
            reloc = struct.unpack_from('>I', raw, offset + 20 + 4 * i)[0]
            if reloc >> 30 == 1 and (reloc >> 24 & 63) in (4, 5, 6):
                sites.add(file['vram'] + (reloc & 0xffffff))
        relocations[section] = sites
        digest = ','.join(str(b) for b in bytes.fromhex(file['sha256']))
        lines.append(f'    {{{file["vram"]:#x}, {len(raw):#x}, {text:#x}, {bss:#x}, {offset:#x}, {{{digest}}}, 0}},')
    (output / 'mm_overlays.h').write_text('\n'.join([*lines, '};', '']))
    return bytes(image), {**profile, 'game': {**profile['game'], 'code_sections': [r['name'] for r in regions]},
                          'discovery': {**discovery, 'regions': regions}, 'mm_relocations': relocations}


def finalize_metadata(rom, metadata, profile):
    for function in metadata['functions']:
        sites = profile['mm_relocations'].get(function['section'])
        if sites is not None:
            function['relocatable'] = True
            function['relocation_pcs'] = sorted(sites.intersection(function['instruction_addresses']))


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game = profile['game']
    boot = profile['bootstrap']
    code = profile['discovery']['code']
    constants = {'entry': metadata['entry_vram'], 'rom_size': game['rom_size'], 'ram_size': game['ram_size'],
                 'code_vram': code['vram'], 'code_size': code['size']}
    constants.update({key: value for key, value in boot.items() if type(value) is int})
    lines = [f'#define MM_{name.upper()} UINT32_C({value:#x})' for name, value in constants.items()]
    for name, digest in [('mm_sha1', game['rom_sha1']), ('mm_code_sha256', code['sha256'])]:
        lines.append(f'static const uint8_t {name}[] = {{' + ','.join(str(b) for b in bytes.fromhex(digest)) + '};')
    lines.append('static const uint32_t mm_boot_parameters[][2] = {')
    lines.extend(f'    {{{address}, {value:#x}}},' for address, value in boot['parameters'].items())
    lines.append('};')
    (output / 'game_config.h').write_text('\n'.join([*lines, '']))
    bindings = [f'#define MM_CTL_{name.upper()} UINT32_C({value:#x})'
                for name, value in profile['controls'].items() if isinstance(value, int)]
    (output / 'mm_controls_config.h').write_text('\n'.join(bindings) + '\n')


def rsp_images(rom, profile):
    discovery = profile['discovery']
    code = file_image(rom, discovery['dma_table'], discovery['code'])
    rsp = profile['graphics']['rsp']
    boot = rsp['images'][0]
    offset = boot['rom_offset']
    original = rom[offset:offset + boot['size']]
    jump = rsp['restart_jump_offset']
    return {'.code': code, 'rsp-boot-restart': original[jump:jump + 4] + original[4:]}
