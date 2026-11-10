#!/usr/bin/env python3
"""Rank missing callback tables and function roots, then optionally review them.

The default mode only writes a report. --codex is an explicit, bounded opt-in:
Codex reads the repository in a read-only sandbox and returns structured table
proposals. This script checks the pinned ROM and normal generator before keeping
each change to profile.toml. It never starts or resumes a game.
"""
from __future__ import annotations

import argparse
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from common import RecompError, load_profile, write_json
from export_analysis_rom import load_analysis_rom
from generate_game import GAMES, PROJECT, generate
from probe_function_roots import call_edges, descendants, probe, ranked_roots
from promote_live_patches import add_roots, write_atomic


SCHEMA = {
    'type': 'object', 'additionalProperties': False,
    'required': ['status', 'reason', 'tables', 'roots'],
    'properties': {
        'status': {'type': 'string', 'enum': ['verified', 'unresolved']},
        'reason': {'type': 'string'},
        'tables': {'type': 'array', 'items': {
            'type': 'object', 'additionalProperties': False,
            'required': ['pc', 'expected_word', 'table_vram', 'count', 'indices'],
            'properties': {
                'pc': {'type': 'integer'},
                'expected_word': {'type': 'integer'},
                'table_vram': {'type': 'integer'},
                'count': {'type': 'integer'},
                'indices': {'type': 'array', 'items': {'type': 'integer'}},
            },
        }},
        'roots': {'type': 'array', 'items': {'type': 'integer'}},
    },
}

ROOT_SCHEMA = {
    'type': 'object', 'additionalProperties': False,
    'required': ['status', 'reason', 'address'],
    'properties': {
        'status': {'type': 'string', 'enum': ['verified', 'unresolved']},
        'reason': {'type': 'string'},
        'address': {'type': 'integer'},
    },
}


def root_groups(game: str, report: dict, original_sha1: str) -> list[dict]:
    """Keep disconnected call trees visible, especially observed runtime roots."""
    candidates = report['candidates']
    edges = call_edges(candidates)
    ranked = ranked_roots(candidates, edges)
    by_key = {(item['section'], item['vram']): item for item in candidates}
    by_address: dict[int, list[dict]] = {}
    for candidate in candidates:
        by_address.setdefault(candidate['vram'], []).append(candidate)
    requests = PROJECT / 'local' / 'live-patches' / game
    observed = set()
    if requests.is_dir():
        for path in requests.glob('request-*.json'):
            try:
                request = json.loads(path.read_text())
            except (OSError, ValueError):
                continue
            if request.get('rom_sha1') == original_sha1 and request.get('kind') == 'missing_function':
                observed.add(int(request['address'], 0))
    selected = {(item['section'], item['vram']): item for item in ranked}
    for address in observed:
        matches = by_address.get(address, [])
        if len(matches) == 1:
            candidate = matches[0]
            if not candidate.get('registered') and candidate['body'].get('stack_prologue'):
                selected.setdefault((candidate['section'], address),
                                    {'vram': address, 'section': candidate['section'],
                                     'candidate_descendants': len(descendants(address, edges))})
    groups = []
    for item in selected.values():
        candidate = by_key[item['section'], item['vram']]
        groups.append({'vram': item['vram'], 'section': item['section'],
                       'confidence': candidate['confidence'],
                       'candidate_descendants': item['candidate_descendants'],
                       'runtime_observed': item['vram'] in observed,
                       'references': sorted({ref['kind'] for ref in candidate['references']}),
                       'body_issues': candidate['body'].get('issues', [])[:2]})
    groups.sort(key=lambda item: (not item['runtime_observed'],
                                  -item['candidate_descendants'], item['section'], item['vram']))
    return groups


