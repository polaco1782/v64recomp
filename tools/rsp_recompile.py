"""Translate checked RSP instruction images to C11, including IMEM overlays.

Each unique instruction gets a native entry with constant operands. IMEM DMA
binds those entries only after matching a complete range against a declared
image; execution never decodes a guest opcode or compiles unknown RAM.
"""
from __future__ import annotations

import json
from pathlib import Path

from common import RecompError


VECTOR = {
    0x00: 'vmulf', 0x01: 'vmulu', 0x02: 'vrndp', 0x03: 'vmulq',
    0x04: 'vmudl', 0x05: 'vmudm', 0x06: 'vmudn', 0x07: 'vmudh',
    0x08: 'vmacf', 0x09: 'vmacu', 0x0a: 'vrndn', 0x0b: 'vmacq',
    0x0c: 'vmadl', 0x0d: 'vmadm', 0x0e: 'vmadn', 0x0f: 'vmadh',
    0x10: 'vadd', 0x11: 'vsub', 0x13: 'vabs', 0x14: 'vaddc', 0x15: 'vsubc',
    0x1d: 'vsar', 0x20: 'vlt', 0x21: 'veq', 0x22: 'vne', 0x23: 'vge',
    0x24: 'vcl', 0x25: 'vch', 0x26: 'vcr', 0x27: 'vmrg',
    0x28: 'vand', 0x29: 'vnand', 0x2a: 'vor', 0x2b: 'vnor', 0x2c: 'vxor', 0x2d: 'vnxor',
    0x30: 'vrcp', 0x31: 'vrcpl', 0x32: 'vrcph', 0x33: 'vmov',
    0x34: 'vrsq', 0x35: 'vrsql', 0x36: 'vrsqh',
}
MEMORY = ('bv', 'sv', 'lv', 'dv', 'qv', 'rv', 'pv', 'uv', 'hv', 'fv', 'wv', 'tv')
SCALE = (0, 1, 2, 3, 4, 4, 3, 3, 4, 4, 4, 4)


def number(value: int) -> str:
    return f'UINT32_C(0x{value & 0xffffffff:x})'


def instruction(word: int) -> str:
    op, rs, rt, rd, shift, fn = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31, word & 63
    immediate = (word & 0xffff) - (0x10000 if word & 0x8000 else 0)
    a, b = f's->gpr[{rs}]', f's->gpr[{rt}]'

    def assign(reg: int, expression: str) -> str:
        return f's->gpr[{reg}] = {expression};' if reg else f'(void)({expression});'

    def branch(condition: str, link: bool = False) -> str:
        target = f'(at + {number(4 + immediate * 4)}) & 4095'
        if link:
            return f'bool take = {condition}; s->gpr[31] = (at + 8) & 4095; if (take) s->next_pc = {target};'
        return f'if ({condition}) s->next_pc = {target};'

    if op == 0:
        expressions = {
            0x00: f'{b} << {shift}', 0x02: f'{b} >> {shift}',
            0x03: f'(uint32_t)((int32_t){b} >> {shift})',
            0x04: f'{b} << ({a} & 31)', 0x06: f'{b} >> ({a} & 31)',
            0x07: f'(uint32_t)((int32_t){b} >> ({a} & 31))',
            0x20: f'{a} + {b}', 0x21: f'{a} + {b}', 0x22: f'{a} - {b}', 0x23: f'{a} - {b}',
            0x24: f'{a} & {b}', 0x25: f'{a} | {b}', 0x26: f'{a} ^ {b}', 0x27: f'~({a} | {b})',
            0x2a: f'(int32_t){a} < (int32_t){b}', 0x2b: f'{a} < {b}',
        }
        if fn in expressions:
            return assign(rd, expressions[fn])
        if fn == 8:
            return f's->next_pc = {a} & 4092;'
        if fn == 9:
            return f'uint32_t target = {a}; {assign(rd, "(at + 8) & 4095")} s->next_pc = target & 4092;'
        if fn == 13:
            return 's->cp0[4] |= 3; s->halted = true;'
    elif op == 1 and rt in (0, 1, 16, 17):
        return branch(f'(int32_t){a} {">=" if rt & 1 else "<"} 0', rt >= 16)
    elif op in (2, 3):
        return ('s->gpr[31] = (at + 8) & 4095; ' if op == 3 else '') + f's->next_pc = 0x{(word << 2) & 4095:x};'
    elif op in (4, 5, 6, 7):
        if op in (4, 5) and rs == rt:
            return branch('true' if op == 4 else 'false')
        return branch({4: f'{a} == {b}', 5: f'{a} != {b}', 6: f'(int32_t){a} <= 0', 7: f'(int32_t){a} > 0'}[op])
    elif op in (8, 9, 10, 11, 12, 13, 14, 15):
        expression = {
            8: f'{a} + {number(immediate)}', 9: f'{a} + {number(immediate)}',
            10: f'(int32_t){a} < {immediate}', 11: f'{a} < {number(immediate)}',
            12: f'{a} & {number(word & 65535)}', 13: f'{a} | {number(word & 65535)}',
            14: f'{a} ^ {number(word & 65535)}', 15: number(word << 16),
        }[op]
        return assign(rt, expression)
    elif op == 16:
        if rs == 0:
            return assign(rt, f'recomp_rsp_cp0_read(s, {rd})')
        if rs == 4:
            return f'recomp_rsp_cp0_write(s, {rd}, {b});'
    elif op == 18:
        element = (word >> 7) & 15
        if word & 0x02000000:
            element = (word >> 21) & 15
            if fn == 0x37:
                return '(void)s;'
            if fn in VECTOR:
                return f'recomp_rsp_{VECTOR[fn]}(s, {shift}, {rd}, {rt}, {element});'
        elif rs == 0:
            return assign(rt, f'recomp_rsp_mfc2(s, {rd}, {element})')
        elif rs == 2:
            return assign(rt, f'recomp_rsp_cfc2(s, {rd})')
        elif rs == 4:
            return f'recomp_rsp_mtc2(s, {rd}, {element}, {b});'
        elif rs == 6:
            return f'recomp_rsp_ctc2(s, {rd}, {b});'
    elif op in (32, 33, 35, 36, 37):
        size = {32: 1, 33: 2, 35: 4, 36: 1, 37: 2}[op]
        value = f'recomp_rsp_load(s, {a} + {number(immediate)}, {size})'
        if op in (32, 33):
            value = f'(uint32_t)(int32_t)(int{size * 8}_t){value}'
        return assign(rt, value)
    elif op in (40, 41, 43):
        return f'recomp_rsp_store(s, {a} + {number(immediate)}, {b}, { {40: 1, 41: 2, 43: 4}[op]});'
    elif op in (50, 58):
        kind, element = (word >> 11) & 31, (word >> 7) & 15
        if kind < len(MEMORY):
            offset = (word & 127) - (128 if word & 64 else 0)
            return f'recomp_rsp_{"l" if op == 50 else "s"}{MEMORY[kind]}(s, {rt}, {element}, {a} + {number(offset * (1 << SCALE[kind]))});'
    return f'recomp_rsp_fault(s, "unsupported compiled RSP instruction", at, {number(word)});'


