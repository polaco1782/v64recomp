#!/usr/bin/env python3
"""Check overlapping section discovery and native dispatch with synthetic ROM bytes.

Run with --build pointing to an existing Debug or Release CMake build.
"""
from copy import deepcopy
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from common import RecompError
from discover import discover
from emit import FunctionRegistry, analyze, cname, decode_function, emit_function
from inspect_function_candidates import inspect_candidates
from translate import translate
import native_hooks

PROJECT = Path(__file__).resolve().parents[1]


def rejected(rom, profile):
    try:
        discover(rom, profile)
    except RecompError:
        return
    raise AssertionError('ambiguous or unchecked overlay was accepted')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    build = args.build.resolve()
    debug = 'CMAKE_BUILD_TYPE:STRING=Debug' in (build / 'CMakeCache.txt').read_text()
    rom = bytearray(0x180)
    struct.pack_into('>I', rom, 8, 0x80000400)
    struct.pack_into('>2I', rom, 0x40, 0x03e00008, 0)
    regions = [dict(name='.boot', vram=0x80000400, rom_offset=0x40, size=8)]
    tables, entries = [], []
    for index, value in enumerate((11, 22)):
        name, offset = f'.overlay{index}', 0x80 + index * 0x80
        caller = [0x03e04025, 0x0c000410, 0, 0x0100f825, 0x03e00008, 0]
        struct.pack_into('>6I', rom, offset, *caller)
        body = [0x00600008, 0, 0x24020000 | value, 0x03e00008, 0,
                0, 0x24020000 | (value + 1), 0x03e00008, 0]
        struct.pack_into(f'>{len(body)}I', rom, offset + 0x40, *body)
        struct.pack_into('>2I', rom, offset + 0x68, 0x80001048, 0x80001058)
        regions.append(dict(name=name, vram=0x80001000, rom_offset=offset,
                            size=0x80, overlay_group='slot'))
        entries.append(dict(section=name, vram=0x80001000, origin='checked-root',
                            sha256=hashlib.sha256(rom[offset:offset+16]).hexdigest()))
        tables.append(dict(section=name, pc=0x80001040, expected_word=0x00600008,
                           table_vram=0x80001068, count=2,
                           sha256=hashlib.sha256(rom[offset+0x68:offset+0x70]).hexdigest()))
    profile = dict(game=dict(name='synthetic overlays', entry_symbol='entry',
                            code_sections=[r['name'] for r in regions], boot_sections=['.boot']),
                   discovery=dict(regions=regions, jump_tables=tables), native_hooks=dict(enabled=False))
    with tempfile.TemporaryDirectory(prefix='v64-overlays-') as directory:
        output = Path(directory)
        inventory = dict(format_version=1, rom_sha1=hashlib.sha1(rom).hexdigest(),
                         complete=False, entries=entries)
        path = output / 'entries.json'
        path.write_text(json.dumps(inventory))
        profile['discovery']['entry_inventory'] = path
        metadata = discover(bytes(rom), profile)
        assert len(metadata['functions']) == 5 and not metadata['discovery_summary']['unresolved_functions']
        report = translate(bytes(rom), metadata, profile, output/'report',
                           names=[f['name'] for f in metadata['functions']], report_only=True)
        assert report['summary'] == {'ready': 5}
        inspect_candidates(bytes(rom), metadata)
        # Identical prefixes at the same VRAM still require each section's full
        # native body. A custom replacement overrides only its selected section.
        original_catalog = native_hooks.catalog
        signatures = []
        for index in range(2):
            offset = 0xc0 + index * 0x80
            signatures.append(dict(symbol='fixture', handler='hook_fixture',
                                   expected_words=[0x00600008, 0], body_size=36,
                                   sha256=hashlib.sha256(rom[offset:offset+36]).hexdigest()))
        native_hooks.catalog = lambda: tuple(signatures)
        try:
            native_profile = deepcopy(profile)
            native_profile['native_hooks']['enabled'] = True
            hooks = native_hooks.resolve_native_hooks(bytes(rom), native_profile)
            assert {h['section'] for h in hooks} == {'.overlay0', '.overlay1'}
            assert len({h['symbol'] for h in hooks}) == 2
            native_profile['hooks'] = [dict(hooks[0], symbol='custom')]
            hooks = native_hooks.resolve_native_hooks(bytes(rom), native_profile)
            assert len(hooks) == 2 and sum(h['symbol'] == 'custom' for h in hooks) == 1
        finally:
            native_hooks.catalog = original_catalog
        registry = FunctionRegistry(metadata['functions'])
        bodies = []
        for function in metadata['functions']:
            instructions = decode_function(rom, function)
            assert not analyze(function, instructions, registry)
            bodies.append(emit_function(function, instructions, registry))
        bad = deepcopy(profile)
        bad['discovery']['regions'][-1].pop('overlay_group')
        rejected(bytes(rom), bad)
        bad = deepcopy(profile)
        bad['discovery']['jump_tables'][-1].pop('section')
        rejected(bytes(rom), bad)
        bad = deepcopy(profile)
        bad['discovery']['jump_tables'][-1]['sha256'] = '0' * 64
        rejected(bytes(rom), bad)
        inventory['entries'][0].pop('section')
        path.write_text(json.dumps(inventory))
        rejected(bytes(rom), profile)

        (output / 'generated.h').write_text('#include "recomp.h"\n')
        source = '#include "runtime.h"\n#include <assert.h>\n' + '\n'.join(bodies)
        source += '\nstatic const RecompFunctionEntry entries[] = {\n'
        for function in metadata['functions']:
            source += (f'{{.address=0x{function["vram"]:x},.size={function["size"]},'
                       f'.section={function["section"]},.function={cname(function)},.name="fixture"}},\n')
        source += '''};
int main(void) {
    bool loaded[] = {true, false, false}; Runtime rt;
    assert(recomp_runtime_init(&rt, 0x400000, entries, sizeof(entries)/sizeof(entries[0]), loaded, 3));
    for (unsigned section = 1; section <= 2; section++) {
        rt.section_loaded[1] = section == 1; rt.section_loaded[2] = section == 2;
        for (unsigned path = 0; path < 2; path++) {
            MipsContext ctx = {0}; ctx.gpr[3] = mips_s32(0x80001048 + 16 * path);
            ctx.gpr[31] = mips_s32(0xf0000000);
            recomp_lookup(&rt, 0x80001000)(&rt, &ctx);
            assert(ctx.gpr[2] == section * 11 + path);
        }
    }
    recomp_runtime_destroy(&rt);
}
'''
        (output / 'check.c').write_text(source)
        for optimization in ('-O0', '-O2'):
            command = [args.cc, '-std=c11', optimization, '-I'+str(PROJECT/'include'), '-I'+str(output)]
            if debug:
                command.append('-DRECOMP_DEBUG')
            command += [str(output/'check.c'), str(build/'librecomp_runtime.a'),
                        '-lpthread', '-lcrypto', '-ldl', '-lm', '-o', str(output/'check')]
            subprocess.run(command, check=True)
            subprocess.run([str(output/'check')], check=True)
    print('Overlay identities, checked switches, rejected ambiguities and native dispatch passed at O0/O2')


if __name__ == '__main__':
    main()
