#!/usr/bin/env python3
"""Convert the complete checked SM64 CPU text inventory, then build or run it."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))

from common import RecompError,load_metadata,load_profile,load_rom,write_json
from discover import discover
from translate import translate
from game_config import write_game_config
from build_native import build_native
from build_renderer import build_renderer
from verify_game import verify_game,verify_suite
from renderer_capture import verify_ppm


def prepare_full(output: Path,report_only: bool=False):
    project=Path(__file__).resolve().parents[2]
    profile=load_profile(project/'games/sm64-us/full.toml');game=profile['game']
    rom,identity=load_rom(game['rom'],game['rom_sha1'],game['rom_size'])
    write_json(output/'symbols.json',discover(rom,profile))
    metadata=load_metadata(output/'symbols.json',rom)
    report=translate(rom,metadata,profile,output,report_only=report_only)
    write_json(output/'full_boundaries.json',{'rom_sha1':identity['sha1'],'coverage':report['summary'],
        'code_inventory':metadata['code_inventory'],'native_hooks':profile['hooks'],
        'remaining':'broader gameplay, course transitions, ending and prolonged-run validation; additional device/microcode variants'})
    if not report_only:
        write_game_config(profile,metadata,output,('gSoundThread','gGameLoopThread','gControllers',
            'gControllerPads','gControllerBits','gEepromProbe','gSaveBuffer','gGlobalTimer',
            'gPhysicalFramebuffers','gPhysicalZBuffer','gGfxVblankQueue','gGameVblankQueue',
            'gCurrentArea','sSegmentTable','gGdCtrl','gCurrLevelNum','gCurrSaveFileNum',
            'sMenuSelectedFile','sLevelScriptCommand','gMarioState','gCamera','gDebugLevelSelect'))
    print(f"Full CPU inventory: {report['summary']}; {metadata['code_inventory']['cpu_words']} CPU instructions; ROM {identity['sha1']}",flush=True)
    return profile,identity,report


def main() -> int:
    project=Path(__file__).resolve().parents[2];parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--report-only',action='store_true')
    parser.add_argument('--build-only',action='store_true')
    parser.add_argument('--render',action='store_true',help='build/run the complete CPU inventory with RT64 and SDL audio')
    parser.add_argument('--cheat-menu',action='store_true',help='enable the original SELECT STAGE debug menu; press Start on the title screen (requires --render and live input)')
    parser.add_argument('--verify-suite',action='store_true')
    parser.add_argument('--sanitize-address',action='store_true')
    parser.add_argument('--cc',default=os.environ.get('RECOMP_TEST_CC') or shutil.which('clang') or 'cc')
    parser.add_argument('--jobs',type=int,default=4)
    parser.add_argument('--frames',type=int)
    parser.add_argument('--mode',choices=('live','idle','move','jump','interior'))
    parser.add_argument('--save',type=Path)
    parser.add_argument('--mute',action='store_true')
    parser.add_argument('--playback',action='store_true')
    parser.add_argument('--capture',action='store_true',help='write task/RAM traces and WAV during interactive play; bounded checks capture automatically')
    textures=project/'local/textures/sm64-us'
    parser.add_argument('--dump-textures',type=Path,nargs='?',const=textures/'dump',help='dump original RGBA PNGs as textures reach RT64 (optional directory)')
    parser.add_argument('--dump-grid',type=Path,nargs='?',const=True,metavar='PNG',help='export all dumped textures as grids of up to 16 enlarged tiles with JSONs/prompts; with --render, export on exit')
    parser.add_argument('--texture-replacements',type=Path,default=textures/'replacements',help='directory of hash-named replacement PNGs; F5 reloads and toggles originals/replacements during play')
    parser.add_argument('--no-texture-replacements',action='store_true',help='render only original textures')
    args=parser.parse_args();mode=args.mode or ('live' if args.render else 'idle')
    if args.dump_grid is not None:
        from remake_textures import main as remake_textures
        dump=args.dump_textures or textures/'dump'
        grid=dump.parent/'grids/all-textures.png' if args.dump_grid is True else args.dump_grid
        grid_command=['--input',str(dump),'--dump-grid',str(grid)]
        if not args.render:return remake_textures(grid_command)
        args.dump_textures=dump
    frames=args.frames if args.frames is not None else (0 if args.render and mode=='live' else 180)
    if frames!=0 and not 60<=frames<=600 or frames==0 and (not args.render or mode=='interior'):
        raise RecompError('frames must be 60..600, or 0 for an interactive graphics run')
    if not 1<=args.jobs<=32:raise RecompError('jobs must be 1..32')
    if args.render and (args.sanitize_address or args.verify_suite):raise RecompError('sanitizers and the acceptance suite use the headless native build')
    if mode=='live' and not args.render:raise RecompError('live keyboard input requires --render')
    if args.cheat_menu and (not args.render or mode!='live' or frames):
        raise RecompError('--cheat-menu requires --render with --mode live and --frames 0')
    if args.dump_textures is not None and not args.render:raise RecompError('--dump-textures requires --render')
    if args.dump_textures is not None and args.dump_textures.resolve()==args.texture_replacements.resolve():
        raise RecompError('texture dump and replacement directories must differ')
    output=(args.output or project/'build/sm64-us/rom'/('full-sanitized' if args.sanitize_address else 'full')).resolve()
    profile,identity,report=prepare_full(output,args.report_only)
    if args.report_only:return 0
    binary=build_renderer(output,args.jobs,'sm64_native_full_rt64','RECOMP_FULL_GENERATED') if args.render else build_native(output,args.cc,sanitize_address=args.sanitize_address,
                              executable_name='sm64_native_full',application=project/'games/sm64-us/app.c')
    print(f'Native full-profile executable: {binary}',flush=True)
    if args.build_only:return 0
    if args.verify_suite:
        verify_suite(binary,profile,output,frames);return 0
    save=(args.save or project/'local/saves'/('sm64-us.eeprom' if args.render else 'sm64-full-validation.eeprom')).resolve()
    save.parent.mkdir(parents=True,exist_ok=True);env=dict(os.environ)
    env['RECOMP_AUDIO_PLAYBACK']='1' if args.playback else '0';env['RECOMP_AUDIO_MUTE']='1' if args.mute else '0'
    env['RECOMP_CAPTURE']='1' if args.capture else '0'
    env['RECOMP_TEXTURE_DUMP']=str(args.dump_textures.resolve()) if args.dump_textures is not None else ''
    env['RECOMP_TEXTURE_REPLACEMENTS']='' if args.no_texture_replacements else str(args.texture_replacements.resolve())
    if args.render:env.update(SDL_VIDEODRIVER='x11',MESA_SHADER_CACHE_DIR=str(project/'local/rt64/mesa-cache'))
    command=[str(binary),str(profile['game']['rom']),str(output),str(save),str(frames),mode]
    if args.cheat_menu:command.append('--cheat-menu')
    subprocess.run(command,
                   env=env,check=True,timeout=600 if frames else None)
    if args.dump_grid is not None:remake_textures(grid_command)
    if not frames and not args.capture:return 0
    last=json.loads((output/'task_trace.jsonl').read_text().splitlines()[-1])
    if last['type']=='cancelled':return 0
    verify_game(output)
    if args.render:
        pixels={name:verify_ppm(output/f'{name}.ppm') for name in ('window','native_framebuffer')}
        write_json(output/'pixel_verification.json',pixels)
    return 0


if __name__=='__main__':
    try:sys.exit(main())
    except (RecompError,OSError,subprocess.SubprocessError,KeyError,ValueError) as error:
        print(f'error: {error}',file=sys.stderr);sys.exit(1)
