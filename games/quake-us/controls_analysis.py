"""ROM-bound Quake 64 command/projection evidence, used during generation."""
import hashlib
import struct
from common import RecompError


def verify_controls(rom, profile, metadata=None, coverage=None):
    spec = profile['controls']

    def require(ok, message):
        if not ok:
            raise RecompError('Quake controls: ' + message)

    def raw(address, size):
        regions = [r for r in profile['discovery']['regions'] if
                   r['vram'] <= address and address + size <= r['vram'] + r['size']]
        require(len(regions) == 1, 'unmapped ROM evidence')
        offset = regions[0]['rom_offset'] + address - regions[0]['vram']
        return rom[offset:offset + size]

    owned = set()
    for body in spec['functions']:
        require(hashlib.sha256(raw(body['vram'], body['size'])).hexdigest() == body['sha256'],
                body['name'] + ' fingerprint mismatch')
        if metadata is not None:
            matches = [f for f in metadata['functions'] if f['vram'] == body['vram']]
            require(len(matches) == 1 and matches[0]['size'] == body['size'] and
                    matches[0]['sha256'] == body['sha256'], body['name'] + ' boundary mismatch')
            owned.update(matches[0]['instruction_addresses'])
            if coverage is not None:
                statuses = [f['status'] for f in coverage['functions'] if f['vram'] == body['vram'] and
                            f['section'] == matches[0]['section']]
                require(statuses == ['ready'], body['name'] + ' original not translated')
        else:
            owned.update(range(body['vram'], body['vram'] + body['size'], 4))
    for binding in spec['bindings']:
        pcs, kind = binding['pcs'], binding['kind']
        words = [int.from_bytes(raw(pc, 4), 'big') for pc in pcs]
        require(words == binding['words'] and
                (kind in ('data_word', 'data_double') or all(pc in owned for pc in pcs)),
                binding['name'] + ' instruction/data mismatch')
        if kind == 'address':
            hi, lo = words
            require(hi >> 26 == 15 and lo >> 26 in (9, 35, 43) and
                    (hi >> 16) & 31 == (lo >> 21) & 31, 'invalid address materialization')
            value = ((hi & 65535) << 16) + ((lo & 32767) - (lo & 32768))
        elif kind == 'field':
            require(len(words) == 1 and words[0] >> 26 in (9, 33, 35, 41, 43, 49, 57),
                    'invalid field operand')
            value = words[0] & 65535
        elif kind == 'return':
            require(len(words) == 2 and words[0] >> 26 == 3 and pcs[1] == pcs[0] + 4,
                    'invalid call/delay pair')
            value = pcs[0] + 8
        elif kind in ('upper_bits', 'double_upper'):
            require(len(words) == 1 and words[0] >> 26 == 15, 'invalid literal')
            value = (words[0] & 65535) << 16
            if kind == 'double_upper':
                value = struct.unpack('>d', ((words[0] & 65535) << 48).to_bytes(8, 'big'))[0]
        elif kind == 'data_word':
            require(len(words) == 1, 'invalid word literal')
            value = words[0]
        elif kind == 'data_double':
            require(len(words) == 2 and pcs[1] == pcs[0] + 4, 'invalid double literal')
            value = struct.unpack('>d', raw(pcs[0], 8))[0]
        else:
            raise RecompError('Quake controls: unknown binding kind')
        require(value == binding['value'], binding['name'] + ' decoded value mismatch')
    return dict(verification='static ROM evidence', runtime_verified=False,
                rom_sha1=profile['game']['rom_sha1'], reference=spec['reference'],
                reference_revision=spec['reference_revision'], reference_scope=spec['reference_scope'],
                functions=spec['functions'], bindings=spec['bindings'],
                metadata_verified=metadata is not None, coverage_verified=coverage is not None)
