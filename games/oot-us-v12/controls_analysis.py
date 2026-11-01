"""Checked NTSC 1.2 camera/input evidence; public names are semantic references."""
import hashlib
from common import RecompError


def verify_controls(rom, profile, metadata=None, coverage=None):
    spec = profile['controls']

    def require(ok, message):
        if not ok:
            raise RecompError('OoT controls: ' + message)

    def raw(address, size):
        region = [r for r in profile['discovery']['regions'] if r['name'] == '.code' and
                  r['vram'] <= address and address + size <= r['vram'] + r['size']]
        require(len(region) == 1, 'unmapped code evidence')
        offset = region[0]['rom_offset'] + address - region[0]['vram']
        return rom[offset:offset + size]

    owned = set()
    for body in spec['functions']:
        require(hashlib.sha256(raw(body['vram'], body['size'])).hexdigest() == body['sha256'],
                body['name'] + ' fingerprint mismatch')
        if metadata:
            sections = {s['id']: s['name'] for s in metadata['sections']}
            matches = [f for f in metadata['functions'] if f['vram'] == body['vram'] and
                       sections[f['section']] == '.code']
            require(len(matches) == 1 and matches[0]['size'] == body['size'] and
                    matches[0]['sha256'] == body['sha256'], body['name'] + ' metadata mismatch')
            owned.update(matches[0]['instruction_addresses'])
            if coverage:
                statuses = [f['status'] for f in coverage['functions'] if f['vram'] == body['vram'] and
                            f['section'] == matches[0]['section']]
                require(statuses == ['ready'], body['name'] + ' original not translated')
        else:
            owned.update(range(body['vram'], body['vram'] + body['size'], 4))
    for binding in spec['bindings']:
        words = [int.from_bytes(raw(pc, 4), 'big') for pc in binding['pcs']]
        require(words == binding['words'] and all(pc in owned for pc in binding['pcs']),
                binding['name'] + ' instruction mismatch')
        kind = binding['kind']
        if kind == 'field':
            require(len(words) == 1 and words[0] >> 26 in (9, 33, 35, 36, 37, 40, 41, 43, 49, 57),
                    'invalid field access')
            value = words[0] & 0xffff
        elif kind == 'large_field':
            require(len(words) == 2 and words[0] >> 26 == 15 and words[1] >> 26 in (36, 37),
                    'invalid wide field access')
            value = ((words[0] & 65535) << 16) + ((words[1] & 32767) - (words[1] & 32768))
        elif kind == 'address':
            hi, lo = words
            require(hi >> 26 == 15 and lo >> 26 in (9, 33, 35), 'invalid global address')
            value = ((hi & 65535) << 16) + ((lo & 32767) - (lo & 32768))
        elif kind == 'return':
            require(len(words) == 2 and binding['pcs'][1] == binding['pcs'][0] + 4 and
                    (words[0] >> 26 == 3 or words[0] == 0x0320f809), 'invalid call/delay pair')
            value = binding['pcs'][0] + 8
        else:
            raise RecompError('OoT controls: unknown binding kind')
        require(value == binding['value'], binding['name'] + ' decoded operand mismatch')
    tables = [t for t in profile['discovery']['function_tables'] if t['pc'] == spec['camera_dispatch_pc']]
    require(len(tables) == 1, 'missing checked camera dispatch')
    table = tables[0]
    data = raw(table['table_vram'], table['count'] * 4)
    require(hashlib.sha256(data).hexdigest() == table['sha256'] and
            int.from_bytes(data[8:12], 'big') == spec['normal_camera'], 'camera dispatch table mismatch')
    settings = spec['settings_table']
    require(hashlib.sha256(raw(settings['vram'], settings['count'] * 8)).hexdigest() == settings['sha256'],
            'camera settings table mismatch')
    fov = spec['native_fov']
    require(raw(fov['vram'], 4) == fov['value'].to_bytes(2, 'big') + fov['tag'].to_bytes(2, 'big'),
            'native normal-camera FOV parameter mismatch')
    return dict(verification='static prepared-ROM evidence', runtime_verified=False,
                reference=spec['reference'], reference_revision=spec['reference_revision'],
                rom_sha1=profile['game']['rom_sha1'], functions=spec['functions'], bindings=spec['bindings'],
                camera_table=table, settings_table=settings, native_fov=fov,
                metadata_verified=metadata is not None, coverage_verified=coverage is not None)
