"""Game-specific inputs for CMake source generation."""
from pathlib import Path
from prepare import prepare  # Exported to the shared CMake generation driver.
import hashlib


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    constants.update(STARTUP=int(startup))
    lines = ['/* Generated from the pinned Ocarina of Time ROM profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    if startup:
        from prepare import file_bytes
        raw, _ = file_bytes(game['rom'].read_bytes(), 27)
        dd, _ = file_bytes(game['rom'].read_bytes(), 28)
        lines.append(f'#define OOT_DD_SIZE {len(dd)}')
        lines.append('static const uint8_t oot_dd_sha256[32] = {' + ','.join(str(b) for b in hashlib.sha256(dd).digest()) + '};')
        lines.append(f'#define OOT_CODE_SIZE {len(raw)}')
        lines.append('static const uint8_t oot_code_sha256[32] = {' + ','.join(str(b) for b in hashlib.sha256(raw).digest()) + '};')
    (output / 'oot_config.h').write_text('\n'.join(lines))



def finalize_metadata(rom, metadata, profile):
    for function in metadata['functions']:
        sites=profile.get('oot_relocations',{}).get(function['section'])
        if sites is not None:
            function['relocatable']=True
            owned=set(function['instruction_addresses'])
            function['relocation_pcs']=[pc for pc in sites if pc in owned]
