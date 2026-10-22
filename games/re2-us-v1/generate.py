"""Verified boot and raw-DEFLATE main-image preparation for RE2 US revision 1."""
from copy import deepcopy
import hashlib
from pathlib import Path
import struct
import zlib
from common import RecompError


def prepare(rom: bytes, profile: dict, output: Path) -> tuple[bytes, dict]:
    profile = deepcopy(profile)
    image = bytearray(rom)
    decoded = {}
    if rom[0x12cd0:0x12ce0] != b'OVERLAYTABLENUMC':
        raise RecompError('RE2 overlay table marker mismatch')
    for overlay in profile['discovery']['overlays']:
        offset, size = overlay['rom_offset'], overlay['compressed_size']
        expected = (0xb0000000 + offset, size, overlay['vram'], overlay['vram'] + overlay['size'])
        if struct.unpack_from('>4I', rom, overlay['record_offset']) != expected:
            raise RecompError('RE2 overlay record mismatch')
        if 'patch_source' in overlay:
            data = bytearray(decoded[overlay['patch_source']])
            if size % 4:
                raise RecompError('RE2 unaligned overlay patch list')
            site = 0
            for entry in range(offset, offset + size, 4):
                delta, value = struct.unpack_from('>HB', rom, entry)
                site += delta
                if site >= len(data):
                    raise RecompError('RE2 overlay patch outside source image')
                data[site] = value
        else:
            decoder = zlib.decompressobj(-15)
            data = decoder.decompress(rom[offset + 2:offset + size], overlay['size'] + 1)
            if not decoder.eof:
                raise RecompError('RE2 incomplete overlay stream')
        if (len(data) != overlay['size']
                or hashlib.sha256(data).hexdigest() != overlay['sha256']):
            raise RecompError('RE2 decoded overlay fingerprint mismatch')
        decoded[overlay['section']] = bytes(data)
        region = next(r for r in profile['discovery']['regions'] if r['name'] == overlay['section'])
        if region['vram'] != overlay['vram'] or region['size'] != len(data):
            raise RecompError('RE2 analysis mapping mismatch')
        image.extend(bytes((-len(image)) % 16))
        if region['rom_offset'] != len(image):
            raise RecompError('RE2 prepared offset mismatch')
        image.extend(data)
    return bytes(image), profile


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    lines = ['/* Generated from the pinned RE2 US revision 1 profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint8_t main_sha256[32] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(profile['discovery']['overlays'][0]['sha256'])) + '};')
    lines.append('static const struct { uint32_t address, size, compressed_size, cartridge, section; bool patched; uint8_t sha256[32]; } re2_overlays[] = {')
    for overlay in profile['discovery']['overlays']:
        section = next(s['id'] for s in metadata['sections'] if s['name'] == overlay['section'])
        digest = ','.join(f'0x{b:02x}' for b in bytes.fromhex(overlay['sha256']))
        patched = 'true' if 'patch_source' in overlay else 'false'
        lines.append(f'    {{0x{overlay["vram"]:x},0x{overlay["size"]:x},0x{overlay["compressed_size"]:x},0x{0xb0000000+overlay["rom_offset"]:x},{section},{patched},{{{digest}}}}},')
    lines.append('};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    (output / 're2_config.h').write_text('\n'.join(lines))