def table_groups(report: dict, checked_sites: set[int]) -> list[dict]:
    """Rank shared producer tables, counting each target only once per table."""
    groups: dict[tuple[int, int], dict] = {}
    for table in report.get('indirect_tables', []):
        if table['pc'] in checked_sites:
            continue
        key = table['table_section'], table['table_vram']
        group = groups.setdefault(key, {'table_section': key[0], 'table_vram': key[1],
                                        'sites': [], 'candidates': set(), 'likely': set()})
        group['sites'].append({name: table[name] for name in
                               ('pc', 'section', 'function', 'scanned_words', 'stop_reason',
                                'bounds_verified', 'target_count') if name in table})
    for candidate in report['candidates']:
        if candidate.get('registered'):
            continue
        for ref in candidate.get('references', []):
            key = ref.get('table_section'), ref.get('table_vram')
            group = groups.get(key)
            if group and ref.get('pc') in {site['pc'] for site in group['sites']}:
                group['candidates'].add(candidate['vram'])
                if candidate['confidence'] == 'likely':
                    group['likely'].add(candidate['vram'])
    ranked = []
    for group in groups.values():
        if not group['candidates']:
            continue
        group['sites'].sort(key=lambda site: site['pc'])
        group['candidates'] = sorted(group['candidates'])
        group['likely'] = sorted(group['likely'])
        group['likely_count'] = len(group['likely'])
        group['candidate_count'] = len(group['candidates'])
        ranked.append(group)
    ranked.sort(key=lambda group: (-group['likely_count'], -group['candidate_count'],
                                   group['table_section'], group['table_vram']))
    return ranked


def read_inputs(game: str, metadata_path: Path) -> tuple[bytes, dict, dict]:
    rom, metadata = load_analysis_rom(game, metadata_path)
    report = json.loads(metadata_path.with_name('function-candidates.json').read_text())
    if report['rom_sha1'] != metadata['rom_sha1']:
        raise RecompError('candidate report and generated metadata use different ROMs')
    return rom, metadata, report


def checked_sites(game: str) -> set[int]:
    profile = load_profile(PROJECT / 'games' / game / 'profile.toml')
    discovery = profile['discovery']
    return {table['pc'] for key in ('function_tables', 'jump_tables')
            for table in discovery.get(key, [])}


def rank_report(game: str, metadata_path: Path, output: Path) -> tuple[bytes, dict, dict, list[dict], list[dict]]:
    rom, metadata, candidates = read_inputs(game, metadata_path)
    groups = table_groups(candidates, checked_sites(game))
    profile = load_profile(PROJECT / 'games' / game / 'profile.toml')
    roots = root_groups(game, candidates, profile['game']['rom_sha1'])
    write_json(output, {'game': game, 'rom_sha1': metadata['rom_sha1'],
                        'summary': candidates['summary'], 'table_groups': groups,
                        'root_groups': roots,
                        'note': 'Scanned tables and candidate call trees need checked entry evidence.'})
    print(f'{game}: {candidates["summary"]}; {len(groups)} table groups, '
          f'{len(roots)} independent root probes')
    for group in groups[:10]:
        sites = ', '.join(f'0x{s["pc"]:08x}' for s in group['sites'][:4])
        print(f'  0x{group["table_vram"]:08x}: {group["likely_count"]} likely, '
              f'{group["candidate_count"]} candidates; sites {sites}')
    for root in roots[:10]:
        print(f'  root 0x{root["vram"]:08x}: {root["candidate_descendants"]} candidate '
              f'functions; {root["confidence"]}; '
              f'{"runtime observed" if root["runtime_observed"] else ", ".join(root["references"])}')
    print(f'Report: {output}')
    return rom, metadata, candidates, groups, roots


def ask_codex(game: str, group: dict, metadata_path: Path, output: Path) -> dict:
    codex = shutil.which('codex')
    if not codex:
        raise RecompError('Codex CLI is unavailable')
    output.mkdir(parents=True, exist_ok=True)
    tag = f'{group["table_section"]}-{group["table_vram"]:08x}'
    schema = output / 'proposal-schema.json'
    schema.write_text(json.dumps(SCHEMA, indent=2) + '\n')
    answer, transcript = output / f'{tag}.json', output / f'{tag}.log'
    prompt = (
        f'Follow AGENTS.md in {PROJECT}. Review one unregistered function-table group '
        f'for {game}, using the pinned original ROM and prepared metadata at {metadata_path}. '
        f'Table address 0x{group["table_vram"]:08x}, analysis section {group["table_section"]}; '
        f'call sites {", ".join(f"0x{s["pc"]:08x}" for s in group["sites"])}. '
        f'The diagnostic scan found {group["candidate_count"]} targets, including '
        f'{group["likely_count"]} likely. Candidate addresses: '
        f'{", ".join(f"0x{address:08x}" for address in group["candidates"][:256])}. '
        f'Its scanned length is NOT a proven table bound. '
        f'Candidate report: {metadata_path.with_name("function-candidates.json")}. '
        f'Inspect the producer code and exact index constraints, adjacent data, target ownership, '
        f'and ROM bytes. A checked function_tables declaration requires exact count/indices '
        f'and every proposed call site. If selector bounds are unproved, you may instead '
        f'propose individual function roots from this table: include only exact ROM pointer '
        f'slots with separate closed entry bodies, unambiguous ownership and a credible '
        f'function-pointer-table consumer. Roots assert entries, not selector bounds. '
        f'Return verified only for justified tables or roots; omit uncertain targets. '
        f'Use decimal JSON integers. An empty indices list means all slots from zero '
        f'through count-1. If nothing is verified, return unresolved with empty arrays. '
        f'Do not edit files, run the game, or invoke another agent.'
    )
    with transcript.open('w') as log:
        process = subprocess.run([codex, 'exec', '--sandbox', 'read-only', '--ephemeral',
                                  '-C', str(PROJECT), '--output-schema', str(schema),
                                  '--output-last-message', str(answer), '-'],
                                 input=prompt, text=True, stdout=log,
                                 stderr=subprocess.STDOUT, check=False)
    if process.returncode:
        raise RecompError(f'Codex review failed for {tag}; see {transcript}')
    return json.loads(answer.read_text())


