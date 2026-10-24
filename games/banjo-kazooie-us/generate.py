"""Prepare the pinned US revision's two-stream RareZip cores and overlay slot.

Only verified cartridge bytes are inputs; reference source trees are not needed.
"""
from copy import deepcopy
import hashlib
from pathlib import Path
import zlib

from common import RecompError


def decode_overlay(rom: bytes, overlay: dict) -> bytes:
    position, end = overlay['rom_offset'], overlay['rom_end']
    parts = []
    for expected_size in (overlay['text_size'], overlay['size'] - overlay['text_size']):
        if rom[position:position + 2] != b'\x11\x72':
            raise RecompError(f"Banjo RareZip header mismatch at {position:#x}")
        if int.from_bytes(rom[position + 2:position + 6], 'big') != expected_size:
            raise RecompError('Banjo RareZip declared size mismatch')
        decoder = zlib.decompressobj(-15)
        raw = decoder.decompress(rom[position + 6:end], expected_size + 1)
        if not decoder.eof or len(raw) != expected_size:
            raise RecompError('Banjo RareZip decoded size mismatch')
        position = end - len(decoder.unused_data)
        parts.append(raw)
    raw = b''.join(parts)
    if hashlib.sha256(raw).hexdigest() != overlay['sha256']:
        raise RecompError(f"Banjo {overlay['name']} decoded fingerprint mismatch")
    return raw


def prepare(rom: bytes, profile: dict, output: Path) -> tuple[bytes, dict]:
    profile = deepcopy(profile)
    image = bytearray(rom)
    for overlay in profile['discovery']['overlays']:
        raw = decode_overlay(rom, overlay)
        region = dict(name=overlay['name'], vram=overlay['vram'],
                      rom_offset=len(image), size=len(raw))
        if overlay['vram'] == 0x803863f0:
            region['overlay_group'] = 'level'
        profile['discovery']['regions'].append(region)
        image.extend(raw)
    return bytes(image), profile


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game = profile['game']
    constants = dict(ENTRY=metadata['entry_vram'], ROM_SIZE=game['rom_size'], RAM_SIZE=game['ram_size'])
    constants.update({k.upper(): v for k, v in profile['bootstrap'].items() if type(v) is int})
    lines = ['/* Generated from the pinned Banjo-Kazooie US revision 0 profile. */']
    lines += [f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items()]
    lines += ['static const uint8_t boot_sha1[20] = {' + ','.join(str(b) for b in bytes.fromhex(game['rom_sha1'])) + '};',
              'static const uint32_t boot_parameters[][2] = {']
    lines += [f'    {{{address}, 0x{value:x}}},' for address, value in profile['bootstrap']['parameters'].items()]
    lines += ['};']
    lines += ['static const struct { uint32_t section, address, size; uint8_t sha256[32]; } banjo_images[] = {']
    for overlay in profile['discovery']['overlays']:
        section = next(s['id'] for s in metadata['sections'] if s['name'] == overlay['name'])
        digest = ','.join(str(b) for b in bytes.fromhex(overlay['sha256']))
        lines.append(f'    {{{section}, 0x{overlay["vram"]:x}, 0x{overlay["size"]:x}, {{{digest}}}}},')
    lines += ['};']
    (output / 'banjo_config.h').write_text('\n'.join(lines) + '\n')
