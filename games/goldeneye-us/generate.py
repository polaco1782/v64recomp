"""Game-specific inputs for CMake source generation."""
from pathlib import Path
import hashlib, json, math, struct, zlib
import re
from common import RecompError
from input_analysis import verify_input_analysis


def startup_data(rom: bytes, boot: dict) -> bytes:
    """Independent bounded oracle for the original MIPS inflater, not game code."""
    offset, size, expected = (boot[k] for k in
                              ('compressed_data_offset', 'compressed_data_size', 'data_size'))
    if (type(offset) is not int or type(size) is not int or type(expected) is not int
            or offset < 0 or size < 3 or offset + size > len(rom) or not 0 < expected <= 0x400000):
        raise RecompError('invalid GoldenEye compressed startup data range')
    block = rom[offset:offset + size]
    if block[:2] != b'\x11\x72':
        raise RecompError('GoldenEye startup data has no RareZip 1172 header')
    decoder = zlib.decompressobj(-15)
    try:
        result = decoder.decompress(block[2:], expected + 1)
    except zlib.error as error:
        raise RecompError(f'invalid GoldenEye startup deflate stream: {error}') from error
    if not decoder.eof or len(result) != expected:
        raise RecompError('GoldenEye startup data is truncated or has an unexpected decoded size')
    if hashlib.sha256(result).hexdigest() != boot['data_sha256']:
        raise RecompError('GoldenEye decoded startup data fingerprint mismatch')
    return result


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    if (boot['initial_rom_offset'] < 0 or boot['initial_load_size'] <= 0
            or boot['initial_rom_offset'] + boot['initial_load_size'] > game['rom_size']
            or not 0x80000000 <= boot['initial_load_address'] < 0xa0000000
            or (boot['initial_load_address'] & 0x1fffffff) + boot['initial_load_size'] > game['ram_size']
            or not 0x80000000 <= boot['bss_start'] < boot['bss_end'] <= 0x80000000 + game['ram_size']):
        raise RecompError('invalid GoldenEye bootstrap RAM/ROM mapping')
    inflater = next(s for s in metadata['sections'] if s['name'] == '.inflate')
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size'],
                 'INFLATE_ROM_OFFSET': inflater['rom_offset'], 'INFLATE_SIZE': inflater['size'],
                 'INFLATE_SECTION': inflater['id'], 'INFLATE_VRAM': inflater['vram']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    lines = ['/* Generated from the pinned GoldenEye ROM profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' +
                 ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},'
                 for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    if 'pager' in profile:
        lines.append('static const GoldenEyePagerConfig ge_pager_config = {')
        lines.extend(f'    .{name}=UINT32_C(0x{value:x}),' for name,value in profile['pager'].items())
        lines.extend(['};', 'static const RecompPlatformConfig ge_platform_config = {'])
        lines.extend(f'    .{name}=UINT32_C(0x{value:x}),' for name,value in profile['platform'].items())
        lines.extend(['    .vi_period_ticks=BOOT_VI_PERIOD_TICKS,', '};'])
        names = profile['runtime'].get('thread_names', [])
        entries = {function['vram'] for function in metadata['functions']}
        seen = set()
        if names:
            lines.append('static const RecompThreadName ge_thread_names[] = {')
            for item in names:
                entry, name = item['entry'], item['name']
                if (type(entry) is not int or entry not in entries or entry in seen
                        or not isinstance(name, str) or not re.fullmatch(r'n64-[a-z0-9-]{1,11}', name)):
                    raise RecompError('invalid or duplicate GoldenEye thread name/entry')
                seen.add(entry)
                lines.append(f'    {{UINT32_C(0x{entry:x}), {json.dumps(name)}}},')
            lines.append('};')
        lines.append('static const RecompSchedulerConfig ge_scheduler_config = {')
        lines.extend(f'    .{name}=UINT32_C(0x{value:x}),' for name,value in profile['runtime'].items()
                     if name != 'thread_names')
        if names:
            lines.extend(['    .thread_names=ge_thread_names,',
                          '    .thread_name_count=sizeof(ge_thread_names)/sizeof(ge_thread_names[0]),'])
        lines.extend(['};', ''])
        game_section=next(s for s in metadata['sections'] if s['name']=='.game')
        lines.append(f'#define BOOT_GAME_SECTION {game_section["id"]}')
    for name,value in profile.get('inspection',{}).items():
        if type(value) is int:lines.append(f'#define GE_INSPECT_{name.upper()} UINT32_C(0x{value:x})')
    (output / 'game_config.h').write_text('\n'.join(lines))
    analysis = profile['input_analysis']
    constants = {'CURRENT_PLAYER': analysis['current_player']['address'],
                 'PROCESS_RETURN': profile['input_adapter']['process_return'],
                 'DUAL_STYLE': profile['input_adapter']['dual_style'],
                 'STAGE_ADDRESS': profile['inspection']['stage_address'],
                 'TITLE_STAGE': profile['inspection']['title_stage']}
    constants.update({f['name'].upper(): f['offset'] for f in analysis['player_fields']})
    constants.update({g['name'].upper(): g['address'] for g in profile['input_adapter']['globals']})
    for name, target in [('X', 0x7000c0ec), ('Y', 0x7000c1fc), ('BUTTONS', 0x7000c30c)]:
        call = next(c for c in analysis['calls'] if c['owner'] == 'bondviewProcessInput' and c['target'] == target)
        constants[f'SECONDARY_{name}_RETURN'] = call['pc'] + 8
    angle = next(f for f in analysis['functions'] if f['name'] == 'bondviewApplyVertaTheta')
    entry = next(f for f in metadata['functions'] if f['vram'] == angle['vram'])
    lines = ['/* Verified GoldenEye input addresses; generated from profile.toml. */']
    lines.extend(f'#define GE_INPUT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append(f'#define GE_INPUT_ORIGINAL_ANGLES recomp_s{entry["section"]}_{entry["vram"]:08x}')
    (output / 'goldeneye_input_config.h').write_text('\n'.join(lines) + '\n')
    fov = profile['fov_analysis']
    constants = {'NATIVE_BITS': int.from_bytes(struct.pack('>f', fov['base_fovy']['value']), 'big'),
                 'STAGE_ADDRESS': profile['inspection']['stage_address'],
                 'TITLE_STAGE': profile['inspection']['title_stage']}
    constants.update({f'{c["name"].upper()}_RETURN': c['pc'] + 8 for c in fov['calls']
                      if c['name'] in ('viewport', 'zoom', 'render', 'player_aspect_wide', 'player_aspect_normal',
                                       'vi_aspect_wide', 'vi_aspect_normal')})
    constants.update({f['name'].upper(): f['offset'] for f in fov['player_fields']})
    lines = ['/* Verified GoldenEye FOV addresses; generated from profile.toml. */']
    lines.extend(f'#define GE_FOV_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    (output / 'goldeneye_fov_config.h').write_text('\n'.join(lines) + '\n')
    ai = verify_ai_analysis(Path(game['rom']).read_bytes(), profile, metadata)
    lines = ['/* ROM-checked GoldenEye combat AI ABI; generated from profile.toml. */']
    lines.extend(f'#define GE_AI_{name.upper()} UINT32_C(0x{value:x})' for name, value in ai.items())
    models = profile['ai_analysis']['scenery_models']
    lines.append('static const uint16_t ge_ai_scenery_models[] = {' + ','.join(str(i) for i in models) + '};')
    # Native definitions control both damage/collision radius and rendered size.
    # Scale spatial growth with the radius while keeping the original timing.
    data = startup_data(Path(game['rom']).read_bytes(), boot)
    block = next(b for b in profile['ai_analysis']['data'] if b['name'] == 'explosion_types')
    lines.append('static const uint32_t ge_explosion_changes[][3] = {')
    for index in range(21):
        address = block['address'] + index * 64
        row = data[address - boot['data_address']:address - boot['data_address'] + 64]
        if struct.unpack_from('>f', row, ai['EXPLOSION_DAMAGE'])[0] <= 0:
            continue # Bullet impacts and debris-only effects keep their timing.
        for name, scale in [
            ('hrange', 3), ('vrange', 3), ('hchange', 3), ('vchange', 3),
            ('size', 3), ('range', 3), ('damage_range', 3),
            # ('flare_speed', 5), # Disabled: keep the original fade timing.
        ]:
            offset = ai['EXPLOSION_' + name.upper()]
            value = struct.unpack_from('>f', row, offset)[0]
            if not math.isfinite(value) or value < 0:
                raise RecompError('GoldenEye explosion: invalid native definition')
            before = struct.unpack_from('>I', row, offset)[0]
            after = int.from_bytes(struct.pack('>f', value * scale), 'big')
            if before != after:
                lines.append(f'    {{UINT32_C(0x{address + offset:x}), UINT32_C(0x{before:x}), UINT32_C(0x{after:x})}},')
        # Disabled: keep the original explosion duration.
        # offset = ai['EXPLOSION_DURATION']
        # duration, rate = struct.unpack_from('>hh', row, offset)
        # if not 0 < duration <= 32767 // 5 or rate <= 0:
        #     raise RecompError('GoldenEye explosion: invalid native lifetime')
        # before, after = duration << 16 | rate, duration * 5 << 16 | rate
        # lines.append(f'    {{UINT32_C(0x{address + offset:x}), UINT32_C(0x{before:x}), UINT32_C(0x{after:x})}},')
    lines.append('};')
    (output / 'goldeneye_ai_config.h').write_text('\n'.join(lines) + '\n')


def verify_ai_analysis(rom, profile, metadata=None):
    """Bind the native combat selector to complete bodies, operands and scripts."""
    spec = profile['ai_analysis']
    region = next(r for r in profile['discovery']['regions'] if r['name'] == spec['section'])

    def require(ok, message):
        if not ok:
            raise RecompError('GoldenEye AI: ' + message)

    def raw(address, size):
        offset = region['rom_offset'] + address - region['vram']
        require(region['vram'] <= address and address + size <= region['vram'] + region['size'],
                'code outside mapped section')
        return rom[offset:offset + size]

    require(len(rom) == profile['game']['rom_size'] and
            hashlib.sha1(rom).hexdigest() == profile['game']['rom_sha1'], 'ROM identity mismatch')
    constants = {'STAGE': profile['inspection']['stage_address']}
    owned = set()
    for function in spec['functions']:
        address, size = function['vram'], function['size']
        require(hashlib.sha256(raw(address, size)).hexdigest() == function['sha256'],
                function['name'] + ' body fingerprint mismatch')
        constants[function['name'].upper()] = address
        if metadata:
            sections = {s['id'] for s in metadata['sections'] if s['name'] == spec['section']}
            matches = [f for f in metadata['functions'] if f['vram'] == address and f['section'] in sections]
            require(len(matches) == 1 and matches[0]['size'] == size and
                    matches[0]['sha256'] == function['sha256'], function['name'] + ' metadata mismatch')
            owned.update(matches[0]['instruction_addresses'])

    def words(pc, expected):
        require(raw(pc, 4 * len(expected)) == b''.join(w.to_bytes(4, 'big') for w in expected),
                f'instruction mismatch at {pc:#x}')
        if metadata:
            require(set(range(pc, pc + 4 * len(expected), 4)) <= owned, 'unowned ABI evidence')

    for field in spec['fields']:
        word = field['expected_word']
        words(field['pc'], [word])
        require(word >> 26 in (32, 33, 35, 36, 37, 40, 41, 43, 49, 57) and
                (word >> 21) & 31 != 29, 'field evidence is not an object-relative memory operand')
        constants[field['name'].upper()] = word & 65535
    for global_ in spec['globals']:
        words(global_['pc'], global_['expected_words'])
        hi, lo = global_['expected_words']
        require(hi >> 26 == 15 and lo >> 26 == 9 and (hi >> 16) & 31 == (lo >> 21) & 31 and
                (lo >> 16) & 31 == (hi >> 16) & 31, 'global is not LUI/ADDIU')
        constants[global_['name'].upper()] = ((hi & 65535) << 16) + struct.unpack('>h', struct.pack('>H', lo & 65535))[0]
    words(spec['set_self_pc'], spec['set_self_words'])
    require(spec['set_self_words'] == [0x0fc0d4e6, 0x00009025] and
            constants['FIND_LIST'] == 0x7f035398, 'SetChrAiList self-call ABI mismatch')
    constants['SET_SELF_RETURN'] = spec['set_self_pc'] + 8
    words(spec['noise_pc'], spec['noise_words'])
    require(spec['noise_words'] == [0x0fc08bbb, 0xc7ac00e4] and constants['HEAR_SOUND'] == 0x7f022eec,
            'weapon-noise call/F12 argument mismatch')
    constants['NOISE_RETURN'] = spec['noise_pc'] + 8
    words(spec['noise_alert_pc'], spec['noise_alert_words'])
    require(spec['noise_alert_words'] == [0x0fc0a700, 0x02302021] and constants['ALERT'] == 0x7f029c00,
            'weapon-noise alert caller mismatch')
    constants['NOISE_ALERT_RETURN'] = spec['noise_alert_pc'] + 8
    words(spec['clock_pc'], spec['clock_words'])
    require(spec['clock_words'] == [0x3c0e8005, 0x58400009, 0x00001025, 0x8dce837c], 'clock operand mismatch')
    constants['CLOCK'] = 0x80050000 - 0x7c84
    words(spec['explosion_pc'], spec['explosion_words'])
    require(spec['explosion_words'] == [0x3c198004, 0xafb10020, 0x27390284, 0x000fc180],
            'explosion table pointer/64-byte stride mismatch')
    data = startup_data(rom, profile['bootstrap'])
    blocks = {}
    for block in spec['data']:
        offset = block['address'] - profile['bootstrap']['data_address']
        require(0 <= offset and offset + block['size'] <= len(data), 'data outside initialized RAM')
        body = data[offset:offset + block['size']]
        require(hashlib.sha256(body).hexdigest() == block['sha256'], block['name'] + ' data mismatch')
        blocks[block['name']] = (block['address'], body)
    require(blocks['explosion_types'][0] == 0x80040284 and len(blocks['explosion_types'][1]) == 21 * 64,
            'explosion type table bounds mismatch')
    table = blocks['global_lists'][1]
    require(len(table) == 19 * 8 and table[-8:] == bytes(8), 'global-list table bounds mismatch')
    for name, index in [('standard', 2), ('deaf', 5), ('simple', 7), ('attack', 6), ('chase', 8),
                        ('persistent', 13), ('draw', 16)]:
        pointer, identifier = struct.unpack_from('>II', table, index * 8)
        require(identifier == index and pointer == blocks[name][0], name + ' script selector mismatch')
        constants[name.upper() + '_LIST'] = pointer
    bodies = blocks['bodies'][1]
    require(len(bodies) == 43 * 20, 'body table bounds mismatch')
    for name, index, filename in [('scientist_female', 28, b'CtechwomanZ\0'),
                                  ('scientist_male', 35, b'CtechmanZ\0')]:
        pointer = struct.unpack_from('>I', bodies, index * 20 + 4)[0]
        offset = pointer - profile['bootstrap']['data_address']
        require(data[offset:offset + len(filename)] == filename, name + ' model mismatch')
        constants[name.upper() + '_BODY'] = index
    pointer = struct.unpack_from('>I', blocks['setup_names'][1], 34 * 4)[0]
    offset = pointer - profile['bootstrap']['data_address']
    facility_name = b'UsetuparkZ\0'
    require(data[offset:offset + len(facility_name)] == facility_name, 'Facility stage selector mismatch')
    constants['FACILITY_STAGE'] = 34
    models = spec['scenery_models']
    require(models and len(set(models)) == len(models) and all(type(i) is int and 0 <= i < 341 for i in models),
            'invalid scenery model allowlist')
    return constants


def verify_fov_analysis(rom, profile, metadata=None, coverage=None):
    report = verify_input_analysis(rom, profile, metadata, coverage,
                                   analysis_key='fov_analysis', adapter_key=None)
    spec = profile['fov_analysis']
    literal = spec['base_fovy']
    owner = next(f for f in spec['functions'] if f['name'] == literal['owner'])
    pc = literal['pc']
    region = next(r for r in profile['discovery']['regions'] if r['name'] == owner['section'])
    offset = region['rom_offset'] + pc - region['vram']
    code = [int.from_bytes(rom[offset+i:offset+i+4], 'big') for i in (0, 4)]
    # LUI at, immediate; MTC1 at, f12: the native viewport's vertical FOV.
    if (not owner['vram'] <= pc <= owner['vram'] + owner['size'] - 8
            or code != literal['expected_words'] or code[0] >> 16 != 0x3c01
            or code[1] != 0x44816000
            or struct.unpack('>f', ((code[0] & 65535) << 16).to_bytes(4, 'big'))[0] != literal['value']):
        raise RecompError('GoldenEye FOV: native vertical FOV literal mismatch')
    if metadata:
        entry = next(f for f in metadata['functions'] if f['vram'] == owner['vram'])
        if not {pc, pc + 4} <= set(entry['instruction_addresses']):
            raise RecompError('GoldenEye FOV: native FOV literal is not owned code')
    report['base_fovy'] = literal
    return report


def checked_dispatches(rom: bytes, profile: dict) -> None:
    """Resolve profile jump tables from the verified compressed startup data."""
    data=startup_data(rom,profile['bootstrap'])
    transfers=[]
    for table in profile['discovery'].get('jump_tables', []):
        address,count=table['table_vram'],table['count']
        if (type(address) is not int or type(count) is not int or address%4
                or not 0<count<=4096):
            raise RecompError('invalid GoldenEye checked dispatch table')
        offset=address-profile['bootstrap']['data_address']
        size=count*4
        if offset<0 or offset+size>len(data):
            raise RecompError('GoldenEye jump table outside initialized data')
        raw=data[offset:offset+size]
        if hashlib.sha256(raw).hexdigest()!=table['sha256']:
            raise RecompError(f'GoldenEye jump table fingerprint mismatch at {table["pc"]:#010x}')
        indices=table.get('indices',list(range(count)))
        if (not isinstance(indices,list) or not indices
                or any(type(i) is not int or not 0<=i<count for i in indices)
                or len(set(indices))!=len(indices)):
            raise RecompError('invalid GoldenEye checked dispatch table indices')
        transfers.append(dict(pc=table['pc'],expected_word=table['expected_word'],kind='jump',
            targets=sorted({int.from_bytes(raw[i*4:i*4+4],'big') for i in indices}),
            table_vram=address,table_count=count,table_sha256=table['sha256']))
        if 'indices' in table:
            transfers[-1]['table_indices']=indices
    # These tables live outside the CPU ROM mappings. Resolve their bytes here;
    # discovery still checks each transfer instruction and every target mapping.
    profile['discovery']={**profile['discovery'],'jump_tables':[],
        'transfers':[*profile['discovery'].get('transfers',[]),*transfers]}


def validate_idle_loop(rom: bytes, profile: dict) -> None:
    boot=profile['bootstrap'];idle=boot['idle_entry']
    section=next((s for s in profile['discovery']['regions'] if s['vram']<=idle<s['vram']+s['size']),None)
    if section is None or idle%4 or idle+12>section['vram']+section['size']:
        raise RecompError('GoldenEye idle loop outside mapped code')
    offset=section['rom_offset']+idle-section['vram']
    words=[int.from_bytes(rom[offset+i*4:offset+i*4+4],'big') for i in range(3)]
    if words!=boot['idle_words'] or words!=[0xafa40000,0x1000ffff,0]:
        raise RecompError('GoldenEye idle-loop fingerprint mismatch')
    # Check the complete osCreateThread argument setup before allowing the
    # scheduler to park this object instead of executing its empty loop.
    create=0x70000750
    region=next((s for s in profile['discovery']['regions']
                 if s['vram']<=create and create+36<=s['vram']+s['size']),None)
    if region is None:raise RecompError('GoldenEye idle-thread creation outside mapped code')
    offset=region['rom_offset']+create-region['vram']
    expected=[0x3c048006,0x3c067000,0x24c60718,0x2484d490,0x24050001,
              0x00003825,0xafa20010,0x0c00350c,0xafa00014]
    if (rom[offset:offset+36]!=b''.join(w.to_bytes(4,'big') for w in expected)
            or profile['runtime']['idle_thread']!=0x8005d490
            or idle!=0x70000718 or boot['idle_thread_id']!=1):
        raise RecompError('GoldenEye idle-thread creation fingerprint mismatch')


def validate_inspection(rom: bytes, profile: dict) -> None:
    for name in ('stage','difficulty'):
        inspection=profile['inspection'];reader=inspection[f'{name}_reader']
        region=next((s for s in profile['discovery']['regions'] if s['vram']<=reader<=s['vram']+s['size']-12),None)
        if region is None or reader%4:raise RecompError('GoldenEye inspection getter outside mapped code')
        offset=region['rom_offset']+reader-region['vram']
        words=[int.from_bytes(rom[offset+i*4:offset+i*4+4],'big') for i in range(3)]
        if (words!=inspection[f'{name}_words'] or words[0]!=0x3c028005 or words[1]!=0x03e00008
                or words[2]>>16!=0x8c42
                or 0x80050000+((words[2]&65535)-65536 if words[2]&0x8000 else words[2]&65535)!=inspection[f'{name}_address']):
            raise RecompError(f'GoldenEye {name} inspection getter/address mismatch')


def prepare(rom, profile, output):
    verify_ai_analysis(rom, profile)
    verify_input_analysis(rom, profile)
    verify_fov_analysis(rom, profile)
    validate_idle_loop(rom,profile)
    validate_inspection(rom,profile)
    checked_dispatches(rom,profile)
    return rom, profile


def write_report(rom, profile, metadata, coverage, output):
    from common import write_json
    write_json(output / 'input-hooks.json', verify_input_analysis(rom, profile, metadata, coverage))
    write_json(output / 'fov-hooks.json', verify_fov_analysis(rom, profile, metadata, coverage))
    verify_ai_analysis(rom, profile, metadata)


def configure_translation(rom, metadata, profile, output):
    from common import write_json
    from translate import translate
    audit=translate(rom,metadata,profile,output,report_only=True)
    functions={(f['section'],f['vram']):f for f in metadata['functions']}
    for function in audit['functions']:
        if function['status']!='unsupported':continue
        original=functions[function['section'],function['vram']];offset=original['rom_offset']
        profile.setdefault('boundaries',[]).append(dict(symbol=original['name'],vram=original['vram'],
            kind='unsupported_function',reason='Pending GoldenEye conversion: '+function['errors'][0]['reason'],
            expected_words=[int.from_bytes(rom[offset:offset+4],'big')]))
    write_json(output/'pending-boundaries.json',{'complete_conversion':False,'boundaries':profile.get('boundaries',[])})
