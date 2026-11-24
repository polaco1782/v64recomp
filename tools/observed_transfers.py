"""Validate successfully interpreted CPU paths before adding them to a game's profile."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory

from common import RecompError
from generate_game import generate, load_generator


def checked_edge(record, rom, metadata, coverage, parents):
    if record['rom'] != coverage['cartridge_sha1'] or record['layout'] != coverage['section_layout_sha256']:
        raise RecompError('different cartridge or section layout')
    sections = metadata['sections']
    pc, target, size = (record[key] for key in ('pc', 'target', 'size'))
    sid, tid = record['section'], record['target_section']
    if (any(type(n) is not int or n < 0 for n in (pc, target, size, sid, tid))
            or sid >= len(sections) or tid >= len(sections) or size <= 0
            or (pc | target | size) & 3 or type(record['jump']) is not bool):
        raise RecompError('invalid observed instruction range')

    def raw(section, address, length):
        if not section['vram'] <= address <= section['vram'] + section['size'] - length:
            raise RecompError('observation outside its section')
        offset = section['rom_offset'] + address - section['vram']
        return rom[offset:offset + length]

    word = int.from_bytes(raw(sections[sid], pc, 4), 'big')
    if word != record['word'] or hashlib.sha256(raw(sections[tid], target, size)).hexdigest() != record['sha256']:
        raise RecompError('observed bytes differ from the prepared ROM')
    owners = [f for f in metadata['functions'] if f['section'] == sid and pc in f['instruction_addresses']]
    owner = owners[0] if len(owners) == 1 else None
    if not owners:
        # A nested call may be discovered during the same run. Its parent
        # must itself lead back to a generated caller. The isolated generation
        # below verifies instruction ownership and complete bodies for both.
        if record['jump'] or not any(e['target_section'] == sections[sid]['name'] and
                e['target'] <= pc < e['target'] + e['size'] for e in parents):
            raise RecompError('source has no unique translated owner')
    elif not owner or not any(f['section'] == sid and f['vram'] == owner['vram'] and f['selected']
                              and f['status'] == 'ready' for f in coverage['functions']):
        raise RecompError('source is not a translated guest function')
    transfer = next((t for t in owner['indirect_transfers'] if t['pc'] == pc), {}) if owner else {}
    if record['jump']:
        if word & 0xfc1fffff != 8 or word >> 21 & 31 == 31:
            raise RecompError('source is not a computed jump')
        kind = transfer.get('kind')
        if not kind and sid == tid and target in owner['instruction_addresses']:
            kind = 'jump'
        if kind not in ('jump', 'tail') or kind == 'jump' and (sid != tid or target < owner['vram']):
            raise RecompError('cannot establish local jump or tail-call ownership')
        if transfer.get('origin') == 'bounded-switch' or 'table_vram' in transfer:
            raise RecompError('observed jump needs a review of its checked table')
    else:
        kind = 'call'
        if word >> 26 == 3:
            if (((pc+4) & 0xf0000000) | ((word & 0x03ffffff) << 2)) != target:
                raise RecompError('direct call has a different destination')
        elif word & 0xfc1fffff != 0xf809:
            raise RecompError('source is not a function call')
    for f in metadata['functions']:
        if f['section'] != tid or f['vram'] >= target+size or f['vram']+f['size'] <= target:
            continue
        if (kind == 'jump' and f is owner) or (kind != 'jump' and f['vram'] == target):
            continue
        if any(target <= a < target+size for a in f['instruction_addresses']):
            raise RecompError('target overlaps a different function')
    return dict(section=sections[sid]['name'], pc=pc, expected_word=word, kind=kind,
                target_section=sections[tid]['name'], target=target, size=size, sha256=record['sha256'])


def verify_generated(edges, metadata, coverage):
    functions = {(f['section'], f['vram']): f for f in metadata['functions']}
    ready = {(f['section'], f['vram']) for f in coverage['functions'] if f['selected'] and f['status'] == 'ready'}
    sections = {s['name']: s['id'] for s in metadata['sections']}
    for edge in edges:
        sid, tid = sections[edge['section']], sections[edge['target_section']]
        pc, target = edge['pc'], edge['target']
        owners = [key for key,f in functions.items() if key[0] == sid and pc in f['instruction_addresses']]
        if len(owners) != 1 or owners[0] not in ready:
            raise RecompError('source no longer has a unique translated owner')
        owner = functions[owners[0]]
        if edge['kind'] != 'jump':
            f = functions.get((tid, target))
            if (not f or (tid, target) not in ready or f['size'] != edge['size'] or f['sha256'] != edge['sha256']):
                raise RecompError('static translation differs from the interpreted function')
        elif target not in owner['instruction_addresses']:
            raise RecompError('local destination is outside the recompiled caller')
        if edge['expected_word'] >> 26 != 3 and not any(
                t['pc'] == pc and t['kind'] == edge['kind']
                and target in (*t['targets'], *t.get('known_targets', [])) for t in owner['indirect_transfers']):
            raise RecompError('static dispatch does not include the observed target')


def promote_observations(game, profile_path, profile, rom, build_dir, directory, write_atomic):
    paths = sorted(directory.glob('*.json'))
    if not paths:
        return
    output = build_dir/'generated'/game
    current = json.loads((output/'symbols.json').read_bytes())
    coverage = json.loads((output/'coverage.json').read_bytes())
    if hashlib.sha1(rom).hexdigest() != current['rom_sha1']:
        generator = load_generator(game)
        with TemporaryDirectory(dir=build_dir, prefix='.cpu-image-') as scratch:
            rom, _ = generator.prepare(rom, deepcopy(profile), Path(scratch))
        if hashlib.sha1(rom).hexdigest() != current['rom_sha1']:
            raise RecompError('prepared ROM differs from the current build')
    original = profile_path.read_bytes()
    known = profile['discovery'].get('observed_transfers', [])
    pending, unresolved = [], {}
    for path in paths:
        try:
            unresolved[path] = json.loads(path.read_bytes())
        except (RecompError, ValueError, KeyError, TypeError) as exc:
            print(f'CPU observation retained for review: {path.name}: {exc}')
    while unresolved:
        remaining = len(unresolved)
        errors = {}
        for path, record in list(unresolved.items()):
            try:
                edge = checked_edge(record, rom, current, coverage, [e for _, e in pending])
                try:
                    verify_generated([edge], current, coverage)
                except RecompError:
                    if edge in known:
                        raise
                    pending.append((path, edge))
                else:
                    path.unlink()  # Discovery already compiles this exact transfer.
                del unresolved[path]
            except (RecompError, ValueError, KeyError, TypeError) as exc:
                errors[path] = exc
        if unresolved and len(unresolved) == remaining:
            for path, reason in errors.items():
                print(f'CPU observation retained for review: {path.name}: {reason}')
            break
    if not pending:
        return
    # Generate in isolation. A failed or interrupted trial cannot damage the
    # current build, profile or the observed evidence.
    before = {(f['section'], f['vram'], f['status']) for f in coverage['functions'] if f['selected']}
    accepted = []

    def validate(batch):
        edges = [edge for _, edge in [*accepted, *batch]]
        with TemporaryDirectory(dir=build_dir, prefix='.cpu-observations-') as trial:
            trial = Path(trial)
            try:
                generate(game, trial, False, observed=edges)
                symbols = json.loads((trial/'symbols.json').read_bytes())
                result = json.loads((trial/'coverage.json').read_bytes())
                after = {(f['section'], f['vram'], f['status']) for f in result['functions'] if f['selected']}
                if not before <= after:
                    raise RecompError('previously compiled functions lost coverage')
                verify_generated(edges, symbols, result)
            except RecompError as exc:
                if len(batch) == 1:
                    print(f'CPU observation retained for review: {batch[0][0].name}: {exc}')
                    return
            else:
                accepted.extend(batch)
                return
        # Isolate a disputed edge so independent valid observations still land.
        middle = len(batch) // 2
        validate(batch[:middle])
        validate(batch[middle:])

    validate(pending)
    if not accepted:
        return
    additions = []
    for _, edge in accepted:
        additions.append('\n[[discovery.observed_transfers]]\n')
        for key,value in edge.items():
            additions.append(f'{key} = {hex(value) if isinstance(value, int) else json.dumps(value)}\n')
    if profile_path.read_bytes() != original:
        raise RecompError('profile changed during observation promotion; retry the build')
    write_atomic(profile_path, original + ''.join(additions).encode())
    for path,_ in accepted:
        path.unlink()
    print(f'Promoted {len(accepted)} interpreted CPU transfers to {profile_path}')