def ask_codex_root(game: str, group: dict, metadata_path: Path, output: Path,
                   evidence: dict) -> dict:
    codex = shutil.which('codex')
    if not codex:
        raise RecompError('Codex CLI is unavailable')
    tag = f'root-{group["section"]}-{group["vram"]:08x}'
    schema = output / 'root-proposal-schema.json'
    schema.write_text(json.dumps(ROOT_SCHEMA, indent=2) + '\n')
    answer, transcript = output / f'{tag}.json', output / f'{tag}.log'
    prompt = (
        f'Follow AGENTS.md in {PROJECT}. Review one missing function entry for {game} '
        f'at 0x{group["vram"]:08x}, section {group["section"]}, using the pinned ROM and '
        f'metadata at {metadata_path}. Candidate report: '
        f'{metadata_path.with_name("function-candidates.json")}. '
        f'Candidate evidence: {json.dumps(group)}. Independent dry-run discovery: '
        f'{json.dumps(evidence)}. Inspect the exact ROM pointer context, entry/body '
        f'boundaries, control flow, and whether its descendants are separate functions. '
        f'The dry run proves only that discovery accepts bytes, not that the root is '
        f'a real runtime entry. Return verified only when the entry evidence is '
        f'sufficient to make it a permanent checked root. Set address to the exact '
        f'decimal entry address, or zero if unresolved. Do not edit files, run the game, '
        f'or invoke another agent.'
    )
    with transcript.open('w') as log:
        process = subprocess.run([codex, 'exec', '--sandbox', 'read-only', '--ephemeral',
                                  '-C', str(PROJECT), '--output-schema', str(schema),
                                  '--output-last-message', str(answer), '-'],
                                 input=prompt, text=True, stdout=log,
                                 stderr=subprocess.STDOUT, check=False)
    if process.returncode:
        raise RecompError(f'Codex root review failed for {tag}; see {transcript}')
    return json.loads(answer.read_text())


