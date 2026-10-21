"""Game-specific inputs for CMake source generation."""
from pathlib import Path
from common import RecompError


def prepare(rom, profile, output):
    boot = profile['bootstrap']
    start = boot['idle_transition']
    expected = [0x3c048009, 0x24844c60, 0x0c01484c, 0x00002821,
                0x0c0148f0, 0, 0x08000148, 0]
    region = next((r for r in profile['discovery']['regions']
                   if r['vram'] <= start and start + len(expected) * 4 <= r['vram'] + r['size']), None)
    if (region is None or start != 0x80000510 or boot['idle_words'] != expected
            or boot['idle_thread'] != boot['main_object'] or boot['idle_thread'] != 0x80094c60
            or boot['main_entry'] != 0x800004ac or boot['main_id'] != 1 or boot['main_priority'] != 10):
        raise RecompError('Quake II idle-thread identity/transition mismatch')
    offset = region['rom_offset'] + start - region['vram']
    if rom[offset:offset + len(expected) * 4] != b''.join(w.to_bytes(4, 'big') for w in expected):
        raise RecompError('Quake II idle-thread transition fingerprint mismatch')
    return rom, profile


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    constants.update(STARTUP=int(startup))
    lines = ['/* Generated from the pinned Quake II ROM profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    (output / 'quake2_config.h').write_text('\n'.join(lines))
