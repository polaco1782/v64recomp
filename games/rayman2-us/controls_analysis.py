"""Checked camera projection, visibility and input ABI for Rayman 2 US."""
import hashlib
from common import RecompError


def verify_controls(rom, profile, metadata=None, coverage=None):
    spec = profile['controls']

    def require(ok, message):
        if not ok:
            raise RecompError('Rayman 2 controls: ' + message)

    require(len(rom) == profile['game']['rom_size'] and
            hashlib.sha1(rom).hexdigest() == profile['game']['rom_sha1'], 'ROM identity mismatch')

    def raw(address, size):
        regions = [r for r in profile['discovery']['regions'] if
                   r['vram'] <= address and address + size <= r['vram'] + r['size']]
        require(len(regions) == 1 and address % 4 == 0 and size > 0, 'unmapped evidence')
        offset = regions[0]['rom_offset'] + address - regions[0]['vram']
        require(0 <= offset and offset + size <= len(rom), 'evidence outside ROM')
        return rom[offset:offset + size]

    owned = set()
    for body in spec['functions']:
        require(hashlib.sha256(raw(body['vram'], body['size'])).hexdigest() == body['sha256'],
                body['name'] + ' fingerprint mismatch')
        if metadata is not None:
            matches = [f for f in metadata['functions'] if f['vram'] == body['vram'] and
                       f['section'] == body['section']]
            require(len(matches) == 1 and matches[0]['size'] == body['size'] and
                    matches[0]['sha256'] == body['sha256'], body['name'] + ' boundary mismatch')
            owned.update(matches[0]['instruction_addresses'])
            if coverage is not None:
                states = [f['status'] for f in coverage['functions'] if f['vram'] == body['vram'] and
                          f['section'] == body['section']]
                require(states == ['ready'], body['name'] + ' original not translated')
        else:
            owned.update(range(body['vram'], body['vram'] + body['size'], 4))
    names = set()
    for binding in spec['bindings']:
        name, kind, pcs = binding['name'], binding['kind'], binding['pcs']
        require(name not in names and pcs, 'duplicate/empty binding')
        names.add(name)
        words = [int.from_bytes(raw(pc, 4), 'big') for pc in pcs]
        require(words == binding['words'] and all(pc in owned for pc in pcs), name + ' instruction mismatch')
        if kind == 'address':
            hi, lo = words
            require(hi >> 26 == 15 and lo >> 26 == 35 and
                    (hi >> 16) & 31 == (lo >> 21) & 31, name + ' invalid global load')
            value = ((hi & 65535) << 16) + ((lo & 32767) - (lo & 32768))
        elif kind == 'field':
            require(len(words) == 1 and words[0] >> 26 in (35, 40, 49), name + ' invalid field')
            value = words[0] & 65535
        elif kind == 'literal':
            require(len(words) == 1 and words[0] >> 26 == 9 and not (words[0] >> 21) & 31,
                    name + ' invalid literal')
            value = words[0] & 65535
        elif kind == 'upper_bits':
            require(len(words) == 1 and words[0] >> 26 == 15, name + ' invalid float literal')
            value = (words[0] & 65535) << 16
        elif kind == 'return':
            require(len(words) == 2 and words[0] >> 26 == 3 and pcs[1] == pcs[0] + 4,
                    name + ' invalid call/delay pair')
            require((words[0] & 0x3ffffff) << 2 == 0x38c0, name + ' wrong perspective target')
            value = pcs[0] + 8
        else:
            raise RecompError('Rayman 2 controls: unknown binding kind')
        require(value == binding['value'], name + ' decoded operand mismatch')
    for wrapper in profile['function_wrappers']:
        matches = [f for f in spec['functions'] if f['vram'] == wrapper['vram']]
        require(len(matches) == 1 and all(matches[0][k] == wrapper[k] for k in ('size', 'sha256')),
                'wrapper evidence mismatch')
    return dict(verification='static ROM evidence', runtime_verified=False,
                rom_sha1=profile['game']['rom_sha1'], reference=spec['reference'],
                reference_revision=spec['reference_revision'], reference_scope=spec['reference_scope'],
                functions=spec['functions'], bindings=spec['bindings'],
                metadata_verified=metadata is not None, coverage_verified=coverage is not None)
