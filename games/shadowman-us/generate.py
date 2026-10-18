"""Game-specific inputs for CMake source generation."""
from pathlib import Path


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    section = next(s for s in metadata['sections'] if s['name'] == '.game')
    constants.update(GAME_SECTION=section['id'], GAME_ROM_OFFSET=section['rom_offset'],
                     GAME_VRAM=section['vram'], GAME_SIZE=section['size'], STARTUP=int(startup))
    lines = ['/* Generated from the pinned Shadow Man ROM profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    (output / 'shadowman_config.h').write_text('\n'.join(lines))
