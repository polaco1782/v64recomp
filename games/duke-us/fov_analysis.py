"""ROM-bound evidence for Duke's CPU/GPU projections and sector scan window."""
import hashlib
from common import RecompError


def verify_fov(rom, profile, metadata=None, coverage=None):
    spec = profile['fov_adapter']

    def require(condition, message):
        if not condition:
            raise RecompError('Duke FOV: ' + message)

    require(len(rom) == profile['game']['rom_size'] and
            hashlib.sha1(rom).hexdigest() == profile['game']['rom_sha1'], 'ROM identity mismatch')

    def raw(address, size):
        regions = [r for r in profile['discovery']['regions']
                   if r['vram'] <= address and address + size <= r['vram'] + r['size']]
        require(len(regions) == 1 and address % 4 == 0 and size > 0, 'unmapped evidence')
        offset = regions[0]['rom_offset'] + address - regions[0]['vram']
        require(0 <= offset and offset + size <= len(rom), 'evidence outside ROM')
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
        require(words == binding['words'] and all(pc in owned for pc in pcs), name + ' instruction mismatch')
        if kind == 'return':
            require(len(words) == 2 and words[0] >> 26 == 3 and pcs[1] == pcs[0] + 4,
                    name + ' invalid call/delay pair')
            target = ((pcs[0] + 4) & 0xf0000000) | ((words[0] & 0x3ffffff) << 2)
            require(target == binding['target'] and any(f['vram'] == target for f in spec['functions']),
                    name + ' wrong target')
            value = pcs[0] + 8
        elif kind == 'address':
            hi, low = words
            require(hi >> 26 == 15 and low >> 26 == 33 and
                    (hi >> 16) & 31 == (low >> 21) & 31, name + ' invalid LUI/LH address')
            value = ((hi & 0xffff) << 16) + ((low & 0x7fff) - (low & 0x8000))
        elif kind == 'upper_bits':
            require(len(words) == 1 and words[0] >> 26 == 15, name + ' invalid LUI constant')
            value = (words[0] & 0xffff) << 16
        elif kind == 'literal_bits':
            hi, low = words
            require(hi >> 26 == 15 and low >> 26 == 13 and
                    (hi >> 16) & 31 == (low >> 21) & 31 == (low >> 16) & 31,
                    name + ' invalid LUI/ORI constant')
            value = ((hi & 0xffff) << 16) | (low & 0xffff)
        elif kind == 'saved_ra':
            require(len(words) == 1 and words[0] >> 16 == 0xafbf, name + ' invalid SW ra, offset(sp)')
            value = words[0] & 0xffff
        else:
            raise RecompError('Duke FOV: unknown binding kind')
        require(value == binding['value'], name + ' decoded operand mismatch')

    # Every guPerspective caller must stay visible in the report. The wrapper
    # also checks its saved parent return address before changing the GPU path.
    callers = []
    if metadata:
        target = next(f['vram'] for f in spec['functions'] if f['name'] == 'guPerspective')
        for function in metadata['functions']:
            for pc in function['instruction_addresses']:
                word = int.from_bytes(raw(pc, 4), 'big')
                if word >> 26 == 3 and (((pc + 4) & 0xf0000000) | ((word & 0x3ffffff) << 2)) == target:
                    callers.append(dict(owner=function['name'], pc=pc))
    return dict(verification='static ROM evidence', runtime_verified=False,
                analysis_sha1=hashlib.sha1(rom).hexdigest(), reference=spec['reference'],
                reference_revision=spec['reference_revision'], functions=spec['functions'],
                bindings=spec['bindings'], metadata_verified=metadata is not None,
                coverage_verified=coverage is not None, perspective_callers=callers)
