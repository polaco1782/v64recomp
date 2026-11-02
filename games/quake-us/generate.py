"""Game-specific inputs for CMake source generation."""
from pathlib import Path
from controls_analysis import verify_controls


def prepare(rom, profile, output):
    verify_controls(rom, profile)
    return rom, profile


def write_report(rom, profile, metadata, coverage, output):
    from common import write_json
    write_json(output / 'controls-hooks.json', verify_controls(rom, profile, metadata, coverage))


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    constants.update(STARTUP=int(startup))
    lines = ['/* Generated from the pinned Quake 64 ROM profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    (output / 'quake_config.h').write_text('\n'.join(lines))
    lines = ['/* Verified Quake US command, projection and culling operands. */']
    for binding in profile['controls']['bindings']:
        value = binding['value']
        literal = repr(value) if isinstance(value, float) else f'UINT32_C(0x{value:x})'
        lines.append(f'#define Q_CTL_{binding["name"].upper()} {literal}')
    (output / 'quake_controls_config.h').write_text('\n'.join(lines) + '\n')
