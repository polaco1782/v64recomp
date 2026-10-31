"""Shared ROM evidence checks for opt-in game input adapters."""
import hashlib
from common import RecompError


def verify_input_evidence(rom, profile, metadata=None, coverage=None):
    """Bind the reviewed command ABI to complete bodies and decoded operands.

    These checks establish static ROM evidence, not live gameplay acceptance.
    """
    spec = profile['input_adapter']

    def require(condition, message):
        if not condition:
            raise RecompError(profile['game']['name'] + ' input: ' + message)

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
            require(hi >> 26 == 15 and low >> 26 in (9, 32, 33, 35, 36, 37, 40, 41, 43) and
                    (hi >> 16) & 31 == (low >> 21) & 31, name + ' invalid global access')
            value = ((hi & 0xffff) << 16) + ((low & 0x7fff) - (low & 0x8000))
        elif binding['kind'] == 'immediate':
            require(len(words) == 1 and words[0] >> 26 in (9, 32, 33, 35, 36, 37, 40, 41, 43, 49, 57), name + ' invalid field access')
            value = words[0] & 0xffff
        elif binding['kind'] == 'return':
            require(len(words) == 2 and pcs[1] == pcs[0] + 4 and words[0] >> 26 == 3,
                    name + ' invalid call/delay pair')
            target = ((pcs[0] + 4) & 0xf0000000) | ((words[0] & 0x3ffffff) << 2)
            require(any(w['vram'] == target for w in profile['function_wrappers']), name + ' wrong wrapper target')
            value = pcs[0] + 8
        else:
            raise RecompError(profile['game']['name'] + ' input: unknown binding kind')
        require(value == binding['value'], name + ' decoded operand mismatch')
    return dict(verification='static ROM evidence', runtime_verified=False,
                rom_sha1=profile['game']['rom_sha1'], functions=spec['functions'],
                data=spec['data'], bindings=spec['bindings'],
                metadata_verified=metadata is not None, coverage_verified=coverage is not None)