def emit_program(output: Path, images: list[tuple[str, bytes]]) -> None:
    if not 1 <= len(images) <= 128:
        raise RecompError('invalid RSP native image count')
    words_by_image = []
    for name, raw in images:
        if not raw or len(raw) % 4 or len(raw) > 65536:
            raise RecompError(f'invalid RSP instruction image {name}')
        words_by_image.append([int.from_bytes(raw[i:i + 4], 'big') for i in range(0, len(raw), 4)])
    words = sorted({word for image in words_by_image for word in image})
    if len(words) >= 65535:
        raise RecompError('RSP native instruction entry count exhausted')
    entries = {word: i for i, word in enumerate(words)}
    lines = ['/* Generated C11 from verified RSP images. Private ROM-derived output. */',
             '#include "rsp.h"',
             'static void rsp_run(RecompRspState *s) {', 'while (!s->halted) {',
             'uint32_t at = s->pc;', 's->instruction_pc = at;',
             'if ((at & 3) || at >= 4096 || ++s->instructions > s->instruction_limit) {',
             'recomp_rsp_fault(s, "RSP execution address or instruction limit", at, 0); break; }',
             'uint16_t entry = s->native[at / 4];',
             's->pc = s->next_pc; s->next_pc = (s->next_pc + 4) & 4095;',
             'switch (entry) {']
    for word in words:
        lines.append(f'case {entries[word]}: {{ {instruction(word)} break; }}')
    lines.extend(['default: recomp_rsp_fault(s, "RSP executed untranslated IMEM", at, entry); break;',
                  '}', 's->gpr[0] = 0;', '}', '}'])
    for index, image in enumerate(words_by_image):
        lines.append(f'static const uint32_t rsp_words_{index}[] = {{')
        for offset in range(0, len(image), 8):
            lines.append(','.join(f'0x{word:08x}' for word in image[offset:offset + 8]) + ',')
        lines.extend(['};', f'static const uint16_t rsp_entries_{index}[] = {{'])
        for offset in range(0, len(image), 16):
            lines.append(','.join(str(entries[word]) for word in image[offset:offset + 16]) + ',')
        lines.append('};')
    lines.append('static const RecompRspCode rsp_images[] = {')
    for index, ((name, _), image) in enumerate(zip(images, words_by_image)):
        lines.append(f'{{{json.dumps(name)}, rsp_words_{index}, rsp_entries_{index}, {len(image)}}},')
    lines.extend(['};', 'const RecompRspProgram game_rsp_program = {rsp_images, sizeof(rsp_images)/sizeof(rsp_images[0]), rsp_run};', ''])
    (output / 'game_rsp.c').write_text('\n'.join(lines))
