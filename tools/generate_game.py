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
GAMES = ('sm64-us','goldeneye-us','shadowman-us','quake-us','duke-us','nightmare-us','oot-us-v12','perfectdark-us-v11','conker-us','duke-zero-hour-us','hexen-us','rayman2-us','quake2-us','re2-us-v1','turok-us-v12')


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


def generate(game: str, output: Path, report_only: bool) -> None:
    with GenerationProgress(game) as progress:
        _generate(game, output, report_only, progress)


def _generate(game: str, output: Path, report_only: bool, progress: GenerationProgress) -> None:
    base = PROJECT/'games'/game
    profile = load_profile(base/'profile.toml')
    rom, _ = load_rom(profile['game']['rom'],profile['game']['rom_sha1'],profile['game']['rom_size'])
    original_rom=rom
    progress.stage('Checking generation cache')
    # Cache against ROM, Python implementation, and all checked game metadata.
    inputs = sorted([*Path(__file__).parent.glob('*.py'),*Path(__file__).parent.glob('*.toml'),DATABASE,
                     *base.glob('*.toml'),*base.glob('*.json'),*base.glob('*.py')])
    digest = hashlib.sha256(rom)
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
    sys.path.insert(0,str(base))
    spec=importlib.util.spec_from_file_location('game_generate',base/'generate.py')
    generator=importlib.util.module_from_spec(spec);spec.loader.exec_module(generator)
    original_profile=profile
    if hasattr(generator,'prepare'):
        progress.stage('Preparing ROM code and overlays')
        rom,profile=generator.prepare(rom,profile,output)
    progress.stage('Discovering functions')
    metadata=discover(rom,profile)
    progress.stage(f"Validating metadata for {len(metadata['functions'])} discovered functions")
    if hasattr(generator,'finalize_metadata'):generator.finalize_metadata(rom,metadata,profile)
    write_json(output/'symbols.json',metadata)
    metadata=load_metadata(output/'symbols.json',rom)
    from inspect_function_candidates import inspect_candidates
    progress.stage('Inspecting unregistered function candidates')
    candidates=inspect_candidates(rom,metadata)
    write_json(output/'function-candidates.json',candidates)
    print(f"{game}: unregistered function candidates {candidates['summary']}; "
          f"see {output/'function-candidates.json'} (review required)",flush=True)
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
    if hasattr(generator,'write_report'):generator.write_report(original_rom,original_profile,metadata,report,output)
    if not report_only:generator.write_config(original_profile,metadata,output,True)
    if report_only:return
    progress.stage('Splitting CPU sources for parallel compilation')
    sources=split_sources(output)
    # CMake reconfigures when this source manifest changes.
    manifest='set(RECOMP_CPU_SOURCES\n'+''.join(f'    "{p.as_posix()}"\n' for p in sources)+')\n'
    (output/'sources.cmake').write_text(manifest)
    progress.stage('Finalizing generated files and cache')
    outputs=[p.name for p in [*sources,*output.glob('*.h'),output/'sources.cmake',
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
    parser.add_argument('--report-only',action='store_true')
    args=parser.parse_args()
    try:generate(args.game,args.output.resolve(),args.report_only)
    except (RecompError,OSError,ValueError,KeyError) as error:
        print(f'generation failed: {error}',file=sys.stderr);return 1
    return 0

if __name__=='__main__':sys.exit(main())