def checked_table_lines(rom: bytes, metadata: dict, report: dict, group: dict, proposal: dict,
                        existing_sites: set[int]) -> list[str]:
    if proposal.get('status') == 'unresolved':
        if proposal.get('tables'):
            raise RecompError('unresolved review must not propose tables')
        return []
    if proposal.get('status') != 'verified' or not proposal.get('reason', '').strip():
        raise RecompError('Codex response has no verified proposal')
    sites = {site['pc']: site for site in group['sites']}
    table_section = metadata['sections'][group['table_section']]
    known_targets = ({candidate['vram'] for candidate in report['candidates']}
                     | {function['vram'] for function in metadata['functions']})
    lines = []
    seen = set()
    for table in proposal['tables']:
        pc, word, address, count = (table[key] for key in
                                    ('pc', 'expected_word', 'table_vram', 'count'))
        indices = table['indices']
        if (type(pc) is not int or type(word) is not int or type(address) is not int
                or type(count) is not int or pc not in sites or pc in existing_sites or pc in seen
                or address != group['table_vram'] or not 0 < count <= 8192
                or count > sites[pc]['scanned_words'] or address % 4
                or not isinstance(indices, list)
                or any(type(i) is not int or not 0 <= i < count for i in indices)
                or len(indices) != len(set(indices))):
            raise RecompError('proposed table site, count, or indices are outside reviewed evidence')
        seen.add(pc)
        source = [f for f in metadata['functions'] if f['section'] == sites[pc]['section']
                  and pc in f['instruction_addresses']]
        if len(source) != 1:
            raise RecompError(f'proposed call site 0x{pc:08x} has no unique owner')
        offset = source[0]['rom_offset'] + pc - source[0]['vram']
        if (int.from_bytes(rom[offset:offset + 4], 'big') != word
                or word >> 26 != 0 or word & 63 != 9):
            raise RecompError(f'proposed call site instruction changed at 0x{pc:08x}')
        if (address < table_section['vram'] or
                address + count * 4 > table_section['vram'] + table_section['size']):
            raise RecompError('proposed table exceeds its verified image section')
        offset = table_section['rom_offset'] + address - table_section['vram']
        raw = rom[offset:offset + count * 4]
        selected = indices or list(range(count))
        targets = [int.from_bytes(raw[i * 4:i * 4 + 4], 'big') for i in selected]
        if any(target == 0 or target % 4 or target not in known_targets or not any(
                section['vram'] <= target < section['vram'] + section['size']
                for section in metadata['sections']) for target in targets):
            raise RecompError('proposed table includes null, unaligned, or unmapped target')
        digest = hashlib.sha256(raw).hexdigest()
        extra = ', indices = [' + ', '.join(f'0x{i:x}' for i in indices) + ']' if indices else ''
        lines.append(f'  {{pc = 0x{pc:08x}, expected_word = 0x{word:08x}, '
                     f'table_vram = 0x{address:08x}, count = 0x{count:x}{extra}, '
                     f'sha256 = "{digest}"}},')
    return lines


def checked_table_roots(rom: bytes, metadata: dict, report: dict,
                        group: dict, proposal: dict) -> list[dict]:
    """Verify exact table slots before using them as individual code roots."""
    roots = proposal.get('roots')
    if (not isinstance(roots, list) or len(roots) > 256
            or any(type(address) is not int for address in roots)
            or len(roots) != len(set(roots))):
        raise RecompError('Codex root list is invalid')
    if proposal.get('status') == 'unresolved':
        if roots:
            raise RecompError('unresolved review must not propose roots')
        return []
    sites = {site['pc'] for site in group['sites']}
    candidates = {}
    for candidate in report['candidates']:
        candidates.setdefault(candidate['vram'], []).append(candidate)
    additions = []
    for address in roots:
        matches = candidates.get(address, []) if type(address) is int else []
        if len(matches) != 1:
            raise RecompError('proposed root has no unique candidate mapping')
        candidate = matches[0]
        section = metadata['sections'][candidate['section']]
        offset = section['rom_offset'] + address - section['vram']
        sources = [ref for ref in candidate['references']
                   if ref['kind'] == 'indexed-table-call'
                   and ref.get('table_vram') == group['table_vram']
                   and ref.get('table_section') == group['table_section']
                   and ref.get('pc') in sites]
        if (candidate['registered'] or candidate['confidence'] != 'likely'
                or candidate['body']['status'] != 'closed'
                or candidate['body']['issues'] or not sources
                or candidate['rom_offset'] != offset
                or hashlib.sha256(rom[offset:offset + 16]).hexdigest() != candidate['sha256']
                or any(int.from_bytes(rom[ref['rom_offset']:ref['rom_offset'] + 4], 'big')
                       != address for ref in sources)):
            raise RecompError(f'proposed table root 0x{address:08x} lacks checked entry evidence')
        additions.append({'address': address, 'section_name': section['name']})
    if additions:
        first = min(additions, key=lambda item: item['address'])
        first['evidence'] = (f'Function pointer slots at 0x{group["table_vram"]:08x}; '
                             f'Codex reviewed table provenance; the ROM and generator '
                             f'checked each closed entry. '
                             f'This root set does not assert a selector bound. '
                             + ' '.join(proposal['reason'].split()))
    return additions


def with_tables(original: bytes, lines: list[str]) -> bytes:
    if not lines:
        return original
    text = original.decode()
    matches = list(re.finditer(r'(?m)^function_tables = \[\n', text))
    if len(matches) != 1:
        raise RecompError('profile must have exactly one discovery function_tables list')
    start = matches[0].end()
    end = text.find('\n]', start)
    if end < 0:
        raise RecompError('cannot locate function_tables list end')
    return (text[:end] + '\n  # Reviewed before gameplay from bounded ROM table evidence.\n'
            + '\n'.join(lines) + text[end:]).encode()


