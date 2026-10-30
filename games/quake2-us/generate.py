"""Game-specific inputs for CMake source generation."""
from pathlib import Path
import hashlib
from common import RecompError, write_json


def verify_input(rom, profile, metadata=None, coverage=None):
    """Bind the reviewed command ABI to complete bodies and decoded operands.

    These checks establish static ROM evidence, not live gameplay acceptance.
    """
    spec = profile['input_adapter']

    def require(condition, message):
        if not condition:
            raise RecompError('Quake II input: ' + message)

    def raw(address, size):
        regions = [r for r in profile['discovery']['regions']
                   if r['vram'] <= address and address + size <= r['vram'] + r['size']]
        require(len(regions) == 1 and size > 0, 'unmapped evidence span')
        offset = regions[0]['rom_offset'] + address - regions[0]['vram']
        require(offset + size <= len(rom), 'evidence outside ROM')
        return rom[offset:offset + size]

    require(len(rom) == profile['game']['rom_size'] and
            hashlib.sha1(rom).hexdigest() == profile['game']['rom_sha1'], 'ROM identity mismatch')
    owned = set()
    for item in spec['functions'] + spec['data']:
        require(hashlib.sha256(raw(item['vram'], item['size'])).hexdigest() == item['sha256'],
                item['name'] + ' fingerprint mismatch')
    for item in spec['functions']:
        if metadata is None:
            owned.update(range(item['vram'], item['vram'] + item['size'], 4))
            continue
        matches = [f for f in metadata['functions'] if f['vram'] == item['vram']]
        require(len(matches) == 1, item['name'] + ' missing/ambiguous in discovery')
        f = matches[0]
        require(f['size'] == item['size'] and f['sha256'] == item['sha256'],
                item['name'] + ' discovered boundary mismatch')
        owned.update(f['instruction_addresses'])
        if coverage is not None:
            statuses = [c['status'] for c in coverage['functions']
                        if c['vram'] == f['vram'] and c['section'] == f['section']]
            require(statuses == ['ready'], item['name'] + ' original is not translated')
    names = set()
    for binding in spec['bindings']:
        name, pcs = binding['name'], binding['pcs']
        require(name not in names and all(pc in owned for pc in pcs), name + ' duplicate/unowned evidence')
        names.add(name)
        words = [int.from_bytes(raw(pc, 4), 'big') for pc in pcs]
        require(words == binding['words'], name + ' instruction mismatch')
        if binding['kind'] == 'address':
            hi, low = words
            require(hi >> 26 == 15 and low >> 26 in (35, 43) and
                    (hi >> 16) & 31 == (low >> 21) & 31, name + ' invalid global access')
            value = ((hi & 0xffff) << 16) + ((low & 0x7fff) - (low & 0x8000))
        elif binding['kind'] == 'immediate':
            require(len(words) == 1 and words[0] >> 26 in (9, 35, 43, 49, 57), name + ' invalid field access')
            value = words[0] & 0xffff
        elif binding['kind'] == 'return':
            require(len(words) == 2 and pcs[1] == pcs[0] + 4 and words[0] >> 26 == 3,
                    name + ' invalid call/delay pair')
            target = ((pcs[0] + 4) & 0xf0000000) | ((words[0] & 0x3ffffff) << 2)
            require(any(w['vram'] == target for w in profile['function_wrappers']), name + ' wrong wrapper target')
            value = pcs[0] + 8
        else:
            raise RecompError('Quake II input: unknown binding kind')
        require(value == binding['value'], name + ' decoded operand mismatch')
    return dict(verification='static ROM evidence', runtime_verified=False,
                rom_sha1=profile['game']['rom_sha1'], functions=spec['functions'],
                data=spec['data'], bindings=spec['bindings'],
                metadata_verified=metadata is not None, coverage_verified=coverage is not None)


def prepare(rom, profile, output):
    verify_input(rom, profile)
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
    lines = ['/* Generated from verified Quake II input operands. */']
    lines.extend(f'#define Q2_INPUT_{b["name"].upper()} UINT32_C(0x{b["value"]:x})'
                 for b in profile['input_adapter']['bindings'])
    (output / 'quake2_input_config.h').write_text('\n'.join(lines) + '\n')


def write_report(rom, profile, metadata, coverage, output):
    write_json(output / 'input-hooks.json', verify_input(rom, profile, metadata, coverage))
