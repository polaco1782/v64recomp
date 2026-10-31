"""Game-specific inputs for CMake source generation."""
from pathlib import Path
from common import write_json
from input_evidence import verify_input_evidence


def prepare(rom, profile, output):
    verify_input_evidence(rom, profile)
    return rom, profile


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    constants.update(STARTUP=int(startup))
    lines = ['/* Generated from the pinned Nightmare Creatures ROM profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    (output / 'nightmare_config.h').write_text('\n'.join(lines))
    lines = ['/* Generated from verified Nightmare Creatures input operands. */']
    lines.extend(f'#define NC_INPUT_{b["name"].upper()} UINT32_C(0x{b["value"]:x})'
                 for b in profile['input_adapter']['bindings'])
    (output / 'nightmare_input_config.h').write_text('\n'.join(lines) + '\n')


def write_report(rom, profile, metadata, coverage, output):
    write_json(output / 'input-hooks.json', verify_input_evidence(rom, profile, metadata, coverage))
