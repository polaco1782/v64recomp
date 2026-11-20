"""Extract verified V1.2 CPU images for ahead-of-time translation.

Runtime continues to use the unmodified, compressed cartridge image.
"""
from __future__ import annotations
import hashlib
import struct
from pathlib import Path
from common import RecompError
from yaz0 import yaz0

DMA_TABLE = 0x7960

def file_bytes(rom: bytes, index: int) -> tuple[bytes, int]:
    start, end, physical, compressed_end = struct.unpack_from('>4I', rom, DMA_TABLE+index*16)
    if not start < end or physical == 0xffffffff:
        raise RecompError('invalid OOT CPU file')
    if compressed_end:
        return yaz0(rom[physical:compressed_end], end-start), start
    return rom[physical:physical+end-start], start

def prepare(rom: bytes, profile: dict, output: Path) -> tuple[bytes, dict]:
    image = bytearray(rom)
    regions = list(profile['discovery']['regions'])
    code, _ = file_bytes(rom, 27)
    regions.append(dict(name='.code', vram=0x800116e0, rom_offset=len(image), size=len(code)))
    image.extend(code)
    dd, _ = file_bytes(rom, 28)
    regions.append(dict(name=".n64dd", vram=0x801c7740, rom_offset=len(image), size=len(dd)))
    image.extend(dd)
    overlays = profile['discovery']['overlays']
    descriptors = []
    relocations = {}
    for overlay in overlays:
        raw, vrom = file_bytes(rom, overlay['index'])
        if hashlib.sha256(raw).hexdigest() != overlay['sha256'] or vrom != overlay['vrom']:
            raise RecompError('OOT overlay fingerprint mismatch')
        section = len(regions)
        regions.append(dict(name=overlay['name'], vram=overlay['vram'], rom_offset=len(image), size=len(raw)))
        image.extend(raw)
        offset = overlay['relocation_offset']
        count = struct.unpack_from('>I', raw, offset+16)[0]
        sites = []
        bases = [0, 0, overlay['text_size'], overlay['text_size']+overlay['data_size']]
        for k in range(count):
            entry = struct.unpack_from('>I', raw, offset+20+k*4)[0]
            if entry >> 30 == 1 and (entry >> 24 & 63) in (5, 6):
                sites.append(overlay['vram']+bases[entry >> 30]+(entry & 0xffffff))
        relocations[section] = sites
        descriptors.append(f'    {{{section}, 0x{overlay["vram"]:x}, 0x{len(raw):x}, 0x{overlay["text_size"]:x}, '
                           f'0x{overlay["bss_size"]:x}, 0x{offset:x}, '
                           '{'+','.join(str(b) for b in bytes.fromhex(overlay['sha256']))+'}, 0},')
    (output/'oot_overlays.h').write_text('static OotOverlay oot_overlays[] = {\n'+'\n'.join(descriptors)+'\n};\n')
    profile = {**profile, 'game': {**profile['game'], 'code_sections': [r['name'] for r in regions]},
               'discovery': {**profile['discovery'], 'regions': regions}, 'oot_relocations': relocations}
    return bytes(image), profile
