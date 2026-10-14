#!/usr/bin/env python3
"""Build and validate Nightmare Creatures's ROM-derived native startup."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT / 'tools'))
from build_native import build_native
from build_renderer import build_renderer
from common import RecompError, load_metadata, load_profile, load_rom, write_json
from discover import discover
from translate import translate
from renderer_capture import verify_ppm


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game, boot = profile['game'], profile['bootstrap']
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': game['rom_size'], 'RAM_SIZE': game['ram_size']}
    constants.update({name.upper(): value for name, value in boot.items() if type(value) is int})
    constants.update(STARTUP=int(startup))
    lines = ['/* Generated from the pinned Nightmare Creatures ROM profile. */']
    lines.extend(f'#define BOOT_{name} UINT32_C(0x{value:x})' for name, value in constants.items())
    lines.append('static const uint8_t boot_sha1[20] = {' + ','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1'])) + '};')
    lines.append('static const uint32_t boot_parameters[][2] = {')
    lines.extend(f'    {{UINT32_C({address}), UINT32_C(0x{value:x})}},' for address, value in boot['parameters'].items())
    lines.extend(['};', ''])
    (output / 'nightmare_config.h').write_text('\n'.join(lines))


def controller_script(path: Path, output: Path, sha1: str) -> Path:
    document = json.loads(path.read_text())
    if document.get('format_version') != 1 or document.get('rom_sha1') != sha1:
        raise RecompError('controller replay belongs to a different ROM/version')
    events = document.get('events')
    if not isinstance(events, list) or not events:
        raise RecompError('controller replay needs events')
    lines, previous = [], -1
    for event in events:
        frame, buttons = event.get('frame'), event.get('buttons')
        x, y = event.get('stick_x', 0), event.get('stick_y', 0)
        if (type(frame) is not int or frame <= previous or frame > 0xffffffff
                or type(buttons) is not int or not 0 <= buttons <= 0xffff
                or type(x) is not int or type(y) is not int or not -128 <= x <= 127 or not -128 <= y <= 127):
            raise RecompError('controller replay has an invalid frame or pad value')
        lines.append(f'{frame} {buttons} {x} {y}')
        previous = frame
    destination = output/'controller-input.txt'
    destination.write_text('\n'.join(lines)+'\n')
    return destination

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, help='verify and import the supplied ROM into local/')
    parser.add_argument('--output', type=Path, default=PROJECT / 'build/nightmare-us/rom')
    parser.add_argument('--report-only', action='store_true', help='report discovery and translation gaps without building')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--startup', action='store_true', help='execute the original application through its first RSP task')
    parser.add_argument('--render', action='store_true', help='run original startup with RT64, audio and live controls')
    parser.add_argument('--debug', action='store_true', help='build RT64 with full debug symbols in build/debug (requires --render)')
    parser.add_argument('--frames', type=int, default=360, help='presentation limit; zero runs until the window closes')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--mute', action='store_true')
    parser.add_argument('--performance', action='store_true', help='record timing and audio queue depth during rendering')
    parser.add_argument('--wav', type=Path)
    parser.add_argument('--input-script', type=Path, help='ROM-bound controller replay JSON')
    parser.add_argument('--capture-every', type=int, default=0, help='capture every N presentations during rendered checks')
    parser.add_argument('--sanitize-address', action='store_true')
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    if args.debug and not args.render:
        parser.error('--debug requires --render')
    if args.frames < 0 or args.frames > 0xffffffff or args.jobs <= 0 or not 0 <= args.capture_every <= 0xffffffff:
        parser.error('frames must be a nonnegative 32-bit count and jobs must be positive')
    if args.render and args.sanitize_address:
        parser.error('--sanitize-address applies to native headless checks')
    if (args.input_script or args.mute or args.wav or args.capture_every or args.performance) and not args.render:
        parser.error('controller replay, mute, WAV, captures and timing require --render')
    if args.render: args.startup = True
    profile = load_profile(PROJECT / 'games/nightmare-us' / ('startup.toml' if args.startup or args.report_only else 'boot.toml'))
    game = profile['game']
    if args.rom:
        subprocess.run([sys.executable, str(PROJECT/'tools/import_rom.py'), '--profile',
                        str(PROJECT/'games/nightmare-us/profile.toml'), '--rom', str(args.rom.resolve())], check=True)
    rom, identity = load_rom(game['rom'], game['rom_sha1'], game['rom_size'])
    output = args.output.resolve() / ('discovery' if args.report_only else 'startup' if args.startup else 'boot')
    output.mkdir(parents=True, exist_ok=True)
    metadata = discover(rom, profile)
    write_json(output / 'symbols.json', metadata)
    metadata = load_metadata(output / 'symbols.json', rom)
    names = [f['name'] for f in metadata['functions']]
    coverage = translate(rom, metadata, profile, output, names=names, report_only=args.report_only)
    print(f"Nightmare Creatures conversion: {coverage['summary']}; {coverage['instruction_count']} instruction words", flush=True)
    if args.report_only:
        print(f"Coverage: {output/'coverage.json'}", flush=True)
        return 0
    write_config(profile, metadata, output, args.startup)
    binary = build_renderer(output, args.jobs, 'nightmare_native_rt64', 'RECOMP_NIGHTMARE_RENDER_GENERATED', debug=args.debug) if args.render else build_native(output, args.cc, application=PROJECT/'games/nightmare-us/app.c',
                          executable_name='nightmare_native_startup' if args.startup else 'nightmare_native_boot',
                          sanitize_address=args.sanitize_address, audio=False)
    print(f'Native executable: {binary}', flush=True)
    if args.build_only:
        return 0
    report_path = output/'boot-report.json'
    verification = output/'verification.json'
    report_path.unlink(missing_ok=True)
    verification.unlink(missing_ok=True)
    command = [str(binary), str(game['rom']), str(report_path)]
    env = os.environ.copy()
    if args.render:
        command.extend([str(args.frames), str(PROJECT/'local/rt64')])
        env['SDL_VIDEODRIVER'] = 'x11'
        env['MESA_SHADER_CACHE_DIR'] = str(PROJECT/'local/rt64/mesa-cache')
        env.pop('RECOMP_AUDIO_MUTE', None)
        env.pop('RECOMP_NIGHTMARE_PERFORMANCE', None)
        if args.performance: env['RECOMP_NIGHTMARE_PERFORMANCE'] = '1'
        env.pop('RECOMP_NIGHTMARE_WAV', None)
        env.pop('RECOMP_NIGHTMARE_INPUT', None)
        env['RECOMP_NIGHTMARE_CAPTURE_EVERY'] = str(args.capture_every)
        if args.mute: env['RECOMP_AUDIO_MUTE'] = '1'
        if args.wav:
            args.wav.parent.mkdir(parents=True, exist_ok=True)
            env['RECOMP_NIGHTMARE_WAV'] = str(args.wav.resolve())
        if args.input_script:
            env['RECOMP_NIGHTMARE_INPUT'] = str(controller_script(args.input_script, output, identity['sha1']))
    subprocess.run(command, env=env, check=True, timeout=None if args.render and not args.frames else 180 if args.render else 60)
    report = json.loads(report_path.read_text())
    expected_state = 'rendered_startup' if args.render else 'first_rsp_task' if args.startup else 'stopped_before_entry'
    if (report.get('main_state') != expected_state or report.get('main_entry') != profile['bootstrap']['main_entry']
            or report.get('main_id') != profile['bootstrap']['main_id'] or report.get('main_priority') != profile['bootstrap']['main_priority']
            or not all(report.get(k) is True for k in ('bss_cleared', 'bss_guard_verified'))):
        raise RecompError('Nightmare Creatures bootstrap report did not pass its original startup checks')
    pixels = {}
    if args.render and report['graphics_tasks']:
        if (report['sp_callbacks'] != report['graphics_tasks']
                or report['dp_callbacks'] + report['graphics_tasks_without_dp'] != report['graphics_tasks']
                or args.frames and not report['window_closed'] and report['presentations'] != args.frames):
            raise RecompError('Nightmare Creatures graphics completion/presentation counts differ')
        pixels = verify_ppm(Path(str(report_path)+'.window.ppm'), allow_grayscale=True)
    write_json(verification, {'status': 'native_startup_rendered' if args.render else 'native_startup_passed' if args.startup else 'native_bootstrap_passed', 'rom_sha1': identity['sha1'],
                              'sanitizers': 'none' if args.render else 'address,undefined' if args.sanitize_address else 'undefined',
                              'complete_game_conversion': False, 'bootstrap': report,
                              **({'window_pixels': pixels} if pixels else {})})
    if args.render:
        result = f"{report['presentations']} presentations, {report['graphics_tasks']} graphics tasks, {report['audio_frames']} audio frames"
    elif args.startup:
        result = 'original application reached its first RSP task'
    else:
        result = 'original BSS clear and application thread creation'
    print(f'PASS: {result}; {verification}', flush=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RecompError, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
