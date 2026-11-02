"""Static ROM evidence for Hexen's world projection and visibility adapter."""
import hashlib
import struct
from common import RecompError


def verify_fov(rom, profile, metadata=None, coverage=None):
    spec = profile['fov_adapter']

    def require(condition, message):
        if not condition:
            raise RecompError('Hexen FOV: ' + message)

    def raw(address, size):
        regions = [r for r in profile['discovery']['regions']
                   if r['vram'] <= address and address + size <= r['vram'] + r['size']]
        require(len(regions) == 1 and size > 0, 'unmapped evidence')
        offset = regions[0]['rom_offset'] + address - regions[0]['vram']
        require(offset + size <= len(rom), 'evidence outside image')
        return rom[offset:offset + size]

    owned = set()
    for body in spec['functions']:
        require(hashlib.sha256(raw(body['vram'], body['size'])).hexdigest() == body['sha256'],
                body['name'] + ' fingerprint mismatch')
        if metadata is not None:
            found = [f for f in metadata['functions'] if f['vram'] == body['vram']]
            require(len(found) == 1 and found[0]['size'] == body['size'] and
                    found[0]['sha256'] == body['sha256'], body['name'] + ' boundary mismatch')
            owned.update(found[0]['instruction_addresses'])
            if coverage is not None:
                statuses = [f['status'] for f in coverage['functions'] if
                            f['vram'] == body['vram'] and f['section'] == found[0]['section']]
                require(statuses == ['ready'], body['name'] + ' is not translated')
        else:
            owned.update(range(body['vram'], body['vram'] + body['size'], 4))
    names = set()
    for binding in spec['bindings']:
        name, kind, pcs = binding['name'], binding['kind'], binding['pcs']
        require(name not in names and pcs, name + ' duplicate/empty binding')
        names.add(name)
        words = [int.from_bytes(raw(pc, 4), 'big') for pc in pcs]
        require(words == binding['words'], name + ' evidence mismatch')
        require(kind == 'data_word' or all(pc in owned for pc in pcs), name + ' unowned instructions')
        if kind == 'address':
            hi, low = words
            require(hi >> 26 == 15 and low >> 26 in (9, 43, 49) and
                    (hi >> 16) & 31 == (low >> 21) & 31, name + ' invalid address')
            value = ((hi & 0xffff) << 16) + ((low & 0x7fff) - (low & 0x8000))
        elif kind == 'return':
            require(len(words) == 2 and words[0] >> 26 == 3 and pcs[1] == pcs[0] + 4,
                    name + ' invalid call')
            target = ((pcs[0] + 4) & 0xf0000000) | ((words[0] & 0x3ffffff) << 2)
            require(any(w['vram'] == target for w in profile['function_wrappers']), name + ' wrong wrapper')
            value = pcs[0] + 8
        elif kind in ('upper_bits', 'double_high'):
            require(len(words) == 1 and words[0] >> 26 == 15, name + ' invalid constant')
            value = (words[0] & 0xffff) << 16
            if kind == 'double_high':
                value = struct.unpack('>d', ((words[0] & 0xffff) << 48).to_bytes(8, 'big'))[0]
        elif kind == 'literal_bits':
            hi, low = words
            require(hi >> 26 == 15 and low >> 26 == 13 and
                    (hi >> 16) & 31 == (low >> 21) & 31 == (low >> 16) & 31,
                    name + ' invalid literal')
            value = ((hi & 0xffff) << 16) | (low & 0xffff)
        elif kind == 'data_word':
            require(len(words) == 1, name + ' invalid scalar')
            value = words[0]
        else:
            raise RecompError('Hexen FOV: unknown binding kind')
        require(value == binding['value'], name + ' decoded operand mismatch')
    return dict(verification='static ROM evidence', runtime_verified=False,
                analysis_sha1=hashlib.sha1(rom).hexdigest(), functions=spec['functions'],
                bindings=spec['bindings'], metadata_verified=metadata is not None,
                coverage_verified=coverage is not None)
