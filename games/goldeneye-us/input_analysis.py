"""ROM-bound evidence for the input adapter; verification does not alter code."""
from __future__ import annotations

import hashlib

from common import RecompError


def verify_input_analysis(rom: bytes, profile: dict, metadata: dict | None = None,
                          coverage: dict | None = None) -> dict:
    """Check complete code spans, call/delay pairs, and decoded data operands.

    Semantic names come from reviewed disassembly and the pinned reference.
    Hashes bind that review to these bytes; they do not prove live behavior.
    metadata, when supplied, must have passed common.load_metadata first.
    """
    spec = profile['input_analysis']
    game = profile['game']
    if len(rom) != game['rom_size'] or hashlib.sha1(rom).hexdigest() != game['rom_sha1']:
        raise RecompError('GoldenEye input analysis: ROM identity mismatch')

    def require(ok: bool, message: str) -> None:
        if not ok:
            raise RecompError(f'GoldenEye input analysis: {message}')

    def raw(section: str, address: int, size: int) -> bytes:
        regions = [r for r in profile['discovery']['regions'] if r['name'] == section
                   and r['vram'] <= address and address + size <= r['vram'] + r['size']]
        require(type(address) is int and type(size) is int and address % 4 == 0
                and size > 0 and size % 4 == 0 and len(regions) == 1,
                f'invalid mapped range {section} {address:#x}+{size:#x}')
        region = regions[0]
        offset = region['rom_offset'] + address - region['vram']
        require(0 <= offset and offset + size <= len(rom), 'range outside ROM')
        return rom[offset:offset + size]

    functions = {}
    entries = {}
    sections = {s['id']: s['name'] for s in metadata['sections']} if metadata else {}
    for item in spec['functions']:
        name = item['name']
        require(name not in functions, f'duplicate function {name}')
        require(not any((f['section'], f['vram']) == (item['section'], item['vram'])
                        for f in functions.values()), f'duplicate entry {name}')
        body = raw(item['section'], item['vram'], item['size'])
        require(hashlib.sha256(body).hexdigest() == item['sha256'], f'{name} body fingerprint mismatch')
        functions[name] = item
        if metadata:
            matches = [f for f in metadata['functions'] if f['vram'] == item['vram']
                       and sections[f['section']] == item['section']]
            require(len(matches) == 1, f'{name} missing or ambiguous in metadata')
            entry = matches[0]
            require(entry['size'] == item['size'] and entry['sha256'] == item['sha256'],
                    f'{name} metadata span/fingerprint mismatch')
            entries[name] = entry
        if coverage is not None:
            require(metadata is not None, 'coverage requires verified metadata')
            entry = entries[name]
            matches = [f for f in coverage['functions'] if f['vram'] == entry['vram']
                       and f['section'] == entry['section']]
            require(len(matches) == 1 and matches[0]['status'] == 'ready',
                    f'{name} is not translated original code')

    def words(owner: str, pc: int, expected: list[int]) -> list[int]:
        require(owner in functions, f'unknown evidence owner {owner}')
        item = functions[owner]
        size = len(expected) * 4
        require(size > 0 and item['vram'] <= pc and pc + size <= item['vram'] + item['size'],
                f'{owner} evidence outside function')
        body = raw(item['section'], pc, size)
        actual = [int.from_bytes(body[i:i + 4], 'big') for i in range(0, size, 4)]
        require(actual == expected, f'{owner} instruction mismatch at {pc:#x}')
        if metadata:
            owned = set(entries[owner]['instruction_addresses'])
            require(all(address in owned for address in range(pc, pc + size, 4)),
                    f'{owner} evidence includes unowned instructions at {pc:#x}')
        return actual

    for call in spec['calls']:
        pair = words(call['owner'], call['pc'], call['expected_words'])
        require(len(pair) == 2 and pair[0] >> 26 == 3, 'expected JAL and its delay slot')
        target = ((call['pc'] + 4) & 0xf0000000) | ((pair[0] & 0x3ffffff) << 2)
        require(target == call['target'], f"call target mismatch at {call['pc']:#x}")
        if metadata:
            targets = [f for f in metadata['functions'] if f['vram'] == target]
            require(len(targets) == 1, f'call target missing or ambiguous at {target:#x}')

    pointer = spec['current_player']
    hi, lo, deref = words(pointer['owner'], pointer['pc'], pointer['expected_words'])
    reg = (hi >> 16) & 31
    signed_low = (lo & 65535) - (65536 if lo & 0x8000 else 0)
    require(hi >> 26 == 15 and lo >> 26 == 9 and (lo >> 21) & 31 == reg
            and (lo >> 16) & 31 == reg and deref >> 26 == 35
            and (deref >> 21) & 31 == reg and deref & 65535 == 0,
            'current-player pointer must be LUI/ADDIU/LW')
    require(((hi & 65535) << 16) + signed_low == pointer['address'],
            'current-player global does not match decoded address')

    field_names = set()
    for field in spec['player_fields']:
        require(field['name'] not in field_names, f"duplicate field {field['name']}")
        field_names.add(field['name'])
        word, = words(field['owner'], field['pc'], [field['expected_word']])
        # These are reviewed player-relative LW/LWC1/SWC1 instructions, not host offsets.
        require(word >> 26 in (35, 49, 57) and (word >> 21) & 31 != 29
                and (word & 65535) == field['offset'] and 0 <= field['offset'] < 0x8000,
                f"{field['name']} is not the declared player-relative access")

    adapter = profile.get('input_adapter')
    if adapter:
        require(any(c['owner'] == 'MoveBond' and c['target'] == functions['bondviewProcessInput']['vram']
                    and c['pc'] + 8 == adapter['process_return'] for c in spec['calls']),
                'input adapter return is not the verified MoveBond call')
        for state in adapter['globals']:
            code = words(state['owner'], state['pc'], state['expected_words'])
            hi, low = code[0], code[-1]
            signed_low = (low & 65535) - (65536 if low & 0x8000 else 0)
            require((len(code) == 2 or (len(code) == 3 and code[1] == 0x03e00008))
                    and hi >> 26 == 15 and low >> 26 == 35 and (low >> 21) & 31 == (hi >> 16) & 31
                    and ((hi & 65535) << 16) + signed_low == state['address'],
                    f"{state['name']} address does not match the decoded global")

    # Expose every owned direct caller of the candidates, not just our chosen site.
    # This makes frozen/menu callers visible before someone enables a global hook.
    callers = []
    if metadata:
        names = {f['vram']: name for name, f in functions.items()}
        for entry in metadata['functions']:
            owned = set(entry['instruction_addresses'])
            for pc in entry['instruction_addresses']:
                word = int.from_bytes(raw(sections[entry['section']], pc, 4), 'big')
                target = ((pc + 4) & 0xf0000000) | ((word & 0x3ffffff) << 2)
                if word >> 26 == 3 and target in names:
                    callers.append(dict(owner=entry['name'], owner_vram=entry['vram'], pc=pc,
                                        target=names[target], delay_slot_owned=pc + 4 in owned))

    return dict(format_version=1, verification='static ROM evidence', runtime_verified=False,
                replacements_installed=False, rom_sha1=game['rom_sha1'], reference=spec['reference'],
                reference_revision=spec['reference_revision'], functions=spec['functions'],
                calls=spec['calls'], current_player=pointer, player_fields=spec['player_fields'],
                metadata_verified=metadata is not None, coverage_verified=coverage is not None,
                direct_callers=callers, adapter=adapter, function_wrappers=profile.get('function_wrappers', []))
