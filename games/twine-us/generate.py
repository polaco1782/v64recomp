"""Emit configuration for the pinned USA revision 0 cartridge."""
from pathlib import Path


def configure_translation(rom: bytes, metadata: dict, profile: dict, output: Path) -> None:
    """Keep unconverted paths explicit while bringing up the partial port."""
    from common import write_json
    from translate import translate
    audit = translate(rom, metadata, profile, output, report_only=True)
    functions = {(f['section'], f['vram']): f for f in metadata['functions']}
    pending = []
    for function in audit['functions']:
        if function['status'] != 'unsupported':
            continue
        original = functions[function['section'], function['vram']]
        offset = original['rom_offset']
        pending.append(dict(symbol=original['name'], vram=original['vram'],
            kind='unsupported_function', reason='TWINE pending conversion: ' + function['errors'][0]['reason'],
            expected_words=[int.from_bytes(rom[offset:offset + 4], 'big')]))
    profile.setdefault('boundaries', []).extend(pending)
    write_json(output / 'pending-boundaries.json', dict(complete_conversion=False, boundaries=pending))


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'],
                 'RAM_SIZE': game['ram_size'], 'STARTUP': int(startup)}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    lines = ['/* Generated from the pinned TWINE USA revision 0 profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(
        f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},'
                 for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    (output / 'twine_config.h').write_text('\n'.join(lines))
