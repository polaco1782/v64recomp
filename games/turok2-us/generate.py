"""Generate runtime constants for the verified Turok 2 US cartridge."""
from pathlib import Path
from common import write_json
from input_evidence import verify_input_evidence


def write_report(rom, profile, metadata, coverage, output):
    evidence = verify_input_evidence(rom, profile, metadata, coverage)
    write_json(output / "input-hooks.json", evidence)


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    constants.update({s['name'][1:].upper() + '_SECTION': s['id'] for s in metadata['sections']})
    lines = ['/* Generated from the pinned Turok 2 US 1.0 profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(str(b) for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    (output / 'game_config.h').write_text('\n'.join(lines))
    bindings = profile["input_adapter"]["bindings"]
    header = [f'#define AC_{b["name"].upper()} UINT32_C(0x{b["value"]:x})' for b in bindings]
    (output / "acclaim_controls_config.h").write_text("\n".join(header) + "\n")
