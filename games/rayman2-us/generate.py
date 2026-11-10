"""Rayman 2's pinned cartridge configuration for the shared generator."""
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
    if boot['parameters']['0x80000318'] != game['ram_size']:
        raise ValueError('IPL osMemSize must match the configured RDRAM size')
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    constants['STARTUP'] = int(startup)
    lines = ['/* Generated from the pinned Rayman 2 US ROM profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    (output / 'game_config.h').write_text('\n'.join(lines))
    lines = ['/* Checked Rayman 2 camera/input operands from profile.toml. */']
    lines.extend(f'#define R2_CTL_{b["name"].upper()} UINT32_C(0x{b["value"]:x})'
                 for b in profile['controls']['bindings'])
    (output / 'rayman2_controls_config.h').write_text('\n'.join(lines) + '\n')
