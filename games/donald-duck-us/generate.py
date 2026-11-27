"""Emit the boot configuration for the checked US cartridge."""
from pathlib import Path


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    if boot['parameters']['0x80000318'] != game['ram_size']:
        raise ValueError('IPL osMemSize must match the configured RDRAM size')
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    lines = ['/* Generated from the pinned Donald Duck US ROM profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    sha1 = ', '.join(f'0x{byte:02x}' for byte in bytes.fromhex(game['rom_sha1']))
    lines.append(f'static const uint8_t boot_sha1[20] = {{{sha1}}};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    (output / 'game_config.h').write_text('\n'.join(lines))
