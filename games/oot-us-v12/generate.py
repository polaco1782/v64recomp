"""Game-specific inputs for CMake source generation."""
from pathlib import Path
from prepare import prepare as prepare_image
from controls_analysis import verify_controls
import hashlib


def prepare(rom, profile, output):
    image, resolved = prepare_image(rom, profile, output)
    verify_controls(image, resolved)
    return image, resolved


def write_report(rom, profile, metadata, coverage, output):
    from common import write_json
    image, resolved = prepare(rom, profile, output)
    write_json(output / 'controls-hooks.json', verify_controls(image, resolved, metadata, coverage))


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
    (output / 'game_config.h').write_text('\n'.join(lines))
    spec = profile['controls']
    lines = ['/* Verified NTSC 1.2 camera/input operands from profile.toml. */']
    lines.extend(f'#define OOT_CTL_{b["name"].upper()} UINT32_C(0x{b["value"]:x})' for b in spec['bindings'])
    bg = next(f for f in spec['functions'] if f['name'] == 'Camera_BGCheck')
    lines.append(f'#define OOT_CTL_BG_CHECK UINT32_C(0x{bg["vram"]:x})')
    lines.append(f'#define OOT_CTL_NATIVE_FOV {spec["native_fov"]["value"]}.0')
    # Read the reviewed settings table from the verified prepared image. The
    # default mode's normal-family function distinguishes 3D follow cameras.
    image, resolved = prepare_image(game['rom'].read_bytes(), profile, output)
    region = next(r for r in resolved['discovery']['regions'] if r['name'] == '.code')
    def raw(address, size):
        offset = region['rom_offset'] + address - region['vram']
        return image[offset:offset + size]
    flags = []
    for i in range(spec['settings_table']['count']):
        pointer = int.from_bytes(raw(spec['settings_table']['vram'] + i * 8 + 4, 4), 'big')
        flags.append(int(bool(pointer) and 1 <= int.from_bytes(raw(pointer, 2), 'big') <= 5))
    lines.append('static const uint8_t oot_normal_settings[] = {' + ','.join(map(str, flags)) + '};')
    (output / 'oot_controls_config.h').write_text('\n'.join(lines) + '\n')



def finalize_metadata(rom, metadata, profile):
    for function in metadata['functions']:
        sites=profile.get('oot_relocations',{}).get(function['section'])
        if sites is not None:
            function['relocatable']=True
            owned=set(function['instruction_addresses'])
            function['relocation_pcs']=[pc for pc in sites if pc in owned]
