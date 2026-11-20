"""Generate Doom 64 boot parameters from the verified cartridge profile."""
from pathlib import Path
from common import write_json
from input_evidence import verify_adapter_evidence


def prepare(rom, profile, output):
    verify_adapter_evidence(rom, profile, 'fov_adapter')
    return rom, profile


def write_report(rom, profile, metadata, coverage, output):
    evidence = verify_adapter_evidence(rom, profile, 'fov_adapter', metadata, coverage)
    evidence['word_result_hooks'] = coverage['word_result_hooks']
    write_json(output / 'fov-hooks.json', evidence)


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    lines = ['/* Generated from the pinned Doom 64 ROM profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.extend(f'#define DOOM_INPUT_{name.upper()} UINT32_C(0x{value:x})'
                 for name, value in profile['controls'].items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    (output / 'game_config.h').write_text('\n'.join(lines))
    fov = [f'#define DOOM_FOV_{b["name"].upper()} UINT32_C(0x{b["value"]:x})'
           for b in profile['fov_adapter']['bindings']]
    (output / 'doom_fov_config.h').write_text('\n'.join(fov) + '\n')
