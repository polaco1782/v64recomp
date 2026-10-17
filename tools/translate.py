#!/usr/bin/env python3
"""Validate ROM/metadata, report coverage, and emit compiled-function C."""
from __future__ import annotations

import argparse
from collections import Counter
import json
import os
import re
import sys
from pathlib import Path

from common import RecompError, load_metadata, load_profile, load_rom, validate_hook_prefix, write_json
from emit import FunctionRegistry, analyze, cname, decode_function, direct_call_targets, emit_function, instruction_addresses, log_emission, resolve_function, target
from native_hooks import resolve_native_hooks


def translate(rom: bytes, metadata: dict, profile: dict, output: Path,
              names: list[str] | None = None, report_only: bool = False) -> dict:
    profile = dict(profile, hooks=resolve_native_hooks(rom, profile))
    output.mkdir(parents=True, exist_ok=True)
    previous={}
    if not report_only:
        # A failed generation must not leave previous code that appears current.
        for filename in ('functions.c', 'generated.h'):
            path=output/filename
            if path.exists():previous[filename]=(path.read_bytes(),path.stat().st_atime_ns,path.stat().st_mtime_ns)
            (output / filename).unlink(missing_ok=True)
    functions = metadata['functions']
    fpu_fr = profile['game'].get('fpu_fr', 0)
    if type(fpu_fr) is not int or fpu_fr not in (0, 1):
        raise RecompError('game fpu_fr must be zero or one')
    log_emission('Start %s: %d functions, %d sections; output %s',
                 'coverage analysis' if report_only else 'translation', len(functions), len(metadata['sections']), output)
    registry = FunctionRegistry(functions)
    hooks = {}
    for hook in profile.get('hooks', []):
        if hook.get('kind') != 'replace_function':
            raise RecompError(f"hook kind not implemented: {hook.get('kind')}")
        if not re.fullmatch(r"[a-zA-Z_]\w*", hook.get('handler', '')):
            raise RecompError("hook handler must be a C identifier")
        matches = [f for f in functions if hook['symbol'] in [f['name'], *f['aliases']]]
        if len(matches) != 1:
            raise RecompError(f"hook symbol missing or ambiguous: {hook['symbol']}")
        f = matches[0]
        validate_hook_prefix(rom, f['rom_offset'], f['vram'], f['size'], hook)
        key = (f['section'], f['vram'])
        if key in hooks:
            raise RecompError(f"duplicate hook: {hook['symbol']}")
        hooks[key] = hook['handler']
        log_emission('Verified hook %s s%s 0x%08x (ROM 0x%08x) -> %s',
                     hook['symbol'], f['section'], f['vram'], f['rom_offset'], hook['handler'])
    boundaries = {}
    for boundary in profile.get('boundaries', []):
        if (boundary.get('kind') != 'unsupported_function' or 'vram' not in boundary
                or not isinstance(boundary.get('reason'), str) or not boundary['reason']):
            raise RecompError('unsupported boundary requires a kind and reason')
        matches = [f for f in functions if boundary['symbol'] in [f['name'], *f['aliases']]]
        if len(matches) != 1:
            raise RecompError(f"boundary symbol missing or ambiguous: {boundary['symbol']}")
        f = matches[0]; key = (f['section'], f['vram'])
        validate_hook_prefix(rom, f['rom_offset'], f['vram'], f['size'], boundary)
        if key in hooks or key in boundaries:
            raise RecompError(f"duplicate hook/boundary: {boundary['symbol']}")
        boundaries[key] = boundary['reason']
        log_emission('Verified boundary %s s%s 0x%08x: %s',
                     f['name'], f['section'], f['vram'], boundary['reason'])
    selected = list(functions)
    if names:
        by_name = {}
        for f in functions:
            for name in set([f['name'], *f['aliases']]):
                by_name.setdefault(name, []).append(f)
        selected = []
        seen = set()
        for name in names:
            matches = by_name.get(name, [])
            if len(matches) != 1:
                raise RecompError(f"selected function missing or ambiguous: {name}")
            key = (matches[0]['section'], matches[0]['vram'])
            if key not in seen:
                selected.append(matches[0])
                seen.add(key)
    selected_keys = {(f['section'], f['vram']) for f in selected}
    log_emission('Selected %d functions; decoding and checking %d functions', len(selected), len(functions))
    results, decoded, counts = [], {}, Counter()
    for index, f in enumerate(functions, 1):
        key = (f['section'], f['vram'])
        instructions = decode_function(rom, f)
        if any(t.get('registered_targets') for t in f.get('indirect_transfers', [])) and not profile.get('discovery',{}).get('allow_registered_indirect_calls'):
            raise RecompError('registered-target call metadata requires the profile policy')
        decoded[key] = instructions
        counts.update(i.opcode for i in instructions)
        errors = [] if key in hooks else ([{'pc': f['vram'], 'word': instructions[0].word, 'assembly': '',
                                          'reason': 'unsupported execution boundary: ' + boundaries[key]}] if key in boundaries
                                         else [*f.get('discovery_errors', []), *analyze(f, instructions, registry, fpu_fr)])
        if key in selected_keys and not report_only and key not in hooks and key not in boundaries:
            for i in instructions:
                if i.delay and i.opcode not in ('jr', 'jalr'):
                    address = target(i)
                    local = address in instruction_addresses(f) and i.opcode != 'jal'
                    callee = resolve_function(registry, f['section'], address)
                    if not local and callee and (callee['section'], callee['vram']) not in selected_keys:
                        errors.append({'pc': i.pc, 'word': i.word, 'assembly': i.assembly,
                                       'reason': f"callee not selected: {callee['name']}"})
            for transfer in f.get('fallthrough_transfers', []):
                callee=resolve_function(registry,f['section'],transfer['target'])
                if callee and (callee['section'],callee['vram']) not in selected_keys:
                    errors.append({'pc':transfer['pc'],'word':transfer['expected_word'],'assembly':'',
                                   'reason':f"fallthrough callee not selected: {callee['name']}"})
            for transfer in f.get('indirect_transfers', []):
                if transfer['kind'] == 'jump':
                    continue
                for address in transfer['targets']:
                    callee = resolve_function(registry, f['section'], address)
                    if callee and (callee['section'], callee['vram']) not in selected_keys:
                        errors.append({'pc': transfer['pc'], 'word': transfer['expected_word'], 'assembly': '',
                                       'reason': f"indirect callee not selected: {callee['name']}"})
        results.append({'name': f['name'], 'section': f['section'], 'vram': f['vram'],
                        'size': f['size'], 'selected': key in selected_keys,
                        'status': 'hooked' if key in hooks else 'boundary' if key in boundaries else 'unsupported' if errors else 'ready',
                        'errors': errors})
        if key in selected_keys:
            for error in errors:
                log_emission('Issue %s s%s 0x%08x: %s (%s)',
                             f['name'], f['section'], error['pc'], error['reason'], error.get('assembly', ''))
        if index % 100 == 0 or index == len(functions):
            log_emission('Analyzed %d/%d functions; %d instructions', index, len(functions), sum(counts.values()))
    yield_sites = {}
    checkpoints = profile.get('checkpoints', [])
    for site in checkpoints:
        pc = site['pc']
        matches = [(key, f, i) for f in functions for key in [(f['section'], f['vram'])]
                   for i in decoded[key] if i.pc == pc and ('section' not in site or site['section'] == f['section'])]
        if len(matches) != 1:
            raise RecompError(f'checkpoint missing or ambiguous: {pc:#010x}')
        key, f, i = matches[0]
        if (site.get('kind') != 'yield' or site.get('expected_word') != i.word or key in hooks or key in boundaries
                or not i.delay or i.opcode in ('jr', 'jalr', 'jal') or i.opcode not in
                ('j', 'beq', 'bne', 'blez', 'bgtz', 'bltz', 'bgez', 'beql', 'bnel', 'blezl', 'bgtzl', 'bltzl', 'bgezl', 'b')
                or target(i) > pc or target(i) not in instruction_addresses(f)):
            raise RecompError(f'invalid or mismatched yield checkpoint: {pc:#010x}')
        sites = yield_sites.setdefault(key, set())
        if pc in sites:
            raise RecompError(f'duplicate yield checkpoint: {pc:#010x}')
        sites.add(pc)
    report = {'format_version': 1, 'rom_sha1': metadata['rom_sha1'],
              'summary': dict(Counter(f['status'] for f in results)),
              'instruction_count': sum(counts.values()), 'opcodes': dict(counts),
              'metadata_issues': metadata.get('issues', []), 'functions': results,
              'code_inventory':metadata.get('code_inventory'),
              'checkpoints': checkpoints, 'generated': False}
    write_json(output / 'coverage.json', report)
    log_emission('Coverage: %s; wrote %s', report['summary'], output / 'coverage.json')
    if report_only:
        return report
    if any(f['selected'] and f['errors'] and f['status'] != 'boundary' for f in results):
        raise RecompError(f"selected functions have unsupported instructions/targets; see {output / 'coverage.json'}")
    if not names and metadata.get('issues'):
        raise RecompError("unclassified bytes or unresolved discovery issues prevent full generation; see coverage report")
    if not names and boundaries:
        raise RecompError('unsupported execution boundaries prevent full generation')
    header = ['#ifndef GENERATED_RECOMP_H', '#define GENERATED_RECOMP_H', '#include "recomp.h"', '']
    source = ['/* Generated from validated MIPS bytes; do not edit. */', '#include "generated.h"', '']
    log_emission('Memory lowering uses recomp_load/store helpers (RDRAM and supported native registers). '
                 'OS/HW services use the verified native hooks.')
    direct_targets = direct_call_targets(registry, hooks, selected)
    for f in selected:
        key = (f['section'], f['vram'])
        symbol = hooks.get(key, cname(f))
        header.append(f"void {symbol}(Runtime *, MipsContext *);")
        if key in boundaries:
            log_emission('Emit failure boundary %s s%s 0x%08x: %s',
                         f['name'], f['section'], f['vram'], boundaries[key])
            source.append(f'void {symbol}(Runtime *runtime, MipsContext *ctx) {{\n'
                          f'    (void)runtime; (void)ctx;\n'
                          f'    RECOMP_TRACK_PC(ctx, UINT32_C(0x{f["vram"]:08x}));\n'
                          f'    recomp_fail({json.dumps(boundaries[key])}, UINT32_C(0x{f["vram"]:08x}));\n}}')
        elif key in hooks:
            log_emission('Bind native hook %s s%s 0x%08x -> %s', f['name'], f['section'], f['vram'], symbol)
        else:
            source.append(emit_function(f, decoded[key], registry, yield_sites.get(key), hooks, direct_targets))
    source.append('const RecompFunctionEntry recomp_functions[] = {')
    for f in selected:
        symbol = hooks.get((f['section'], f['vram']), cname(f))
        safe_name = re.sub(r'[^a-zA-Z0-9_.$]', '_', f['name'])
        source.append(f'    {{UINT32_C(0x{f["vram"]:08x}), {f["size"]}, {f["section"]}, {symbol}, "{safe_name}"}},')
    source.extend(['};', 'const size_t recomp_function_count = sizeof(recomp_functions) / sizeof(recomp_functions[0]);',
                   'const bool recomp_boot_sections[] = {' + ', '.join('true' if s['loaded_at_boot'] else 'false' for s in metadata['sections']) + '};',
                   'const size_t recomp_section_count = sizeof(recomp_boot_sections) / sizeof(recomp_boot_sections[0]);'])
    header.extend(['extern const RecompFunctionEntry recomp_functions[];', 'extern const size_t recomp_function_count;',
                   'extern const bool recomp_boot_sections[];', 'extern const size_t recomp_section_count;', '#endif'])
    for filename,value in (('generated.h','\n'.join(header)+'\n'),('functions.c','\n\n'.join(source)+'\n')):
        path=output/filename;raw=value.encode();path.write_bytes(raw)
        # Failed generations still remove stale output. Successful identical
        # generations retain timestamps so Ninja need not rebuild the large C file.
        if filename in previous and previous[filename][0]==raw:
            os.utime(path,ns=previous[filename][1:])
    report['generated'] = True
    report['generated_function_count'] = len(selected)
    write_json(output / 'coverage.json', report)
    log_emission('Done: %d functions registered; wrote %s and %s',
                 len(selected), output / 'functions.c', output / 'generated.h')
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('--metadata', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--rom', type=Path)
    parser.add_argument('--function', action='append', dest='functions')
    parser.add_argument('--report-only', action='store_true')
    args = parser.parse_args()
    profile = load_profile(args.profile)
    game = profile['game']
    rom, _ = load_rom(args.rom or game['rom'], game['rom_sha1'], game['rom_size'])
    report = translate(rom, load_metadata(args.metadata, rom), profile, args.output, args.functions, args.report_only)
    print(f"Coverage: {report['summary']}; {report['instruction_count']} instructions")
    if report['generated']:
        print(f"Generated {report['generated_function_count']} C functions in {args.output}")
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RecompError, OSError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