def restore(path: Path, original: bytes) -> None:
    temporary = path.with_name(f'.{path.name}.review-{os.getpid()}')
    try:
        temporary.write_bytes(original)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def run(game: str, build_dir: Path, codex: bool, max_groups: int,
        retry_unresolved: bool = False, roots_only: bool = False,
        root_addresses: list[int] | None = None) -> None:
    output = build_dir / 'candidate-review' / game
    output.mkdir(parents=True, exist_ok=True)
    result_path = output / 'results.json'
    previous = json.loads(result_path.read_text()) if result_path.is_file() else {}
    previous_sha = previous.get('rom_sha1')
    if previous and previous_sha is None:
        old_baseline = output / 'baseline' / 'symbols.json'
        if old_baseline.is_file():
            previous_sha = json.loads(old_baseline.read_text()).get('rom_sha1')
    metadata_path = build_dir / 'generated' / game / 'symbols.json'
    if codex:
        # Refresh in a separate directory so an active executable/build is untouched.
        baseline = output / 'baseline'
        with (output / 'baseline.log').open('w') as log, redirect_stdout(log), redirect_stderr(log):
            generate(game, baseline, False)
        metadata_path = baseline / 'symbols.json'
    if not metadata_path.is_file():
        raise RecompError(f'generate the game first: {metadata_path}')
    rom, metadata, report, groups, roots = rank_report(game, metadata_path, output / 'ranked.json')
    if not codex:
        return
    profile_path = PROJECT / 'games' / game / 'profile.toml'
    attempts = (previous.get('attempts', []).copy()
                if previous.get('game') == game and previous_sha == metadata['rom_sha1'] else [])
    unresolved = {item['group'] for item in attempts if item['status'] == 'unresolved'}
    if roots_only:
        wanted = set(root_addresses or [])
        if wanted - {root['vram'] for root in roots}:
            raise RecompError('requested root is absent from the ranked candidate report')
        pending_roots = [root for root in roots
                         if (not wanted or root['vram'] in wanted)
                         and (retry_unresolved or
                              f'root-{root["section"]}-{root["vram"]:08x}' not in unresolved)]
        for root in pending_roots[:max_groups]:
            address = root['vram']
            tag = f'root-{root["section"]}-{address:08x}'
            attempts = [item for item in attempts if item['group'] != tag]
            item = {'group': tag, 'candidate_descendants': root['candidate_descendants']}
            try:
                evidence = probe(game, metadata_path, [address], 1)['probes'][0]
                if evidence['status'] != 'discovered' or evidence['new_functions'] < 1:
                    item.update(status='unresolved', reason=evidence.get('reason', 'root added no code'))
                else:
                    proposal = ask_codex_root(game, root, metadata_path, output, evidence)
                    item['reason'] = proposal.get('reason', '')
                    if proposal.get('status') == 'unresolved':
                        item['status'] = 'unresolved'
                    elif (proposal.get('status') != 'verified'
                          or type(proposal.get('address')) is not int
                          or proposal['address'] != address or not item['reason'].strip()):
                        raise RecompError('Codex response has no verified matching root')
                    else:
                        candidate = next((candidate for candidate in report['candidates']
                                          if candidate['vram'] == address
                                          and candidate['section'] == root['section']), None)
                        section = metadata['sections'][root['section']]
                        offset = section['rom_offset'] + address - section['vram']
                        if (candidate is None or candidate['rom_offset'] != offset
                                or hashlib.sha256(rom[offset:offset + 16]).hexdigest()
                                != candidate['sha256']):
                            raise RecompError('root ROM mapping or entry fingerprint changed')
                        addition = {'address': address, 'section_name': section['name'],
                                    'evidence': item['reason']}
                        original = profile_path.read_bytes()
                        replacement = add_roots(original, [addition], 'reviewed')
                        write_atomic(profile_path, replacement)
                        try:
                            validation = output / f'validate-{tag}'
                            with (output / f'validate-{tag}.log').open('w') as log, \
                                    redirect_stdout(log), redirect_stderr(log):
                                generate(game, validation, False)
                            updated = json.loads((validation / 'symbols.json').read_text())
                            gained = {(f['section'], f['vram']) for f in updated['functions']}
                            if (root['section'], address) not in gained:
                                raise RecompError('validated generation omitted proposed root')
                            item.update(status='accepted', new_functions=evidence['new_functions'])
                            rom, metadata, report = read_inputs(game, validation / 'symbols.json')
                            metadata_path = validation / 'symbols.json'
                        except Exception:
                            restore(profile_path, original)
                            raise
            except (RecompError, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
                item.update(status='rejected', reason=str(error))
            attempts.append(item)
            write_json(result_path, {'game': game, 'rom_sha1': metadata['rom_sha1'],
                                     'attempts': attempts})
            print(f'{tag}: {item["status"]}: {item.get("reason", "")}')
        print(f'Review results: {result_path}')
        return
    pending = [group for group in groups
               if retry_unresolved or
               f'{group["table_section"]}-{group["table_vram"]:08x}' not in unresolved]
    if len(pending) < len(groups):
        print(f'Skipping {len(groups) - len(pending)} previously unresolved groups; '
              'use --retry-unresolved to revisit them')
    for group in pending[:max_groups]:
        tag = f'{group["table_section"]}-{group["table_vram"]:08x}'
        attempts = [item for item in attempts if item['group'] != tag]
        item = {'group': tag, 'likely_count': group['likely_count']}
        try:
            proposal = ask_codex(game, group, metadata_path, output)
            item['reason'] = proposal.get('reason', '')
            lines = checked_table_lines(rom, metadata, report, group, proposal, checked_sites(game))
            roots = checked_table_roots(rom, metadata, report, group, proposal)
            if not lines and not roots:
                item['status'] = 'unresolved'
                attempts.append(item)
                write_json(result_path, {'game': game, 'rom_sha1': metadata['rom_sha1'],
                                         'attempts': attempts})
                print(f'{tag}: unresolved: {item.get("reason", "")}')
                continue
            original = profile_path.read_bytes()
            replacement = add_roots(with_tables(original, lines), roots, 'table')
            write_atomic(profile_path, replacement)
            try:
                validation = output / f'validate-{tag}'
                with (output / f'validate-{tag}.log').open('w') as log, \
                        redirect_stdout(log), redirect_stderr(log):
                    generate(game, validation, False)
                updated = json.loads((validation / 'symbols.json').read_text())
                addresses = set(group['candidates'])
                prior_functions = {f['vram'] for f in metadata['functions']}
                new_functions = {f['vram'] for f in updated['functions']}
                gained = sorted(addresses & (new_functions - prior_functions))
                coverage = json.loads((validation / 'coverage.json').read_text())
                ready = {(f['section'], f['vram']) for f in coverage['functions']
                         if f['status'] == 'ready'}
                section_ids = {section['name']: section['id'] for section in metadata['sections']}
                if not gained:
                    raise RecompError('proposal registered no new functions from this group')
                if any((section_ids[root['section_name']], root['address']) not in ready
                       for root in roots):
                    raise RecompError('proposed table root did not translate as ready')
                item['status'] = 'accepted'
                item['new_functions'] = [f'0x{address:08x}' for address in gained]
                item['checked_roots'] = [f'0x{root["address"]:08x}' for root in roots]
                rom, metadata, report = read_inputs(game, validation / 'symbols.json')
                metadata_path = validation / 'symbols.json'
            except Exception:
                restore(profile_path, original)
                raise
        except (RecompError, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
            item['status'] = 'rejected'
            item['reason'] = str(error)
        attempts.append(item)
        write_json(result_path, {'game': game, 'rom_sha1': metadata['rom_sha1'],
                                 'attempts': attempts})
        print(f'{tag}: {item["status"]}: {item.get("reason", "")}')
    print(f'Review results: {result_path}')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', choices=GAMES, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--codex', action='store_true', help='ask Codex to review and apply bounded table groups')
    parser.add_argument('--roots-only', action='store_true',
                        help='review missing function call trees and save accepted roots in profile.toml')
    parser.add_argument('--root-address', action='append', type=lambda value: int(value, 0),
                        default=[], help='limit root review to this candidate address; repeatable')
    parser.add_argument('--max-groups', type=int, default=3)
    parser.add_argument('--retry-unresolved', action='store_true',
                        help='revisit table groups that a previous Codex review left unresolved')
    args = parser.parse_args()
    if not 1 <= args.max_groups <= 10:
        parser.error('--max-groups must be between 1 and 10')
    if args.root_address and not args.roots_only:
        parser.error('--root-address requires --roots-only')
    try:
        run(args.game, args.build_dir.resolve(), args.codex, args.max_groups,
            args.retry_unresolved, args.roots_only, args.root_address)
    except (RecompError, OSError, ValueError, KeyError, ImportError) as error:
        print(f'candidate review stopped: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
