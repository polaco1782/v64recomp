#!/usr/bin/env python3
"""Compile one newly discovered function for a paused v64recomp process.

Edit the game's checked profile/inventory first. This command verifies the
original ROM, runs the normal game preparation and discovery hooks, and only
publishes a module after both translation and native compilation succeed.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid

from common import RecompError, load_metadata, load_profile, load_rom, write_json
from discover import discover
from emit import FunctionRegistry, cname, decode, decode_function, emit_function, instruction_addresses, resolve_function, target
from generate_game import GAMES, PROJECT
from translate import translate


def verified_leaf_table_reference(rom: bytes, metadata: dict, candidate: dict) -> bool:
    """Corroborate a frameless leaf with its exact ROM table slot and caller."""
    if candidate.get('body', {}).get('direct_calls') != []:
        return False
    sections = metadata['sections']
    functions = metadata['functions']
    for ref in candidate.get('references', []):
        if ref.get('kind') != 'indexed-table-call':
            continue
        sid, index, base = ref.get('table_section'), ref.get('table_index'), ref.get('table_vram')
        source_sid, pc, load_pc = ref.get('section'), ref.get('pc'), ref.get('load_pc')
        if (type(sid) is not int or not 0 <= sid < len(sections)
                or type(index) is not int or index < 0 or type(base) is not int
                or type(source_sid) is not int or not 0 <= source_sid < len(sections)
                or type(pc) is not int or type(load_pc) is not int):
            continue
        table = sections[sid]
        slot = base + index * 4
        offset = table['rom_offset'] + slot - table['vram']
        if (slot < table['vram'] or slot + 4 > table['vram'] + table['size']
                or offset != ref.get('rom_offset')
                or int.from_bytes(rom[offset:offset + 4], 'big') != candidate['vram']):
            continue
        owners = [f for f in functions if f['section'] == source_sid
                  and pc in instruction_addresses(f) and load_pc in instruction_addresses(f)]
        if len(owners) != 1 or owners[0]['name'] != ref.get('function'):
            continue
        source = sections[source_sid]
        call_offset = source['rom_offset'] + pc - source['vram']
        load_offset = source['rom_offset'] + load_pc - source['vram']
        call = decode_function(rom, {'vram': pc, 'size': 4, 'rom_offset': call_offset})[0]
        load = decode_function(rom, {'vram': load_pc, 'size': 4, 'rom_offset': load_offset})[0]
        if (call.word == ref.get('expected_word') and call.opcode == 'jalr'
                and load.opcode == 'lw' and load.rt == call.rs):
            return True
    return False


def verified_published_direct_caller(rom: bytes, candidate: dict, patch_dir: Path) -> bool:
    """Accept a closed nested function called by an already published module."""
    for ref in candidate.get('references', []):
        if ref.get('kind') != 'candidate-direct-call':
            continue
        source, section, pc, offset = (ref.get(key) for key in
                                       ('candidate', 'section', 'pc', 'rom_offset'))
        if any(type(value) is not int for value in (source, section, pc, offset)):
            continue
        control = patch_dir / f'function-{source:08x}.path'
        if not control.is_file() or control.is_symlink():
            continue
        module = Path(control.read_text().strip())
        if (not module.is_absolute() or not module.is_file()
                or module.parent.resolve() != patch_dir.resolve()):
            continue
        match = re.fullmatch(rf'module-{source:08x}-([0-9a-f]{{32}})\.so', module.name)
        if not match:
            continue
        snapshot = patch_dir / '.build' / f'{source:08x}-{match[1]}' / 'symbols.json'
        if not snapshot.is_file():
            continue
        metadata = json.loads(snapshot.read_text())
        if metadata.get('rom_sha1') != hashlib.sha1(rom).hexdigest():
            continue
        owners = [f for f in metadata['functions'] if f['vram'] == source and f['section'] == section
                  and pc in instruction_addresses(f)]
        if len(owners) != 1 or offset != owners[0]['rom_offset'] + pc - source:
            continue
        instruction = decode(rom[offset:offset + 4], pc)[0]
        if instruction.opcode == 'jal' and target(instruction) == candidate['vram']:
            return True
    return False


def cache_value(path: Path, key: str) -> str:
    for line in (path / 'CMakeCache.txt').read_text().splitlines():
        if line.startswith(key + ':'):
            return line.split('=', 1)[1]
    raise RecompError(f'{key} missing from {path / "CMakeCache.txt"}')


def automatic_candidate(rom: bytes, profile: dict, address: int, build_dir: Path,
                        patch_dir: Path, game: str) -> dict | None:
    """Seed one ROM-bound, closed candidate for a temporary live module."""
    report_path = build_dir / 'generated' / game / 'function-candidates.json'
    report = json.loads(report_path.read_text())
    if report.get('rom_sha1') != hashlib.sha1(rom).hexdigest():
        raise RecompError('candidate report does not match the prepared ROM')
    candidates = [item for item in report.get('candidates', []) if item.get('vram') == address]
    if len(candidates) != 1:
        return None
    candidate = candidates[0]
    if candidate.get('registered'):
        return None
    body = candidate.get('body', {})
    references = candidate.get('references', [])
    leaf_table_call = (not body.get('stack_prologue')
                       and verified_leaf_table_reference(
                           rom, load_metadata(report_path.with_name('symbols.json'), rom), candidate))
    published_call = verified_published_direct_caller(rom, candidate, patch_dir)
    # Discovery records these calls as registered-target transfers; live lookup
    # can pause again if the nested call reaches another missing function.
    dynamic_calls_allowed = profile['discovery'].get('allow_registered_indirect_calls') is True
    reasons = []
    if candidate.get('confidence') != 'likely':
        reasons.append(f"confidence is {candidate.get('confidence', 'unknown')}")
    if body.get('status') != 'closed':
        reasons.append(f"body is {body.get('status', 'unproven')}")
    if body.get('issues'):
        reasons.append('; '.join(issue.get('reason', 'unknown flow issue')
                                 for issue in body['issues'][:2]))
    if not (body.get('stack_prologue') or leaf_table_call or published_call):
        reasons.append('no stack frame, verified table leaf, or published caller')
    if body.get('indirect_calls') and not dynamic_calls_allowed:
        reasons.append('nested indirect call is not enabled')
    if body.get('instructions', 0) < (2 if leaf_table_call else 4):
        reasons.append('body is too short for this evidence')
    if candidate.get('reference_count', 0) < 2 and not published_call:
        reasons.append('fewer than two references')
    if not (published_call or any(
            ref.get('kind') in ('materialized-address', 'indexed-table-call', 'image-pointer')
            for ref in references)):
        reasons.append('no verified address source or published caller')
    if reasons:
        raise RecompError('candidate found but needs manual review: ' + '; '.join(reasons))
    regions = profile['discovery']['regions']
    section = candidate.get('section')
    offset = candidate.get('rom_offset')
    if (type(section) is not int or not 0 <= section < len(regions)
            or type(offset) is not int or offset < 0 or offset + 16 > len(rom)):
        raise RecompError('candidate has invalid section or ROM offset')
    region = regions[section]
    if (region['rom_offset'] + address - region['vram'] != offset
            or not region['vram'] <= address < region['vram'] + region['size']
            or hashlib.sha256(rom[offset:offset + 16]).hexdigest() != candidate.get('sha256')):
        raise RecompError('candidate bytes or mapping changed')
    return {'name': f'live_candidate_{address:08x}', 'vram': address, 'section': region['name']}


def build(game: str, address: int, build_dir: Path, patch_dir: Path,
          site: int | None = None, section: int | None = None,
          site_section: int | None = None, auto_candidate: bool = False) -> Path:
    if cache_value(build_dir, 'RECOMP_GAME') != game:
        raise RecompError('build directory belongs to a different game')
    mode = cache_value(build_dir, 'CMAKE_BUILD_TYPE')
    if mode not in ('Debug', 'Release'):
        raise RecompError('live module requires a Debug or Release game build')
    base = PROJECT / 'games' / game
    profile = load_profile(base / 'profile.toml')
    original_rom, _ = load_rom(profile['game']['rom'], profile['game']['rom_sha1'], profile['game']['rom_size'])
    patch_dir.mkdir(parents=True, exist_ok=True)
    token = uuid.uuid4().hex
    output = patch_dir / '.build' / f'{address:08x}-{token}'
    output.mkdir(parents=True)
    sys.path.insert(0, str(base))
    spec = importlib.util.spec_from_file_location(f'live_{game.replace("-", "_")}', base / 'generate.py')
    if spec is None or spec.loader is None:
        raise RecompError('game generate.py cannot be loaded')
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    rom, prepared = original_rom, profile
    if hasattr(generator, 'prepare'):
        rom, prepared = generator.prepare(rom, prepared, output)
    if auto_candidate:
        root = automatic_candidate(rom, prepared, address, build_dir, patch_dir, game)
        if root:
            prepared = {**prepared, 'discovery': {**prepared['discovery'],
                        'roots': [*prepared['discovery'].get('roots', []), root]}}
    metadata = discover(rom, prepared)
    if hasattr(generator, 'finalize_metadata'):
        generator.finalize_metadata(rom, metadata, prepared)
    write_json(output / 'symbols.json', metadata)
    metadata = load_metadata(output / 'symbols.json', rom)
    if hasattr(generator, 'configure_translation'):
        generator.configure_translation(rom, metadata, prepared, output)
    resume = False
    jump = False
    if site is not None:
        owners = [f for f in metadata['functions'] if site in instruction_addresses(f)
                  and (site_section is None or f['section'] == site_section)]
        if len(owners) != 1:
            raise RecompError(f'checked transfer site 0x{site:08x} is missing or ambiguous')
        owner = owners[0]
        transfers = [t for t in owner.get('indirect_transfers', [])
                     if t['pc'] == site and address in t['targets']]
        if len(transfers) != 1 or transfers[0]['kind'] not in ('call', 'jump', 'tail'):
            raise RecompError('the updated checked transfer does not authorize this target')
        jump = transfers[0]['kind'] != 'call'
        resume = jump and address in instruction_addresses(owner)
        if resume and section is not None and owner['section'] != section:
            raise RecompError('local target does not belong to the selected section')
    matches = [owner] if resume else [f for f in metadata['functions'] if f['vram'] == address
                                      and (section is None or f['section'] == section)]
    if len(matches) != 1:
        raise RecompError(f'0x{address:08x} is missing or ambiguous; add a checked function entry')
    function = matches[0]
    registry = FunctionRegistry(metadata['functions'])
    if site is not None and not resume and resolve_function(registry, owner['section'], address) is not function:
        raise RecompError('target does not resolve to the selected function from this site')
    if any(item.get('vram') == address for item in [*prepared.get('hooks', []), *prepared.get('boundaries', [])]):
        raise RecompError('live repair target must be translated guest code')
    report = translate(rom, metadata, prepared, output, names=[function['name']], live_mode=True)
    selected = [f for f in report['functions'] if f['selected']]
    if len(selected) != 1 or selected[0]['status'] != 'ready':
        raise RecompError('selected function did not pass translation checks')
    if resume:
        resume_code = emit_function(function, decode_function(rom, function), registry,
                                    direct_targets={}, resume_entry=True)
        with (output / 'functions.c').open('a') as stream:
            stream.write('\n\n' + resume_code + '\n')
    calls = sorted({(transfer['pc'], address, f['section'])
                    for f in metadata['functions'] for transfer in f.get('indirect_transfers', [])
                    if transfer['kind'] == 'call' and address in transfer['targets']
                    and resolve_function(registry, f['section'], address) is function})
    jumps = [(site, address, owner['section'])] if jump and site is not None else []
    sha = bytes.fromhex(profile['game']['rom_sha1'])
    lines = ['#include "generated.h"', '#include "recomp_live.h"']
    if calls:
        lines += ['static const RecompLiveTransfer allowed_calls[] = {']
        lines += [f'    {{UINT32_C(0x{source:08x}), UINT32_C(0x{target:08x}), {source_section}}},'
                  for source, target, source_section in calls]
        lines += ['};']
    if jumps:
        lines += ['static const RecompLiveTransfer allowed_jumps[] = {']
        lines += [f'    {{UINT32_C(0x{source:08x}), UINT32_C(0x{target:08x}), {source_section}}},'
                  for source, target, source_section in jumps]
        lines += ['};']
    if resume:
        lines += [f'void {cname(function)}_live_resume(Runtime *, MipsContext *, uint32_t, uint32_t, uint32_t);']
    lines += ['const RecompLiveManifest recomp_live_manifest = {',
              '    .abi = RECOMP_LIVE_ABI,', f'    .debug_build = {1 if mode == "Debug" else 0},',
              '    .runtime_size = sizeof(Runtime), .context_size = sizeof(MipsContext),',
              '    .entry_size = sizeof(RecompFunctionEntry),',
              '    .rom_sha1 = {' + ', '.join(f'0x{byte:02x}' for byte in sha) + '},',
              '    .functions = recomp_functions, .function_count = 1,',
              f'    .calls = {"allowed_calls" if calls else "NULL"}, .call_count = {len(calls)},',
              f'    .jumps = {"allowed_jumps" if jumps else "NULL"}, .jump_count = {len(jumps)},',
              f'    .resume = {cname(function) + "_live_resume" if resume else "NULL"},', '};']
    (output / 'manifest.c').write_text('\n'.join(lines) + '\n')
    compiler = cache_value(build_dir, 'CMAKE_C_COMPILER')
    if not Path(compiler).is_file():
        compiler = shutil.which(compiler) or ''
    if not compiler:
        raise RecompError('C compiler from build directory is unavailable')
    module = patch_dir / f'module-{address:08x}-{token}.so'
    command = [compiler, '-std=c11', '-shared', '-fPIC', '-Wl,-Bsymbolic',
               '-fno-fast-math', '-frounding-math', '-ffp-contract=off',
               '-Wall', '-Wextra', '-Werror', '-Wno-unused-label',
               '-DRECOMP_WITH_RT64=1', '-DRECOMP_WITH_AUDIO=1',
               '-I', str(PROJECT / 'include'), '-I', str(output),
               '-O0' if mode == 'Debug' else '-O2']
    if mode == 'Debug':
        command += ['-DRECOMP_DEBUG=1', '-g3']
    command += [str(output / 'functions.c'), str(output / 'manifest.c'), '-o', str(module)]
    print('Compiling checked live function...', flush=True)
    subprocess.run(command, check=True)
    control = patch_dir / f'function-{address:08x}.path'
    temporary = patch_dir / f'.function-{address:08x}-{token}.path'
    temporary.write_text(str(module.resolve()) + '\n')
    os.replace(temporary, control)
    print(f'Published {module} for target 0x{address:08x}', flush=True)
    return module


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', choices=GAMES, required=True)
    parser.add_argument('--address', required=True, type=lambda value: int(value, 0))
    parser.add_argument('--site', type=lambda value: int(value, 0),
                        help='checked indirect call/jump PC; required to repair a local jump')
    parser.add_argument('--section', type=lambda value: int(value, 0),
                        help='target section ID when overlapping overlays share an address')
    parser.add_argument('--site-section', type=lambda value: int(value, 0),
                        help='source section ID when overlapping overlays share the site PC')
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--patch-dir', type=Path,
                        help='control directory (default: local/live-patches/<game>)')
    parser.add_argument('--auto-candidate', action='store_true',
                        help='temporarily seed a verified closed candidate without editing the profile')
    args = parser.parse_args()
    if (not 0 <= args.address <= 0xffffffff or args.address % 4 or
            args.site is not None and (not 0 <= args.site <= 0xffffffff or args.site % 4) or
            args.section is not None and args.section < 0 or
            args.site_section is not None and args.site_section < 0):
        parser.error('address and site must be aligned 32-bit guest addresses')
    try:
        patch_dir = args.patch_dir or PROJECT / 'local' / 'live-patches' / args.game
        build(args.game, args.address, args.build_dir.resolve(), patch_dir.resolve(),
              args.site, args.section, args.site_section, args.auto_candidate)
    except (RecompError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f'live repair failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
