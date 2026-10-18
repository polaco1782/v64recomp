"""Prepare Hexen's compressed CPU image; retain cartridge offsets at runtime."""
import hashlib
from pathlib import Path
from common import RecompError
from lzh import decode_lzh


def prepare(rom: bytes, profile: dict, output: Path):
    boot = profile['bootstrap']
    offset, size = boot['compressed_offset'], boot['decoded_size']
    if int.from_bytes(rom[offset:offset + 4], 'big') != size:
        raise RecompError('Hexen decoded size header mismatch')
    raw = decode_lzh(rom[offset + 4:], size, rom[0x1df20:0x1e020], rom[0x1e020:0x1e120])
    if hashlib.sha256(raw).hexdigest() != boot['decoded_sha256']:
        raise RecompError('Hexen decoded program fingerprint mismatch')
    region = dict(name='.game', vram=boot['decoded_address'], rom_offset=len(rom), size=size)
    return rom + raw, {**profile, 'discovery': {**profile['discovery'],
        'regions': [*profile['discovery']['regions'], region]}}


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    constants.update({s['name'][1:].upper() + '_SECTION': s['id'] for s in metadata['sections']})
    lines = ['/* Generated from the pinned Hexen US profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    raw = prepare(game['rom'].read_bytes(), profile, output)[0]
    helper = hashlib.sha256(raw[0x800000+0x800dc9b0-boot['decoded_address']:0x800000+0x800dca28-boot['decoded_address']]).digest()
    lines.append('static const uint8_t memmove_sha256[32] = {' + ','.join(str(b) for b in helper) + '};')
    lines.append('static const uint8_t decoded_sha256[32] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(boot['decoded_sha256'])) + '};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    for name, ctype in [('runtime', 'RecompSchedulerConfig'), ('platform', 'RecompPlatformConfig')]:
        tag = 'scheduler' if name == 'runtime' else name
        lines.append(f'static const {ctype} hexen_{tag}_config = {{')
        lines.extend(f'    .{k}=UINT32_C(0x{v:x}),' for k,v in profile[name].items())
        lines.append('};')
    (output / 'hexen_config.h').write_text('\n'.join(lines))
