"""Game-specific inputs for CMake source generation."""
from pathlib import Path
import hashlib, json, zlib
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
    (output / 'goldeneye_boot_config.h').write_text('\n'.join(lines))
    analysis = profile['input_analysis']
    constants = {'CURRENT_PLAYER': analysis['current_player']['address'],
                 'PROCESS_RETURN': profile['input_adapter']['process_return']}
    constants.update({f['name'].upper(): f['offset'] for f in analysis['player_fields']})
    constants.update({g['name'].upper(): g['address'] for g in profile['input_adapter']['globals']})
    angle = next(f for f in analysis['functions'] if f['name'] == 'bondviewApplyVertaTheta')
    entry = next(f for f in metadata['functions'] if f['vram'] == angle['vram'])
    lines = ['/* Verified GoldenEye input addresses; generated from profile.toml. */']
    lines.extend(f'#define GE_INPUT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append(f'#define GE_INPUT_ORIGINAL_ANGLES recomp_s{entry["section"]}_{entry["vram"]:08x}')
    (output / 'goldeneye_input_config.h').write_text('\n'.join(lines) + '\n')


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
    verify_input_analysis(rom, profile)
    validate_idle_loop(rom,profile)
    validate_inspection(rom,profile)
    checked_dispatches(rom,profile)
    return rom, profile


def write_report(rom, profile, metadata, coverage, output):
    from common import write_json
    write_json(output / 'input-hooks.json', verify_input_analysis(rom, profile, metadata, coverage))


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
