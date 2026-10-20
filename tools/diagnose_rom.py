#!/usr/bin/env python3
"""Aggregate verified ROM, coverage, crash, callback and RSP evidence for a model.

Does not run the game, regenerate CPU sources, or change profiles/inventories.
Always supply metadata from the executable that produced the failure.
"""
from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys

from common import RecompError, load_profile
from export_analysis_rom import load_analysis_rom
from generate_game import GAMES, PROJECT
from inspect_address import inspect_address, read_inputs
from inspect_callbacks import inspect_callbacks, mapped_range, print_report


def integer(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def address_evidence(metadata, coverage, address, loaded=None, section=None):
    regions = [s for s in metadata['sections'] if s['vram'] <= address < s['vram'] + s['size']
               and (section is None or section in (str(s['id']), s['name']))]
    result = dict(address=address, mappings=[])
    for region in regions:
        sid = region['id']
        entries = [f for f in metadata['functions'] if f['section'] == sid and f['vram'] == address]
        owners = [f for f in metadata['functions'] if f['section'] == sid
                  and address in f['instruction_addresses']]
        state = loaded[sid] if loaded is not None and sid < len(loaded) else None
        result['mappings'].append(dict(section=sid, name=region['name'], loaded=state,
            rom_offset=region['rom_offset'] + address - region['vram'],
            entries=[f['name'] for f in entries], owners=[f['name'] for f in owners],
            translation=[f for f in (coverage or {}).get('functions', [])
                         if f['section'] == sid and any(e['vram'] == f['vram'] for e in entries)]))
    return result


def next_checks(failure, evidence, unsupported):
    checks = []
    if failure and 'unregistered or unloaded' in failure['message']:
        mappings = evidence[0]['mappings']
        if not mappings:
            checks.append('Trace the loader, pager and relocations; the failing target has no selected mapping.')
        elif any(m['entries'] and m['loaded'] is False for m in mappings):
            checks.append('The target is registered but its section was unloaded: inspect DMA destinations and code visibility ranges before adding a root.')
        elif not any(m['entries'] for m in mappings):
            checks.append('Trace the indirect caller and pointer producer in callbacks.txt; verify the complete callback/table ABI before adding checked roots.')
        else:
            checks.append('The target exists in supplied metadata: check executable freshness, runtime section state and relocation/alias selection.')
    elif failure and failure.get('ram_range'):
        checks.append('Treat the failure address as a memory operand. Trace its writer/owner; do not assume it is an instruction PC.')
    elif failure and ('AI buffer' in failure['message'] or 'RSP' in failure['message']):
        checks.append('Inspect task.txt and the current thread arguments; verify task microcode/descriptor or DMA size against its producer.')
    if unsupported:
        checks.append('Review unsupported_functions in summary.json; inspect each transfer, delay slot and table bound before changing metadata.')
    checks.append('Fix the producing profile/adapter/runtime input, rebuild, then replay the same trigger beyond the recorded failure.')
    return checks


def diagnose(args):
    # Refuse an existing directory so earlier crash evidence is never overwritten.
    if args.output.exists():
        raise RecompError('output already exists; choose a new diagnostic directory')
    rom, metadata = load_analysis_rom(args.game, args.metadata)
    _, _, coverage = read_inputs(args.metadata, None, args.coverage)
    profile = load_profile(PROJECT / 'games' / args.game / 'profile.toml')
    crash = json.loads(args.crash.read_text()) if args.crash else None
    warnings = []
    loaded = None
    resolve_native = True
    if crash:
        if crash.get('format_version') != 1 or crash.get('rom_sha1') != profile['game']['rom_sha1']:
            raise RecompError('crash format or original cartridge SHA-1 differs from the selected game')
        if crash.get('game', {}).get('name') != args.game:
            raise RecompError('crash belongs to a different game')
        newer = [str(p) for p in (args.metadata, args.coverage or args.metadata.with_name('coverage.json'),
                 Path(crash['executable'])) if p.is_file() and p.stat().st_mtime_ns > args.crash.stat().st_mtime_ns]
        if newer:
            resolve_native = False
            warnings.append('Artifacts newer than the crash: ' + ', '.join(newer)
                            + '. Loaded-section labels and native source resolution are disabled; preserve the matching original build.')
        else:
            loaded = crash.get('loaded_sections')
        warnings.append('A matching ROM hash alone does not prove matching generated sources or executable bytes.')
    requested = [(a, 'explicit') for a in args.address]
    failure = crash.get('failure') if crash else None
    if failure:
        requested.insert(0, (integer(failure['address']), 'failure operand (may be data or hardware)'))
        for thread in crash.get('threads', []):
            if thread.get('current'):
                requested.append((integer(thread['last_tracked_pc']), 'current guest PC'))
        for a in reversed(crash.get('recent_calls', [])[-3:]):
            requested.append((integer(a), 'recent call (context, not a proven caller)'))
    if not requested and coverage:
        for function in coverage['functions']:
            for error in function.get('errors', []):
                requested.append((error['pc'], 'translation issue'))
    selected = []
    for address, origin in requested:
        if not 0 <= address <= 0xffffffff:
            raise RecompError('addresses must be unsigned 32-bit guest addresses')
        if address not in [a for a, _ in selected]:
            selected.append((address, origin))
        if len(selected) >= args.limit:
            break
    unsupported = [f for f in (coverage or {}).get('functions', []) if f.get('errors')]
    summary = dict(format_version=1, game=args.game, metadata=str(args.metadata.resolve()),
        metadata_sha256=hashlib.sha256(args.metadata.read_bytes()).hexdigest(),
        profile_sha256=hashlib.sha256((PROJECT / 'games' / args.game / 'profile.toml').read_bytes()).hexdigest(),
        cartridge_sha1=profile['game']['rom_sha1'], analysis_sha1=metadata['rom_sha1'],
        sections=metadata['sections'], discovery_summary=metadata.get('discovery_summary'),
        coverage_summary=(coverage or {}).get('summary'), metadata_issues=(coverage or {}).get('metadata_issues', metadata.get('issues')),
        unsupported_functions=unsupported, warnings=warnings, addresses=[], subtools=[])
    if crash:
        summary.update(crash=str(args.crash.resolve()), failure=failure,
            threads=crash.get('threads'), recent_calls=crash.get('recent_calls'),
            recorded_loaded_sections=crash.get('loaded_sections'))
    args.output.mkdir(parents=True)
    text_ranges, callback_targets = [], set()
    for address, origin in selected:
        item = address_evidence(metadata, coverage, address, loaded, args.section)
        item['origin'] = origin
        summary['addresses'].append(item)
        stream = io.StringIO()
        try:
            with redirect_stdout(stream):
                inspect_address(metadata, address, args.section, rom, coverage, before=12, words=20)
            if address % 4 == 0 and len(item['mappings']) == 1:
                region = next(s for s in metadata['sections'] if s['id'] == item['mappings'][0]['section'])
                if address + 16 <= region['vram'] + region['size']:
                    callback_targets.add(address)
                    if not any(r['section'] == region['id'] for r in text_ranges):
                        text_ranges.append(dict(section=region['id'], vram=region['vram'],
                            rom_offset=region['rom_offset'], size=region['size']))
        except RecompError as error:
            stream.write(f'{error}\n')
        (args.output / f'address-{address:08x}.txt').write_text(stream.getvalue())
    if callback_targets:
        data = [mapped_range(metadata, value) for value in args.data]
        callbacks = inspect_callbacks(rom, metadata, text_ranges, data, callback_targets, args.section, True)
        stream = io.StringIO()
        with redirect_stdout(stream):
            print_report(callbacks, rom, metadata, 6, 6)
        (args.output / 'callbacks.txt').write_text(stream.getvalue())
        (args.output / 'callbacks.json').write_text(json.dumps(callbacks, indent=2) + '\n')
    if args.crash:
        commands = [('crash.txt', 'inspect_crash.py', [str(args.crash)] + ([] if resolve_native else ['--no-resolve']))]
        task = crash.get('task')
        if task or len(crash.get('task_words', [])) == 16:
            commands.append(('task.txt', 'inspect_rsp_task.py', [str(args.crash), '--rom',
                str(profile['game']['rom']), '--commands', '24', '--list-scale', str(args.list_scale)]))
        for filename, script, extra in commands:
            try:
                result = subprocess.run([sys.executable, str(PROJECT / 'tools' / script), *extra],
                    capture_output=True, text=True, timeout=45, check=False)
                (args.output / filename).write_text(result.stdout + result.stderr)
                summary['subtools'].append(dict(tool=script, exit_code=result.returncode, output=filename))
            except subprocess.TimeoutExpired:
                summary['subtools'].append(dict(tool=script, error='timed out after 45 seconds'))
    summary['next_checks'] = next_checks(failure, summary['addresses'], unsupported)
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    lines = [f"{args.game}: {summary['coverage_summary']}",
        f"Cartridge SHA-1 {summary['cartridge_sha1']}; analysis SHA-1 {summary['analysis_sha1']}"]
    if failure:
        lines.append(f"Failure: {failure['message']} at {failure['address']}")
    lines.extend('Warning: ' + w for w in warnings)
    lines.extend(f"0x{a['address']:08x} ({a['origin']}): {json.dumps(a['mappings'])}" for a in summary['addresses'])
    lines.extend('Next: ' + c for c in summary['next_checks'])
    (args.output / 'report.txt').write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines[:3 if failure else 2]))
    print(f"Evidence: {args.output / 'report.txt'}; structured report: {args.output / 'summary.json'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, choices=GAMES)
    parser.add_argument('--metadata', required=True, type=Path)
    parser.add_argument('--coverage', type=Path)
    parser.add_argument('--crash', type=Path)
    parser.add_argument('--address', action='append', type=lambda a: int(a, 0), default=[])
    parser.add_argument('--section', help='section ID or name for ambiguous overlay addresses')
    parser.add_argument('--data', action='append', default=[], help='bounded pointer data SECTION:START:END; end exclusive')
    parser.add_argument('--limit', type=int, default=6, help='maximum inspected addresses, 1..32')
    parser.add_argument('--list-scale', type=int, choices=(1, 8), default=1)
    parser.add_argument('--output', required=True, type=Path, help='new private diagnostic directory, normally under build/')
    args = parser.parse_args()
    if not 1 <= args.limit <= 32:
        parser.error('--limit must be 1..32')
    diagnose(args)


if __name__ == '__main__':
    try:
        main()
    except (RecompError, OSError, ValueError, KeyError, TypeError, ImportError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
