#!/usr/bin/env python3
"""Inspect bounded IDO switch idioms; emit evidence, never modify checked inputs."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

from common import RecompError
from emit import decode_function, target
from export_analysis_rom import load_analysis_rom
from generate_game import GAMES


def inspect(rom: bytes, metadata: dict) -> list[dict]:
    results = []
    for function in metadata['functions']:
        instructions = decode_function(rom, function)
        owned = {i.pc: i for i in instructions}
        section = next(s for s in metadata['sections'] if s['id'] == function['section'])
        for jump in instructions:
            if jump.opcode != 'jr' or jump.rs == 31:
                continue
            # Require the complete address-producing chain, not proximity to a
            # plausible table. The shift may be the bounds branch's delay slot.
            chain = [owned.get(jump.pc - n * 4) for n in (4, 3, 2, 1)]
            if any(i is None for i in chain):
                continue
            shift, upper, add, load = chain
            if not (shift.opcode == 'sll' and shift.shift == 2 and shift.rs == 0
                    and upper.opcode == 'lui' and upper.rt != shift.rd
                    and add.opcode == 'addu' and add.rd == upper.rt
                    and {add.rs, add.rt} == {upper.rt, shift.rd}
                    and load.opcode == 'lw' and load.rs == upper.rt
                    and load.rt == jump.rs):
                continue
            guard = None
            for pc in range(shift.pc - 4, max(function['vram'] - 4, shift.pc - 64), -4):
                bound = owned.get(pc)
                if bound is None:
                    break
                if bound.opcode != 'sltiu' or bound.rs != shift.rt or bound.rt == 0:
                    continue
                between = [owned.get(p) for p in range(pc + 4, shift.pc, 4)]
                if any(i is None for i in between):
                    continue
                branches = [i for i in between if i.delay]
                if len(branches) != 1:
                    continue
                branch = branches[0]
                if not (branch.opcode == 'beq' and {branch.rs, branch.rt} == {bound.rt, 0}
                        and target(branch) > jump.pc + 4):
                    continue
                if any(i is not branch and (i.delay or i.gpr_write_mask &
                       ((1 << bound.rt) | (1 << shift.rt))) for i in between):
                    continue
                guard = bound
                break
            if guard is None:
                continue
            count = guard.word & 0xffff
            address = ((upper.word & 0xffff) * 65536 + load.immediate) & 0xffffffff
            if not 0 < count <= 8192 or address % 4:
                continue
            # Preserve section identity for overlapping overlay slots.
            if not section['vram'] <= address < address + count * 4 <= section['vram'] + section['size']:
                continue
            offset = section['rom_offset'] + address - section['vram']
            raw = rom[offset:offset + count * 4]
            targets = [int.from_bytes(raw[i:i + 4], 'big') for i in range(0, len(raw), 4)]
            local = all(t % 4 == 0 and section['vram'] <= t < address for t in targets)
            results.append(dict(section=section['name'], function=function['vram'],
                                pc=jump.pc, expected_word=jump.word, table_vram=address,
                                count=count, sha256=hashlib.sha256(raw).hexdigest(),
                                bounds_pc=guard.pc, targets=targets,
                                targets_in_section_text=local,
                                instructions=[dict(pc=owned[p].pc, word=owned[p].word,
                                                   assembly=owned[p].assembly)
                                              for p in range(guard.pc, jump.pc + 8, 4)
                                              if p in owned]))
    return results


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', choices=GAMES, required=True)
    parser.add_argument('--metadata', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    rom, metadata = load_analysis_rom(args.game, args.metadata)
    tables = inspect(rom, metadata)
    args.output.write_text(json.dumps(dict(rom_sha1=metadata['rom_sha1'], tables=tables), indent=2) + '\n')
    print(f'{len(tables)} bounded switch candidates; review control-flow entry, targets and ownership: {args.output}')


if __name__ == '__main__':
    try:
        main()
    except (RecompError, OSError, ValueError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
