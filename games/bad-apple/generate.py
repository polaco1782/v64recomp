"""Bad Apple generation and its explicit external-ROM build prerequisite."""
from pathlib import Path
from fetch_rom import ensure_rom


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game = profile['game']
    constants = dict(profile['bootstrap'], entry=metadata['entry_vram'],
                     rom_size=game['rom_size'], ram_size=game['ram_size'])
    lines = ['/* Generated from the pinned Bad Apple demo profile. */']
    lines.extend(f'#define BOOT_{name.upper()} UINT32_C(0x{value:x})'
                 for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' +
                 ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    (output / 'bad_apple_config.h').write_text('\n'.join(lines) + '\n')
