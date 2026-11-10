"""Checked retail input, projection and CPU visibility operands for generation."""
import hashlib
import struct
from common import RecompError


def verify_controls(rom, profile, metadata=None, coverage=None):
    spec = profile['controls']

    def require(ok, message):
        if not ok:
            raise RecompError('Turok controls: ' + message)

    def raw(address, size):
        regions = [r for r in profile['discovery']['regions'] if
                   r['vram'] <= address and address + size <= r['vram'] + r['size']]
        require(len(regions) == 1 and size > 0, 'unmapped evidence span')
        offset = regions[0]['rom_offset'] + address - regions[0]['vram']
        require(offset + size <= len(rom), 'evidence outside prepared ROM')
        return rom[offset:offset + size]

    owned = set()
    for body in spec['functions']:
        require(hashlib.sha256(raw(body['vram'], body['size'])).hexdigest() == body['sha256'],
                body['name'] + ' fingerprint mismatch')
        if metadata is None:
            owned.update(range(body['vram'], body['vram'] + body['size'], 4))
            continue
        matches = [f for f in metadata['functions'] if f['section'] == 1 and f['vram'] == body['vram']]
        require(len(matches) == 1 and matches[0]['size'] == body['size'] and
                matches[0]['sha256'] == body['sha256'], body['name'] + ' boundary mismatch')
        owned.update(matches[0]['instruction_addresses'])
        if coverage is not None:
            statuses = [f['status'] for f in coverage['functions'] if f['section'] == 1 and f['vram'] == body['vram']]
            require(statuses == ['ready'], body['name'] + ' original not translated')
    values = {}
    for binding in spec['bindings']:
        name, pcs, kind = binding['name'], binding['pcs'], binding['kind']
        require(name not in values and all(pc in owned for pc in pcs), name + ' duplicate/unowned evidence')
        words = [int.from_bytes(raw(pc, 4), 'big') for pc in pcs]
        require(words == binding['words'], name + ' instruction mismatch')
        signed = lambda word: (word & 0x7fff) - (word & 0x8000)
        if kind == 'address':
            hi, lo = words
            require(hi >> 26 == 15 and lo >> 26 in (9, 33, 35, 49) and
                    (hi >> 16) & 31 == (lo >> 21) & 31, name + ' invalid address')
            value = ((hi & 65535) << 16) + signed(lo)
        elif kind == 'field':
            require(len(words) == 1 and words[0] >> 26 in (9, 32, 33, 35, 36, 37, 43, 49, 57), name + ' invalid field')
            value = words[0] & 65535
        elif kind == 'offset':
            hi, lo, access = words
            require(hi >> 26 == 15 and lo >> 26 == 13 and
                    (hi >> 16) & 31 == (lo >> 21) & 31 and
                    access >> 26 in (35, 49), name + ' invalid large field')
            value = ((hi & 65535) << 16 | (lo & 65535)) + signed(access)
        elif kind == 'offset_pair':
            add, access = words
            require(add >> 26 == 9 and access >> 26 == 35 and
                    (add >> 16) & 31 == (access >> 21) & 31, name + ' invalid player field')
            value = signed(add) + signed(access)
        elif kind == 'return':
            require(len(words) == 2 and words[0] >> 26 == 3 and pcs[1] == pcs[0] + 4, name + ' invalid call/delay')
            target = ((pcs[0] + 4) & 0xf0000000) | ((words[0] & 0x3ffffff) << 2)
            require(any(w['vram'] == target for w in profile['function_wrappers']), name + ' wrong wrapper target')
            value = pcs[0] + 8
        elif kind in ('upper_bits', 'float_upper'):
            require(len(words) == 1 and words[0] >> 26 == 15, name + ' invalid literal')
            value = (words[0] & 65535) << 16
            if kind == 'float_upper':
                value = struct.unpack('>f', value.to_bytes(4, 'big'))[0]
        elif kind == 'frame_size':
            require(len(words) == 1 and words[0] >> 16 == 0x27bd, name + ' invalid stack frame')
            value = -signed(words[0])
        elif kind == 'derived_address':
            # The checked LUI/ORI constructs the offset added to engine_app.
            hi, lo = words
            require(hi >> 26 == 15 and lo >> 26 == 13 and
                    (hi >> 16) & 31 == (lo >> 21) & 31, name + ' invalid relative address')
            value = values[binding['base']] + ((hi & 65535) << 16 | (lo & 65535))
        else:
            raise RecompError('Turok controls: unknown binding kind')
        require(value == binding['value'], name + ' decoded operand mismatch')
        values[name] = value
    return dict(verification='static ROM evidence', runtime_verified=False,
                rom_sha1=profile['game']['rom_sha1'], reference=spec['reference'],
                reference_revision=spec['reference_revision'], reference_scope=spec['reference_scope'],
                functions=spec['functions'], bindings=spec['bindings'],
                metadata_verified=metadata is not None, coverage_verified=coverage is not None)
