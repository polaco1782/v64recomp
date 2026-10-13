#!/usr/bin/env python3
"""Import/discover GoldenEye ROM code and validate its translated native startup."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))

from build_native import build_native
from build_renderer import build_renderer
from common import RecompError, load_metadata, load_profile, load_rom, write_json
from discover import discover
from translate import translate
from renderer_capture import verify_ppm


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


def import_supplied(source: Path, profile: dict) -> dict:
    game = profile['game']
    rom, identity = load_rom(source, game['rom_sha1'], game['rom_size'])
    destination = game['rom']
    project = Path(__file__).resolve().parents[2]
    if not destination.is_relative_to(project / 'local'):
        raise RecompError('ROM import destination must be inside local/')
    if destination.exists() and destination.read_bytes() != rom:
        raise RecompError('GoldenEye import destination already contains different bytes')
    if not destination.exists():
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(rom)
    return identity


def write_boot_config(profile: dict, metadata: dict, output: Path) -> None:
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
        lines.extend(['    .vi_period_ticks=BOOT_VI_PERIOD_TICKS,', '};',
                      'static const RecompSchedulerConfig ge_scheduler_config = {'])
        lines.extend(f'    .{name}=UINT32_C(0x{value:x}),' for name,value in profile['runtime'].items())
        lines.extend(['};', ''])
        game_section=next(s for s in metadata['sections'] if s['name']=='.game')
        lines.append(f'#define BOOT_GAME_SECTION {game_section["id"]}')
    for name,value in profile.get('inspection',{}).items():
        if type(value) is int:lines.append(f'#define GE_INSPECT_{name.upper()} UINT32_C(0x{value:x})')
    if 'cheats' in profile:
        cheats=profile['cheats'];mask=sum(1<<i for i in cheats['allowed_ids'])
        lines.extend([f'#define GE_CHEAT_ENTRY UINT32_C(0x{cheats["entry"]:x})',
                      f'#define GE_CHEAT_UNLOCK_MASK UINT64_C(0x{mask:x})',
                      f'#define GE_CHEAT_COUNT {len(cheats["allowed_ids"])}',
                      f'#define GE_CHEAT_LEVELS_ADDRESS UINT32_C(0x{cheats["levels_address"]:x})'])
    (output / 'goldeneye_boot_config.h').write_text('\n'.join(lines))


def checked_dispatches(rom: bytes, profile: dict) -> None:
    """Bind frozen local jump targets to the ROM's initialized data tables."""
    project = Path(__file__).resolve().parents[2]
    document=json.loads((project/'games/goldeneye-us/dispatches.json').read_text())
    if document.get('format_version')!=1 or document.get('rom_sha1')!=profile['game']['rom_sha1']:
        raise RecompError('GoldenEye dispatch inventory belongs to another ROM')
    data=startup_data(rom,profile['bootstrap'])
    for transfer in document['transfers']:
        offset=transfer['table_vram']-profile['bootstrap']['data_address']
        size=transfer['table_count']*4
        if offset<0 or size<=0 or offset+size>len(data):
            raise RecompError('GoldenEye jump table outside initialized data')
        raw=data[offset:offset+size]
        if (hashlib.sha256(raw).hexdigest()!=transfer['table_sha256']
                or sorted({int.from_bytes(raw[i:i+4],'big') for i in range(0,size,4)})!=transfer['targets']):
            raise RecompError(f'GoldenEye jump table fingerprint/targets mismatch at {transfer["pc"]:#010x}')
    profile['discovery']['transfers'].extend(document['transfers'])


def validate_idle_loop(rom: bytes, profile: dict) -> None:
    boot=profile['bootstrap'];idle=boot['idle_entry']
    section=next((s for s in profile['discovery']['regions'] if s['vram']<=idle<s['vram']+s['size']),None)
    if section is None or idle%4 or idle+12>section['vram']+section['size']:
        raise RecompError('GoldenEye idle loop outside mapped code')
    offset=section['rom_offset']+idle-section['vram']
    words=[int.from_bytes(rom[offset+i*4:offset+i*4+4],'big') for i in range(3)]
    if words!=boot['idle_words'] or words!=[0xafa40000,0x1000ffff,0]:
        raise RecompError('GoldenEye idle-loop fingerprint mismatch')


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


