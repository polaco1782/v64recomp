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
from common import RecompError, load_profile, load_rom, load_metadata, write_json
from discover import discover
from translate import translate

PROJECT = Path(__file__).resolve().parents[1]
GAMES = ('sm64-us','goldeneye-us','shadowman-us','quake-us','duke-us','nightmare-us','oot-us-v12','perfectdark-us-v11')


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
    base = PROJECT/'games'/game
    profile = load_profile(base/('full.toml' if game=='sm64-us' else 'startup.toml'))
    rom, _ = load_rom(profile['game']['rom'],profile['game']['rom_sha1'],profile['game']['rom_size'])
    # Cache against ROM, Python implementation, and all checked game metadata.
    inputs = sorted([*Path(__file__).parent.glob('*.py'),*Path(__file__).parent.glob('*.toml'),
                     *base.glob('*.toml'),*base.glob('*.json'),*base.joinpath('release').glob('*.py')])
    digest = hashlib.sha256(rom)
    for path in inputs:
        digest.update(str(path.relative_to(PROJECT)).encode());digest.update(path.read_bytes())
    key = digest.hexdigest()
    stamp = output/'generation.key'
    if not report_only and stamp.exists() and stamp.read_text()==key and (output/'sources.cmake').exists():
        outputs=json.loads((output/'outputs.json').read_text()) if (output/'outputs.json').exists() else []
        if outputs and all((output/name).is_file() for name in outputs):return
    output.mkdir(parents=True,exist_ok=True)
    previous={p:(p.read_bytes(),p.stat().st_atime_ns,p.stat().st_mtime_ns)
              for pattern in ('*.c','*.h','sources.cmake') for p in output.glob(pattern)}
    sys.path.insert(0,str(base/'release'))
    spec=importlib.util.spec_from_file_location('game_generate',base/'release/generate.py')
    generator=importlib.util.module_from_spec(spec);spec.loader.exec_module(generator)
    original_profile=profile
    if game=='sm64-us':
        generator.prepare_full(output,report_only)
    else:
        if game=='goldeneye-us':
            generator.validate_idle_loop(rom,profile)
            generator.validate_inspection(rom,profile)
            generator.validate_cheats(rom,profile)
            generator.checked_dispatches(rom,profile)
        if hasattr(generator,'prepare'):rom,profile=generator.prepare(rom,profile,output)
        metadata=discover(rom,profile)
        if hasattr(generator,'finalize_metadata'):generator.finalize_metadata(rom,metadata,profile)
        for function in metadata['functions']:
            sites=profile.get('oot_relocations',{}).get(function['section'])
            if sites is not None:
                function['relocatable']=True
                owned=set(function['instruction_addresses'])
                function['relocation_pcs']=[pc for pc in sites if pc in owned]
        write_json(output/'symbols.json',metadata)
        metadata=load_metadata(output/'symbols.json',rom)
        if not report_only and hasattr(generator,'configure_translation'):
            generator.configure_translation(rom,metadata,profile,output)
        if game=='goldeneye-us' and not report_only:
            audit=translate(rom,metadata,profile,output,report_only=True)
            functions={(f['section'],f['vram']):f for f in metadata['functions']}
            for f in audit['functions']:
                if f['status']!='unsupported':continue
                original=functions[f['section'],f['vram']];offset=original['rom_offset']
                profile.setdefault('boundaries',[]).append(dict(symbol=original['name'],vram=original['vram'],
                    kind='unsupported_function',reason='Pending GoldenEye conversion: '+f['errors'][0]['reason'],
                    expected_words=[int.from_bytes(rom[offset:offset+4],'big')]))
            write_json(output/'pending-boundaries.json',{'complete_conversion':False,'boundaries':profile.get('boundaries',[])})
        report=translate(rom,metadata,profile,output,names=[f['name'] for f in metadata['functions']],report_only=report_only)
        print(f"{game}: {report['summary']}; {report['instruction_count']} instruction words",flush=True)
        if not report_only:
            if game=='goldeneye-us':generator.write_boot_config(profile,metadata,output)
            else:generator.write_config(original_profile,metadata,output,True)
    if report_only:return
    sources=split_sources(output)
    # CMake reconfigures when this source manifest changes.
    manifest='set(RECOMP_CPU_SOURCES\n'+''.join(f'    "{p.as_posix()}"\n' for p in sources)+')\n'
    (output/'sources.cmake').write_text(manifest)
    outputs=[p.name for p in [*sources,*output.glob('*.h'),output/'sources.cmake']]
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
