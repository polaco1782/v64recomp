#!/usr/bin/env python3
"""CMake's cached ROM-to-C generation step. Does not compile or launch games."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import sys
import threading
import time
from common import RecompError, load_profile, load_rom, load_metadata, write_json
from discover import discover
from translate import translate
from symbol_database import DATABASE

PROJECT = Path(__file__).resolve().parents[1]
GAMES = ('sm64-us','goldeneye-us','shadowman-us','quake-us','doom64-us','duke-us','nightmare-us','oot-us-v12','perfectdark-us-v11','conker-us','duke-zero-hour-us','hexen-us','rayman2-us','quake2-us','re2-us-v1','turok-us-v12','turok2-us','turok3-us','mm-us','1080-us','banjo-kazooie-us','twine-us','bad-apple','media-player')


class GenerationProgress:
    """Keep long, otherwise silent generation stages visible in build logs."""

    def __init__(self, game: str, interval: float = 5.0) -> None:
        self.game = game
        self.interval = interval
        self.started = time.monotonic()
        self.stage_started = self.started
        self.message = ''
        self.lock = threading.Lock()
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self._heartbeat, daemon=True)

    def _write(self, message: str) -> None:
        elapsed = time.monotonic() - self.started
        print(f'[{self.game} +{elapsed:.1f}s] {message}', file=sys.stderr, flush=True)

    def stage(self, message: str) -> None:
        with self.lock:
            self.message = message
            self.stage_started = time.monotonic()
            self._write(message)

    def _heartbeat(self) -> None:
        while not self.stopped.wait(self.interval):
            with self.lock:
                elapsed = time.monotonic() - self.stage_started
                self._write(f'Still running: {self.message} ({elapsed:.1f}s in stage)')

    def __enter__(self) -> GenerationProgress:
        self.stage('Loading profile and ROM')
        self.thread.start()
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.stopped.set()
        self.thread.join()
        if exc_type is None:
            self._write('Generation complete')


def split_sources(output: Path) -> list[Path]:
    """Bound compiler memory use and allow parallel compilation for every game."""
    source = (output/'functions.c').read_text()
    registry = source.index('const RecompFunctionEntry recomp_functions[]')
    starts = [m.start() for m in re.finditer(r'^void recomp_s[0-9]+_[0-9a-f]+\(',source[:registry],re.M)]
    starts.append(registry)
    groups = {}
    for a,b in zip(starts,starts[1:]):
        part = source[a:b]
        match = re.match(r'void recomp_s([0-9]+)_([0-9a-f]+)',part)
        key = f'{int(match[1]):02d}-{int(match[2],16)//0x8000:06x}'
        groups.setdefault(key,[]).append(part)
    paths = []
    for key,parts in sorted(groups.items()):
        path = output/f'cpu-{key}.c'
        path.write_text('#include "generated.h"\n'+''.join(parts))
        paths.append(path)
    for path in output.glob('cpu-*.c'):
        if path not in paths:path.unlink()
    (output/'functions.c').write_text('#include "generated.h"\n'+source[registry:])
    return [output/'functions.c',*paths]


def write_cpu_regions(output, cartridge, prepared, profile, metadata, coverage, candidates):
    # Only admit words that have the same backing as the generated program.
    # Complete inventories additionally distinguish CPU code from data and RSP code.
    exclusions = {s['name']: [] for s in metadata['sections']}
    inventory = profile['discovery'].get('inventory')
    if inventory:
        for row in json.loads(Path(inventory).read_bytes())['ranges']:
            if row['kind'] != 'code':
                exclusions[row['section']].append((row['vram'], row['size']))
    for f in coverage['functions']:
        if f['status'] != 'ready':
            exclusions[metadata['sections'][f['section']]['name']].append((f['vram'], f['size']))
    for group in [*profile.get('function_wrappers', []), *profile.get('word_result_hooks', [])]:
        exclusions[group['section']].append((group['vram'], group['size']))
    for f in metadata['functions']:
        if any(f.get(k) for k in ('relocatable', 'unwind_calls', 'tail_call_continuations',
                                   'nonlocal_return_calls', 'fallthrough_transfers')):
            exclusions[metadata['sections'][f['section']]['name']].append((f['vram'], f['size']))
    switches = {s['id']: {} for s in metadata['sections']}
    def add_switches(sid, transfers):
        for transfer in transfers:
            if transfer.get('origin') == 'bounded-switch':
                pc = transfer['pc']
                record = (transfer['expected_word'], tuple(transfer['targets']))
                previous = switches[sid].setdefault(pc, record)
                if previous != record:
                    switches[sid][pc] = None  # Ambiguous ownership or table bounds.
    for function in metadata['functions']:
        if not function.get('discovery_errors'):
            add_switches(function['section'], function.get('indirect_transfers', []))
    from analysis.inspect_function_candidates import REVIEW_EVIDENCE

    for candidate in candidates['candidates']:
        body = candidate['body']
        # Diagnostic probes of neighbouring functions and original SDK code
        # must not supply runtime switch metadata on their own.
        if body['status'] == 'closed' and any(ref['kind'] not in REVIEW_EVIDENCE
                                              for ref in candidate['references']):
            add_switches(candidate['section'], body.get('local_switches', []))
    lines, regions = [], []
    for section in metadata['sections']:
        pc, size, offset = (section[key] for key in ('vram', 'size', 'rom_offset'))
        backed = prepared is cartridge or prepared[offset:offset+size] == cartridge[offset:offset+size]
        page_hashes = 'NULL'
        if not backed:
            page_hashes = f'cpu_pages_{section["id"]}'
            lines.append(f'static const uint8_t {page_hashes}[][32] = {{')
            for start in range(offset, offset + size, 256):
                digest = hashlib.sha256(prepared[start:min(start + 256, offset + size)]).digest()
                lines.append('    {' + ','.join(map(str, digest)) + '},')
            lines.append('};')
        spans = []
        for start, length in sorted(exclusions[section['name']]):
            if spans and start <= spans[-1][1]:
                spans[-1] = (spans[-1][0], max(spans[-1][1], start + length))
            else:
                spans.append((start, start + length))
        symbol = 'NULL'
        if spans:
            symbol = f'cpu_excluded_{section["id"]}'
            lines.append(f'static const RecompCodeSpan {symbol}[] = {{')
            lines.extend(f'    {{0x{lo:08x}u, 0x{hi-1:08x}u}},' for lo, hi in spans)
            lines.append('};')
        rows = []
        switch_symbol = 'NULL'
        for site, record in sorted(switches[section['id']].items()):
            if record is None:
                continue
            word, targets = record
            name = f'cpu_cases_{section["id"]}_{site:08x}'
            lines.append(f'static const uint32_t {name}[] = {{' +
                         ', '.join(f'0x{target:08x}u' for target in targets) + '};')
            rows.append(f'    {{0x{site:08x}u, 0x{word:08x}u, {name}, {len(targets)}}},')
        if rows:
            switch_symbol = f'cpu_switches_{section["id"]}'
            lines.extend([f'static const RecompCodeSwitch {switch_symbol}[] = {{', *rows, '};'])
        regions.append(f'    {{0x{pc:08x}u, 0x{size:x}u, 0x{offset:x}u, {page_hashes}, {symbol}, {len(spans)}, {switch_symbol}, {len(rows)}}},')
    lines.extend(['static const RecompCodeRegion cpu_regions[] = {', *regions, '};',
                  f'#define CPU_LAYOUT "{coverage["section_layout_sha256"]}"'])
    (output/'cpu_regions.h').write_text('\n'.join(lines)+'\n')


def load_generator(game):
    base = PROJECT/'games'/game
    sys.path.insert(0,str(base))
    spec=importlib.util.spec_from_file_location('game_generate',base/'generate.py')
    generator=importlib.util.module_from_spec(spec);spec.loader.exec_module(generator)
    return generator


def generate(game: str, output: Path, report_only: bool, renderer: str = "rt64", *, observed=()) -> None:
    with GenerationProgress(game) as progress:
        _generate(game, output, report_only, progress, renderer, observed)


def _generate(game: str, output: Path, report_only: bool, progress: GenerationProgress, renderer: str, observed) -> None:
    base = PROJECT/'games'/game
    profile = load_profile(base/'profile.toml')
    if observed:
        profile['discovery'].setdefault('observed_transfers', []).extend(observed)
    generator = load_generator(game)
    if hasattr(generator,'ensure_rom'):
        progress.stage('Checking external demo ROM download')
        generator.ensure_rom(profile)
    rom, _ = load_rom(profile['game']['rom'],profile['game']['rom_sha1'],profile['game']['rom_size'])
    original_rom=rom
    progress.stage('Checking generation cache')
    # Cache against ROM, Python implementation, and all checked game metadata.
    tools_dir = PROJECT / 'tools'
    inputs = sorted([*tools_dir.glob('*.py'), *tools_dir.glob('*.toml'), DATABASE,
                     tools_dir / 'analysis' / 'inspect_function_candidates.py',
                     *base.glob('*.toml'),*base.glob('*.json'),*base.glob('*.py')])
    digest = hashlib.sha256(rom)
    digest.update(renderer.encode())
    if observed:
        digest.update(json.dumps(observed, sort_keys=True).encode())
    for path in inputs:
        digest.update(str(path.relative_to(PROJECT)).encode());digest.update(path.read_bytes())
    key = digest.hexdigest()
    stamp = output/'generation.key'
    if not report_only and stamp.exists() and stamp.read_text()==key and (output/'sources.cmake').exists():
        outputs=json.loads((output/'outputs.json').read_text()) if (output/'outputs.json').exists() else []
        if outputs and all((output/name).is_file() for name in outputs):
            progress.stage('Using cached CPU sources (up to date)')
            return
    output.mkdir(parents=True,exist_ok=True)
    previous={p:(p.read_bytes(),p.stat().st_atime_ns,p.stat().st_mtime_ns)
              for pattern in ('*.c','*.h','sources.cmake') for p in output.glob(pattern)}
    original_profile=profile
    if hasattr(generator,'prepare'):
        progress.stage('Preparing ROM code and overlays')
        rom,profile=generator.prepare(rom,profile,output)
    progress.stage('Discovering functions')
    metadata=discover(rom,profile)
    metadata['cartridge_sha1'] = original_profile['game']['rom_sha1']
    progress.stage(f"Validating metadata for {len(metadata['functions'])} discovered functions")
    if hasattr(generator,'finalize_metadata'):generator.finalize_metadata(rom,metadata,profile)
    write_json(output/'symbols.json',metadata)
    metadata=load_metadata(output/'symbols.json',rom)
    from analysis.inspect_function_candidates import inspect_candidates
    progress.stage('Inspecting unregistered function candidates')
    candidates=inspect_candidates(rom,metadata)
    write_json(output/'function-candidates.json',candidates)
    print(f"{game}: diagnostic function candidates {candidates['summary']}; "
          f"see {output/'function-candidates.json'} "
          "(not registered automatically; build continues)", flush=True)
    if candidates['summary'].get('likely', 0) >= 100:
        print(f"{game}: candidate-review ranks missing call trees and indirect "
              "tables; candidate-review-roots and candidate-review-tables "
              "verify bounded batches and save accepted entries in profile.toml.",
              flush=True)
    if not report_only and hasattr(generator,'configure_translation'):
        progress.stage('Configuring translation')
        generator.configure_translation(rom,metadata,profile,output)
    # Complete-inventory ports retain the translator's checks for discovery
    # issues and unresolved boundaries; partial ports select registered code.
    names=None if getattr(generator,'REQUIRE_COMPLETE_INVENTORY',False) else [f['name'] for f in metadata['functions']]
    progress.stage('Analyzing CPU coverage' if report_only else 'Translating CPU functions to C')
    report=translate(rom,metadata,profile,output,names=names,report_only=report_only)
    print(f"{game}: {report['summary']}; {report['instruction_count']} instruction words",flush=True)
    progress.stage('Writing reports and game configuration')
    report_files = []
    if hasattr(generator, 'write_report'):
        report_files = generator.write_report(original_rom, original_profile, metadata, report, output) or []
    if not report_only:generator.write_config(original_profile,metadata,output,True)
    if report_only:return
    write_cpu_regions(output, original_rom, rom, profile, metadata, report, candidates)
    progress.stage('Splitting CPU sources for parallel compilation')
    sources=split_sources(output)
    # CMake reconfigures when this source manifest changes.
    manifest='set(RECOMP_CPU_SOURCES\n'+''.join(f'    "{p.as_posix()}"\n' for p in sources)+')\n'
    rsp_sources = []
    graphics_rsp = renderer == 'gles' and 'rsp' in original_profile.get('graphics', {})
    audio_rsp = original_profile.get('audio', {}).get('rsp')
    images = None
    jpeg = original_profile.get('jpeg')
    if graphics_rsp or audio_rsp or jpeg:
        if hasattr(generator, 'rsp_images'):
            images = generator.rsp_images(original_rom, original_profile)
        elif graphics_rsp and hasattr(generator, 'graphics_images'):
            images = generator.graphics_images(original_rom, original_profile)
    if graphics_rsp:
        from graphics_microcode import write_graphics_program
        progress.stage('Compiling RSP graphics microcode')
        rsp_source = write_graphics_program(original_profile, original_rom, output, images)
        manifest += f'set(RECOMP_RSP_SOURCE "{rsp_source.as_posix()}")\n'
        rsp_sources.append(rsp_source)
    if audio_rsp:
        from rsp_recompile import write_rsp_program
        progress.stage('Compiling RSP audio microcode')
        program = write_rsp_program(output, 'game_audio_rsp', audio_rsp['images'], original_rom, images)
        manifest += f'set(RECOMP_AUDIO_RSP_SOURCE "{program.as_posix()}")\n'
        rsp_sources.append(program)
    if jpeg:
        from rsp_recompile import checked_span, write_rsp_program
        progress.stage('Compiling RSP JPEG microcode')
        boot_images = {entry['name']: entry for entry in original_profile['graphics']['rsp']['images']}
        entries = [boot_images[name] for name in jpeg['rsp']['bootstraps']] + jpeg['rsp']['images']
        program = write_rsp_program(output, 'game_jpeg_rsp', entries, original_rom, images)
        constants = checked_span('JPEG data', jpeg['data'], original_rom, images, jpeg['descriptor'][5])
        with program.open('a') as stream:
            stream.write('#include "recomp_jpeg.h"\n')
            stream.write('static const uint8_t jpeg_data[] = {' + ','.join(map(str, constants)) + '};\n')
            descriptor = ', '.join(hex(value) for value in jpeg['descriptor'])
            stream.write('const RecompJpegProfile game_jpeg_profile = {'
                         f'{{{descriptor}}}, jpeg_data, sizeof(jpeg_data), &game_jpeg_rsp_program}};\n')
        manifest += f'set(RECOMP_JPEG_RSP_SOURCE "{program.as_posix()}")\n'
        rsp_sources.append(program)
    (output/'sources.cmake').write_text(manifest)
    progress.stage('Finalizing generated files and cache')
    outputs=[p.name for p in [*report_files,*sources,*rsp_sources,*output.glob('*.h'),output/'sources.cmake',
                             output/'symbols.json',output/'coverage.json',output/'function-candidates.json']]
    (output/'outputs.json').write_text(json.dumps(outputs))
    # Input changes can leave CPU bytes identical. Preserve their timestamps so
    # build tools only recompile files whose content actually changed.
    for path,(content,atime,mtime) in previous.items():
        if path.exists() and path.read_bytes()==content:os.utime(path,ns=(atime,mtime))
    stamp.write_text(key)


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game',required=True,choices=GAMES)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--renderer',choices=('rt64','gles'),default='rt64')
    parser.add_argument('--report-only',action='store_true')
    args=parser.parse_args()
    try:generate(args.game,args.output.resolve(),args.report_only,args.renderer)
    except (RecompError,OSError,ValueError,KeyError) as error:
        print(f'generation failed: {error}',file=sys.stderr);return 1
    return 0

if __name__=='__main__':sys.exit(main())