def controller_script(path: Path, output: Path, rom_sha1: str) -> None:
    """Validate a controller-only replay; it never changes guest game state."""
    document=json.loads(path.read_text())
    if (not isinstance(document,dict) or document.get('format_version')!=1 or document.get('rom_sha1')!=rom_sha1
            or not isinstance(document.get('events'),list) or not 0<len(document['events'])<=4096):
        raise RecompError('invalid GoldenEye controller script identity/events')
    previous=0;lines=[]
    for event in document['events']:
        if not isinstance(event,dict):raise RecompError('controller event must be an object')
        frame,buttons,x,y=(event.get(k,0) for k in ('frame','buttons','stick_x','stick_y'))
        if (any(type(v) is not int for v in (frame,buttons,x,y)) or not previous<frame<=36000
                or not 0<=buttons<=65535 or not -128<=x<=127 or not -128<=y<=127):
            raise RecompError('controller events need increasing frames and valid pad values')
        previous=frame;lines.append(f'{frame} {buttons} {x} {y}')
    output.write_text('\n'.join(lines)+'\n')


def validate_cheats(rom: bytes, profile: dict) -> None:
    cheats=profile['cheats']
    for address,words in [(cheats['entry'],cheats['entry_words']),
                          (cheats['unavailable_target'],cheats['unavailable_words']),
                          (cheats['levels_reader'],cheats['levels_words'])]:
        size=len(words)*4
        region=next((s for s in profile['discovery']['regions'] if s['vram']<=address<=s['vram']+s['size']-size),None)
        if region is None or address%4:raise RecompError('GoldenEye cheat query outside mapped code')
        offset=region['rom_offset']+address-region['vram']
        if rom[offset:offset+size]!=b''.join(w.to_bytes(4,'big') for w in words):
            raise RecompError('GoldenEye cheat query fingerprint mismatch')
    words=cheats['levels_words']
    if (len(words)!=3 or words[:2]!=[0x3c028003,0x03e00008] or words[2]>>16!=0x8c42
            or 0x80030000+((words[2]&65535)-65536 if words[2]&0x8000 else words[2]&65535)!=cheats['levels_address']):
        raise RecompError('GoldenEye mission unlock getter/address mismatch')
    data=startup_data(rom,profile['bootstrap']);offset=cheats['table_address']-profile['bootstrap']['data_address']
    size=cheats['table_count']*4
    if offset<0 or size<=0 or offset+size>len(data):raise RecompError('GoldenEye cheat table outside initialized data')
    table=data[offset:offset+size]
    ids=[i+1 for i in range(cheats['table_count'])
         if int.from_bytes(table[i*4:i*4+4],'big')!=cheats['unavailable_target']]
    if (hashlib.sha256(table).hexdigest()!=cheats['table_sha256'] or ids!=cheats['allowed_ids']
            or len(ids)!=23 or not all(0<i<64 for i in ids)):
        raise RecompError('GoldenEye cheat table/options fingerprint mismatch')


