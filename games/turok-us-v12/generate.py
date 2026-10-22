"""Prepare the pinned RNC1 program without changing cartridge offsets."""
import hashlib
from pathlib import Path
from common import RecompError
from rnc import decode_rnc1


def prepare(rom, profile, output):
    boot = profile['bootstrap']
    raw = decode_rnc1(rom[boot['compressed_offset']:])
    if len(raw) != boot['decoded_size'] or hashlib.sha256(raw).hexdigest() != boot['decoded_sha256']:
        raise RecompError('Turok program fingerprint mismatch')
    return rom + raw, profile


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    constants.update(STARTUP=int(startup))
    constants.update({s['name'][1:].upper() + '_SECTION': s['id'] for s in metadata['sections']})
    lines = ['/* Generated from the pinned Turok US v1.2 ROM profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint8_t decoded_sha256[32] = {' + ','.join(str(b) for b in bytes.fromhex(boot['decoded_sha256'])) + '};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    (output / 'turok_config.h').write_text('\n'.join(lines))