def main() -> int:
    project = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, help='verify/import the supplied byte-, word- or big-endian ROM')
    parser.add_argument('--report-only', action='store_true', help='audit broader reachable code without claiming full conversion')
    parser.add_argument('--build-only', action='store_true', help='compile the bounded startup checker without executing it')
    parser.add_argument('--sanitize-address', action='store_true')
    parser.add_argument('--startup', action='store_true', help='run main-thread startup through the first graphics task')
    parser.add_argument('--render', action='store_true', help='render original startup in an RT64 desktop window (implies --startup)')
    parser.add_argument('--frames', type=int, help='frames to present (0 for live play, 1..36000 for a bounded check; default 360, or 0 with --cheat-menu)')
    parser.add_argument('--cheat-menu',action='store_true',help='unlock all Solo missions and all 23 regular Cheat Options for this rendered session')
    parser.add_argument('--input-script',type=Path,help='controller event JSON for a repeatable rendered check')
    parser.add_argument('--save',type=Path,help='EEPROM path (render default: local/saves/goldeneye-us.eeprom)')
    parser.add_argument('--wav',type=Path,help='capture synthesized stereo PCM to a WAV file')
    parser.add_argument('--mute',action='store_true',help='synthesize audio without opening an SDL playback device')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--cc', default=os.environ.get('RECOMP_TEST_CC') or shutil.which('clang') or 'cc')
    parser.add_argument('--output', type=Path, help='output root (default: build/goldeneye-us/rom)')
    args = parser.parse_args()
    if args.frames is None:args.frames=0 if args.cheat_menu else 360
    if args.render:args.startup=True
    if not 0<=args.frames<=36000 or not 1<=args.jobs<=32:
        raise RecompError('invalid render frame count or build jobs')
    if not args.render and (args.frames==0 or args.input_script or args.wav or args.mute or args.cheat_menu):
        raise RecompError('live frames, --cheat-menu, controller replays and playback options require --render')
    if args.render and (args.report_only or args.sanitize_address):
        raise RecompError('--render cannot be combined with --report-only or --sanitize-address')
    if args.report_only and (args.build_only or args.sanitize_address):
        raise RecompError('--report-only cannot be combined with build or sanitizer options')
    profile_name = 'startup.toml' if args.startup else 'profile.toml' if args.report_only else 'boot.toml'
    profile = load_profile(project / 'games/goldeneye-us' / profile_name)
    input_identity = import_supplied(args.rom, profile) if args.rom else None
    game = profile['game']
    rom, identity = load_rom(game['rom'], game['rom_sha1'], game['rom_size'])
    expected_data = startup_data(rom, profile['bootstrap'])
    if args.startup:validate_idle_loop(rom,profile);validate_inspection(rom,profile);validate_cheats(rom,profile)
    if args.startup: checked_dispatches(rom,profile)
    root = (args.output or project / 'build/goldeneye-us/rom').resolve()
    output = root / ('startup' if args.startup else 'discovery' if args.report_only else 'boot')
    metadata = discover(rom, profile)
    write_json(output / 'symbols.json', metadata)
    metadata = load_metadata(output / 'symbols.json', rom)
    write_json(output / 'rom-layout.json', {
        'rom': identity, 'imported_input': input_identity, 'header_title': rom[32:52].decode('ascii').strip(),
        'region': chr(rom[62]), 'revision': rom[63], 'header_crc': rom[16:24].hex(),
        'sections': metadata['sections'], 'bootstrap': profile['bootstrap'],
        'initial_cpu_load': 'IPL3 ROM copy; original boot/TLB entry is replaced with native page installation',
        'game_code_mapping': '0x7f000000 demand-paged ROM text; native 8 KiB cache installed by --startup',
        'reference_required': False,
    })
    if args.report_only:
        report = translate(rom, metadata, profile, output, report_only=True)
        write_json(output / 'audit.json', {
            'rom_sha1': identity['sha1'], 'discovery': metadata['discovery_summary'],
            'coverage': report['summary'], 'complete_cpu_inventory': False,
            'known_gaps': ['game-code demand paging', 'remaining OS/hardware hooks',
                          'indirect calls and local jump tables', 'unvisited code/data classification',
                          'graphics/audio microcode compatibility and gameplay validation'],
        })
        print(f"GoldenEye ROM discovery: {metadata['discovery_summary']}", flush=True)
        print(f"Coverage: {report['summary']}; incomplete inventory, report: {output / 'audit.json'}", flush=True)
        return 0

    # Explicit selection permits a bounded check while unvisited game code and
    # the declared stop-before-main boundary remain visible in coverage.json.
    # Never describe these selected functions as a full ROM conversion.
    if args.startup:
        # Preserve explicit fail-on-entry boundaries for remaining translation
        # gaps. Startup acceptance requires avoiding every such boundary.
        audit=translate(rom,metadata,profile,output,report_only=True)
        for function in audit['functions']:
            if function['status']!='unsupported':continue
            original=next(f for f in metadata['functions'] if f['section']==function['section'] and f['vram']==function['vram'])
            offset=original['rom_offset']
            profile.setdefault('boundaries',[]).append(dict(symbol=original['name'],vram=original['vram'],
                kind='unsupported_function',reason='Pending GoldenEye conversion: '+function['errors'][0]['reason'],
                expected_words=[int.from_bytes(rom[offset:offset+4],'big')]))
        write_json(output/'pending-boundaries.json',{'complete_conversion':False,'boundaries':profile.get('boundaries',[])})
    report = translate(rom, metadata, profile, output, [f['name'] for f in metadata['functions']])
    write_boot_config(profile, metadata, output)
    print(f"GoldenEye startup conversion: {report['summary']}; {report['instruction_count']} instruction words", flush=True)
    application=project/'games/goldeneye-us'/('startup.c' if args.startup else 'boot.c')
    binary = build_renderer(output,args.jobs,'goldeneye_native_rt64','RECOMP_GOLDENEYE_GENERATED') if args.render else build_native(output, args.cc, sanitize_address=args.sanitize_address,
                          optimization='0' if args.startup else '2',
                          executable_name='goldeneye_native_startup' if args.startup else 'goldeneye_native_boot',
                          application=application,audio=args.startup,
                          extra_sources=(project/'games/goldeneye-us/pager.c',) if args.startup else ())
    print(f'Native startup checker: {binary}', flush=True)
    if args.build_only:
        return 0
    (output / 'verification.json').unlink(missing_ok=True)
    data_capture, boot_report = output / 'startup-data.bin', output / 'boot-report.json'
    data_capture.unlink(missing_ok=True)
    boot_report.unlink(missing_ok=True)
    command=[str(binary),str(game['rom']),str(data_capture),str(boot_report)]
    env=os.environ.copy()
    if args.render:
        command.append(str(args.frames))
        if args.cheat_menu:command.append('--cheat-menu')
        env.update(SDL_VIDEODRIVER='x11',MESA_SHADER_CACHE_DIR=str(project/'local/rt64/mesa-cache'))
        save=(args.save or project/'local/saves/goldeneye-us.eeprom').resolve();save.parent.mkdir(parents=True,exist_ok=True)
        env['RECOMP_GOLDENEYE_SAVE']=str(save);env['RECOMP_AUDIO_MUTE']='1' if args.mute else '0'
        env.pop('RECOMP_GOLDENEYE_INPUT',None);env.pop('RECOMP_GOLDENEYE_WAV',None)
        if args.input_script:
            replay=output/'controller-input.txt';controller_script(args.input_script,replay,identity['sha1'])
            env['RECOMP_GOLDENEYE_INPUT']=str(replay)
        if args.wav:
            wav=args.wav.resolve();wav.parent.mkdir(parents=True,exist_ok=True);env['RECOMP_GOLDENEYE_WAV']=str(wav)
    elif args.save:
        save=args.save.resolve();save.parent.mkdir(parents=True,exist_ok=True);env['RECOMP_GOLDENEYE_SAVE']=str(save)
    subprocess.run(command,env=env,check=True,timeout=None if args.render and not args.frames else max(180,args.frames//15) if args.render else 60)
    actual = data_capture.read_bytes()
    if actual != expected_data:
        raise RecompError('native MIPS-to-C inflater differs from independently decoded ROM startup data')
    boot_result = json.loads(boot_report.read_text())
    expected_state='rendered_startup' if args.render else 'first_graphics_task' if args.startup else 'stopped_before_entry'
    if (boot_result.get('main_state')!=expected_state
            or boot_result.get('gameplay_validated' if args.render else 'gameplay_executed') is not False):
        raise RecompError('GoldenEye startup checker reported an unexpected execution boundary')
    if args.render and (not boot_result.get('rendered') or not boot_result.get('graphics_tasks') and not boot_result.get('window_closed')
                       or args.frames and not boot_result.get('window_closed') and boot_result.get('presentations')!=args.frames
                       or boot_result.get('sp_callbacks')!=boot_result.get('graphics_tasks')
                       or boot_result.get('dp_callbacks')!=boot_result.get('graphics_tasks')):
        raise RecompError('GoldenEye startup did not submit/present the requested graphics')
    pixels={}
    if args.render:
        capture=output/'boot-report.json.window.ppm'
        (output/'window.png').unlink(missing_ok=True)
        if capture.exists():
            try:pixels=verify_ppm(capture,allow_grayscale=True)
            except RecompError as error:
                if not boot_result.get('window_closed') or str(error)!='pixel capture lacks a visible startup image':raise
                pixels={'capture_state':'blank_at_window_close'}
            else:shutil.copyfile(capture.with_suffix('.png'),output/'window.png')
        elif boot_result.get('window_closed'):pixels={'capture_state':'closed_before_graphics'}
        else:raise RecompError('GoldenEye final window capture missing')
        if boot_result.get('save_failures'):raise RecompError('GoldenEye EEPROM persistence failed')
        if (boot_result.get('cheat_menu')!=args.cheat_menu
                or boot_result.get('unlocked_cheat_count')!=(23 if args.cheat_menu else 0)
                or boot_result.get('missions_unlocked')!=args.cheat_menu):
            raise RecompError('GoldenEye cheat-menu launch option was not applied')
    write_json(output / 'verification.json', {
        'status': 'native_startup_rendered' if args.render else 'native_startup_passed' if args.startup else 'native_bootstrap_passed', 'rom_sha1': identity['sha1'],
        'startup_data_sha256': hashlib.sha256(actual).hexdigest(), 'startup_data_bytes': len(actual),
        'data_matches_independent_deflate': True, 'main_thread': boot_result,
        'sanitizers': 'none' if args.render else 'address,undefined' if args.sanitize_address else 'undefined',
        'complete_game_conversion': False,
        **({'window_pixels':pixels} if args.render else {}),
    })
    print(f"PASS: original translated inflater produced {len(actual):,} byte-identical startup bytes; "
          + (f"{boot_result['graphics_tasks']} graphics tasks rendered; window closed cleanly." if args.render and boot_result.get('window_closed')
             else f"{boot_result['graphics_tasks']} graphics tasks rendered with a visible image." if args.render
             else 'original main thread reached graphics.' if args.startup else 'main thread created.'),flush=True)
    print(f"Verification: {output / 'verification.json'}", flush=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RecompError, OSError, ValueError, KeyError, StopIteration, subprocess.SubprocessError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
