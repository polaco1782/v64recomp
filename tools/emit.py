"""A small two-pass C emitter. Unsupported instructions are reported explicitly."""
from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
import struct
import sys

import rabbitizer

from common import RecompError

# Set to True to log translation progress, issues, and native OS/HW hooks.
# Messages go to stderr; False keeps the emitter silent.
LOG_EMISSION = True
# Per-function/instruction traces (calls, checkpoints, memory) are noisy; opt in here.
LOG_EMISSION_VERBOSE = False


def log_emission(message: str, *args: object, verbose: bool = False) -> None:
    if LOG_EMISSION and (not verbose or LOG_EMISSION_VERBOSE):
        print('[emit] ' + (message % args if args else message), file=sys.stderr, flush=True)


# Canonical opcodes keep pseudoinstructions out of the lowering rules.
rabbitizer.config.pseudos_enablePseudos = False

SUPPORTED = set("""
nop lui addiu addi addu add subu sub daddiu daddu dsubu
and or xor nor andi ori xori slt sltu slti sltiu
sll srl sra sllv srlv srav dsll dsrl dsra dsll32 dsrl32 dsra32 dsllv dsrlv dsrav
mult multu div divu mfhi mflo mthi mtlo
dmult dmultu ddiv ddivu break
lb lbu lh lhu lw lwu ld sb sh sw sd
lwl lwr ldl ldr swl swr sdl sdr
mtc1 mfc1 dmtc1 dmfc1 lwc1 swc1 ldc1 sdc1
beq bne blez bgtz bltz bgez beql bnel blezl bgtzl bltzl bgezl b
j jal jalr jr
""".split())

FP_BINARY = {'add', 'sub', 'mul', 'div'}
FP_UNARY = {'sqrt', 'abs', 'neg', 'mov'}
FP_CONDITIONS = ('f', 'un', 'eq', 'ueq', 'olt', 'ult', 'ole', 'ule',
                 'sf', 'ngle', 'seq', 'ngl', 'lt', 'nge', 'le', 'ngt')
SUPPORTED.update(f'{op}.{fmt}' for op in FP_BINARY | FP_UNARY for fmt in ('s', 'd'))
SUPPORTED.update(f'c.{cond}.{fmt}' for cond in FP_CONDITIONS for fmt in ('s', 'd'))
# Rabbitizer spells the double signaling-false predicate "c.df.d".
SUPPORTED.add('c.df.d')
SUPPORTED.update(('cfc1', 'ctc1', 'bc1f', 'bc1t', 'bc1fl', 'bc1tl',
                  'cvt.s.d', 'cvt.s.w', 'cvt.s.l', 'cvt.d.s', 'cvt.d.w', 'cvt.d.l',
                  'cvt.w.s', 'cvt.w.d', 'cvt.l.s', 'cvt.l.d'))
SUPPORTED.update(f'{op}.{integer}.{fmt}' for op in ('round', 'trunc', 'ceil', 'floor')
                 for integer in ('w','l') for fmt in ('s', 'd'))


def fp_error(i: 'Instruction', fpu_fr: int = 0) -> str | None:
    op = i.opcode
    if op in ('cfc1', 'ctc1') and i.rd != 31:
        return 'only FCR31 control access is supported'
    if op.startswith('bc1') and i.rt > 3:
        return 'only floating-point condition code zero is supported'
    parts = op.split('.')
    if len(parts) < 2 or parts[-1] not in ('s', 'd', 'w', 'l'):
        return None
    if parts[0] == 'c' and i.shift:
        return 'only floating-point condition code zero is supported'
    double_regs = []
    if parts[-1] in ('d','l'):
        double_regs.append(i.rd)  # fs
        if parts[0] in FP_BINARY or parts[0] == 'c': double_regs.append(i.rt)  # ft
    if ((len(parts) == 2 and parts[-1] in ('d','l')) or (len(parts) == 3 and parts[1] in ('d','l'))):
        double_regs.append(i.shift)  # fd
    if fpu_fr == 0 and any(reg % 2 for reg in double_regs):
        return 'FR=0 double register must be even'
    return None


@dataclass
class Instruction:
    pc: int
    word: int
    opcode: str
    assembly: str
    delay: bool
    likely: bool
    rs: int
    rt: int
    rd: int
    shift: int
    immediate: int


def decode(raw: bytes, address: int) -> list[Instruction]:
    result = []
    for offset in range(0, len(raw), 4):
        word = struct.unpack_from(">I", raw, offset)[0]
        pc = address + offset
        instruction = rabbitizer.Instruction(word, pc)
        opcode = instruction.getOpcodeName() if instruction.isValid() else "invalid"
        imm = word & 0xffff
        result.append(Instruction(pc, word, opcode, instruction.disassemble(),
                                  instruction.hasDelaySlot(), instruction.isBranchLikely(),
                                  (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31,
                                  (word >> 6) & 31, imm - 65536 if imm & 0x8000 else imm))
    return result


def cname(function: dict) -> str:
    return f"recomp_s{function['section']}_{function['vram']:08x}"


def target(instruction: Instruction) -> int:
    if instruction.opcode in ('j', 'jal'):
        return ((instruction.pc + 4) & 0xf0000000) | ((instruction.word & 0x03ffffff) << 2)
    return (instruction.pc + 4 + instruction.immediate * 4) & 0xffffffff


def resolve_function(registry: dict, section: int, address: int) -> dict | None:
    if (section, address) in registry:
        return registry[(section, address)]
    matches = [f for (s, a), f in registry.items() if a == address]
    return matches[0] if len(matches) == 1 else None


def instruction_addresses(function: dict) -> set[int]:
    return set(function.get('instruction_addresses',
                            range(function['vram'], function['vram'] + function['size'], 4)))


def decode_function(rom: bytes, function: dict) -> list[Instruction]:
    if 'instruction_addresses' not in function:
        return decode(rom[function['rom_offset']:function['rom_offset'] + function['size']], function['vram'])
    result = []
    for pc in function['instruction_addresses']:
        offset = function['rom_offset'] + pc - function['vram']
        result.extend(decode(rom[offset:offset + 4], pc))
    return result


def analyze(function: dict, instructions: list[Instruction], registry: dict, fpu_fr: int = 0) -> list[dict]:
    errors = []
    addresses = instruction_addresses(function)
    transfers = {t['pc']: t for t in function.get('indirect_transfers', [])}
    for transfer in function.get('fallthrough_transfers', []):
        i=next((i for i in instructions if i.pc==transfer['pc']),None)
        if (i is None or i.delay or i.word!=transfer['expected_word'] or transfer['target']!=i.pc+4
                or resolve_function(registry,function['section'],transfer['target']) is None):
            errors.append({'pc':transfer['pc'],'word':transfer['expected_word'],'assembly':'',
                           'reason':'invalid checked function fallthrough'})
    for index, i in enumerate(instructions):
        reason = None
        transfer = transfers.get(i.pc)
        registered = transfer and transfer.get('registered_targets') is True
        if i.opcode not in SUPPORTED:
            reason = f"unsupported instruction: {i.opcode}"
        elif fp_error(i, fpu_fr):
            reason = fp_error(i, fpu_fr)
        elif fpu_fr == 0 and ((i.opcode in ('ldc1', 'sdc1') and i.rt % 2)
              or (i.opcode in ('dmfc1','dmtc1') and i.rd % 2)):
            reason = 'FR=0 double register must be even'
        elif i.opcode == 'jalr' and i.rd != 31:
            reason = "nonstandard link register requires a local replacement"
        elif transfer and (transfer['expected_word'] != i.word
                           or transfer['kind'] not in ('jump', 'tail', 'call')
                           or (not transfer['targets'] and not registered)
                           or len(set(transfer['targets'])) != len(transfer['targets'])
                           or (registered and (transfer['kind']!='call' or transfer['targets']))
                           or (transfer['kind'] == 'call' and i.opcode != 'jalr')
                           or (transfer['kind'] != 'call' and (i.opcode != 'jr' or i.rs == 31))):
            reason = 'invalid checked indirect-transfer metadata'
        elif transfer and any((a not in addresses if transfer['kind'] == 'jump'
                               else resolve_function(registry, function['section'], a) is None)
                              for a in transfer['targets']):
            reason = 'unresolved checked indirect-transfer target'
        elif i.opcode == 'jr' and i.rs != 31 and transfer is None:
            reason = "computed jump requires explicit jump-table targets"
        elif i.delay:
            if (index + 1 == len(instructions) or instructions[index + 1].pc != i.pc + 4
                    or instructions[index + 1].delay):
                reason = "missing delay slot or control transfer inside delay slot"
            elif i.opcode not in ('jr', 'jalr'):
                destination = target(i)
                local = destination in addresses and i.opcode != 'jal'
                if not local and resolve_function(registry, function['section'], destination) is None:
                    reason = f"unresolved target: {destination:#010x}"
                if local and destination % 4:
                    reason = "unaligned branch target"
        if reason:
            errors.append({"pc": i.pc, "word": i.word, "assembly": i.assembly, "reason": reason})
    return errors


def reg(number: int) -> str:
    return f"ctx->gpr[{number}]" if number else "UINT64_C(0)"


def add_immediate(value: str, immediate: int) -> str:
    if immediate == 0:
        return value
    return f"{value} {'+' if immediate > 0 else '-'} {abs(immediate):#x}"


def signed_constant(value: int, bits: int) -> str:
    value &= (1 << bits) - 1
    if value & (1 << (bits - 1)):
        value -= 1 << bits
    return f"UINT64_C(0x{value & 0xffffffffffffffff:x})"


def memory_address(i: Instruction) -> str:
    if i.rs == 0:
        return f"UINT32_C(0x{i.immediate & 0xffffffff:x})"
    return add_immediate(reg(i.rs), i.immediate)


def body(i: Instruction) -> str:
    op, a, b = i.opcode, reg(i.rs), reg(i.rt)
    imm = f"{i.immediate:#x}"
    logical_imm = f"UINT64_C(0x{i.word & 0xffff:04x})"
    destination = i.rt
    expression = None
    if op == 'nop': return ";"
    if op == 'break': return f'recomp_fail("MIPS BREAK {i.word >> 6 & 0xfffff}", UINT32_C(0x{i.pc:08x}));'
    if op == 'mtc1': return f'ctx->fpr[{i.rd}] = {b};'
    if op == 'mfc1': return f'mips_set(ctx, {i.rt}, mips_s32(ctx->fpr[{i.rd}]));'
    if op == 'cfc1': return f'mips_set(ctx, {i.rt}, mips_s32(ctx->fcr31));'
    if op == 'ctc1': return f'mips_fp_set_control(ctx, {b});'
    parts = op.split('.')
    if op == 'dmtc1': return f'mips_set_fpr_pair(ctx, {i.rd}, {b});'
    if op == 'dmfc1': return f'mips_set(ctx, {i.rt}, mips_fpr_pair(ctx, {i.rd}));'
    if len(parts) >= 2 and parts[-1] in ('s', 'd', 'w', 'l'):
        fmt = f'MIPS_FP_{parts[-1].upper()}'
        if parts[0] in FP_BINARY:
            return f'mips_fp_binary(ctx, {i.shift}, {i.rd}, {i.rt}, {fmt}, MIPS_FP_{parts[0].upper()});'
        if parts[0] in FP_UNARY:
            return f'mips_fp_unary(ctx, {i.shift}, {i.rd}, {fmt}, MIPS_FP_{parts[0].upper()});'
        if parts[0] == 'c':
            return f'mips_fp_compare(ctx, {i.rd}, {i.rt}, {fmt}, {i.word & 15});'
        if parts[0] in ('cvt', 'round', 'trunc', 'ceil', 'floor'):
            mode = {'cvt': -1, 'round': 0, 'trunc': 1, 'ceil': 2, 'floor': 3}[parts[0]]
            return f'mips_fp_convert(ctx, {i.shift}, {i.rd}, {fmt}, MIPS_FP_{parts[1].upper()}, {mode});'
    if op in ('lwc1', 'swc1', 'ldc1', 'sdc1'):
        address = memory_address(i)
        if op == 'lwc1': return f'ctx->fpr[{i.rt}] = (uint32_t)recomp_load(runtime, {address}, 4);'
        if op == 'swc1': return f'recomp_store(runtime, {address}, ctx->fpr[{i.rt}], 4);'
        if op == 'ldc1': return f'mips_set_fpr_pair(ctx, {i.rt}, recomp_load(runtime, {address}, 8));'
        return f'recomp_store(runtime, {address}, mips_fpr_pair(ctx, {i.rt}), 8);'
    if op in ('lwl', 'lwr', 'ldl', 'ldr', 'swl', 'swr', 'sdl', 'sdr'):
        address = memory_address(i)
        width = 4 if op[1] == 'w' else 8
        left = 'true' if op.endswith('l') else 'false'
        if op.startswith('s'):
            return f'recomp_store_merge(runtime, {address}, {b}, {width}, {left});'
        expression = f'recomp_load_merge(runtime, {address}, {b}, {width}, {left})'
        if width == 4: expression = f'mips_s32({expression})'
        return f'mips_set(ctx, {i.rt}, {expression});'
    if op == 'lui': expression = signed_constant((i.word & 0xffff) << 16, 32)
    elif op in ('addiu', 'addi', 'daddiu'):
        expression = (f"mips_s32({add_immediate(a, i.immediate)})" if op == 'addiu'
                      else f"mips_add32({a}, {imm})" if op == 'addi' else add_immediate(a, i.immediate))
    elif op in ('addu', 'add', 'subu', 'sub', 'daddu', 'dsubu'):
        destination = i.rd
        sign = '+' if op in ('addu', 'add', 'daddu') else '-'
        # Unsigned 64-bit arithmetic followed by narrowing has the same low
        # 32 bits as narrowing both operands before the wrapping operation.
        expression = (f"mips_s32({a} {sign} {b})" if op in ('addu', 'subu')
                      else f"mips_{'add' if op == 'add' else 'sub'}32({a}, {b})" if op in ('add', 'sub')
                      else f"{a} {sign} {b}")
    elif op in ('and', 'or', 'xor', 'nor', 'andi', 'ori', 'xori'):
        operation = {'and': '&', 'or': '|', 'xor': '^', 'nor': '|', 'andi': '&', 'ori': '|', 'xori': '^'}[op]
        immediate = op.endswith('i')
        destination = i.rt if immediate else i.rd
        expression = f"{a} {operation} {logical_imm if immediate else b}"
        if op == 'nor': expression = f"~({expression})"
    elif op in ('slt', 'sltu', 'slti', 'sltiu'):
        immediate = op.startswith('slti')
        destination = i.rt if immediate else i.rd
        rhs = imm if immediate else b
        if op == 'sltiu' and i.immediate < 0:
            rhs = signed_constant(i.immediate, 16)
        expression = ('UINT64_C(0)' if op == 'sltiu' and i.immediate == 0
                      else f"{a} < {rhs}" if op.endswith('u') else f"mips_signed_less({a}, {rhs})")
    elif op in ('sll', 'srl', 'sra', 'sllv', 'srlv', 'srav', 'dsll', 'dsrl', 'dsra',
                'dsll32', 'dsrl32', 'dsra32', 'dsllv', 'dsrlv', 'dsrav'):
        destination = i.rd
        wide = op.startswith('d')
        count = f"({a} & {63 if wide else 31})" if op.endswith('v') else str(i.shift + (32 if op.endswith('32') else 0))
        if 'sra' in op: expression = f"mips_sra{64 if wide else 32}({b}, {count})"
        else:
            operation = '<<' if 'sll' in op else '>>'
            operand = f"(uint32_t){b}" if not wide and operation == '>>' else b
            expression = f"{b} {operation} {count}" if wide else f"mips_s32({operand} {operation} {count})"
    elif op in ('mfhi', 'mflo'):
        destination, expression = i.rd, f"ctx->{'hi' if op == 'mfhi' else 'lo'}"
    elif op in ('mthi', 'mtlo'): return f"ctx->{'hi' if op == 'mthi' else 'lo'} = {a};"
    elif op in ('mult', 'multu', 'div', 'divu'):
        return f"mips_{'mult' if op.startswith('mult') else 'div'}(ctx, {a}, {b}, {'false' if op.endswith('u') else 'true'});"
    elif op in ('dmult', 'dmultu', 'ddiv', 'ddivu'):
        return f"mips_{'dmult' if op.startswith('dmult') else 'ddiv'}(ctx, {a}, {b}, {'false' if op.endswith('u') else 'true'});"
    elif op in ('lb', 'lbu', 'lh', 'lhu', 'lw', 'lwu', 'ld', 'sb', 'sh', 'sw', 'sd'):
        width = {'b': 1, 'h': 2, 'w': 4, 'd': 8}[op[1]]
        address = memory_address(i)
        if op.startswith('s'): return f"recomp_store(runtime, {address}, {b}, {width});"
        expression = f"recomp_load(runtime, {address}, {width})"
        if op in ('lb', 'lh', 'lw'): expression = f"mips_s{width * 8}({expression})"
    if expression is None:
        raise RecompError(f"no straight-line emitter for {op}")
    return f"mips_set(ctx, {destination}, {expression});"


def constant_condition(i: Instruction) -> bool | None:
    """Resolve only outcomes proven by register identity or the zero register."""
    op = i.opcode.rstrip('l')
    if op == 'b' or op == 'beq' and i.rs == i.rt:
        return True
    if op == 'bne' and i.rs == i.rt:
        return False
    if i.rs == 0:
        if op in ('bgez', 'blez'):
            return True
        if op in ('bltz', 'bgtz'):
            return False
    return None


def condition(i: Instruction) -> str:
    op, a, b = i.opcode.rstrip('l'), reg(i.rs), reg(i.rt)
    if op == 'b': return 'true'
    if op == 'bc1t': return '(ctx->fcr31 & UINT32_C(0x800000)) != 0'
    if op == 'bc1f': return '(ctx->fcr31 & UINT32_C(0x800000)) == 0'
    if op == 'beq': return f"{a} == {b}"
    if op == 'bne': return f"{a} != {b}"
    if op in ('bltz', 'bgez'):
        return f"{'!' if op == 'bgez' else ''}mips_signed_less({a}, 0)"
    if op == 'blez': return f"mips_signed_less({a}, 0) || {a} == 0"
    if op == 'bgtz': return f"!mips_signed_less({a}, 0) && {a} != 0"
    raise RecompError(f"no branch condition for {i.opcode}")


def direct_hook_targets(registry: dict, hooks: dict[tuple[int, int], str]) -> dict[int, tuple[int, str]]:
    # A shared guest address must retain runtime loaded-section resolution.
    counts = Counter(address for _, address in registry)
    return {address: (section, handler) for (section, address), handler in hooks.items()
            if counts[address] == 1}


def emit_function(function: dict, instructions: list[Instruction], registry: dict,
                  yield_sites: set[int] | None = None,
                  hooks: dict[tuple[int, int], str] | None = None,
                  direct_hooks: dict[int, tuple[int, str]] | None = None) -> str:
    log_emission('Emit %s s%s 0x%08x -> %s (%d instructions)',
                 function['name'], function['section'], function['vram'], cname(function), len(instructions),
                 verbose=True)
    safe_name = function['name'].replace('*/', '* /').replace('\n', ' ')
    lines = [f"/* {safe_name} at 0x{function['vram']:08x} */",
             f"void {cname(function)}(Runtime *runtime, MipsContext *ctx) {{",
             "    uint32_t return_address = (uint32_t)ctx->gpr[31];",
             "    (void)return_address; (void)runtime; (void)ctx;"]
    addresses = instruction_addresses(function)
    transfers = {t['pc']: t for t in function.get('indirect_transfers', [])}
    fallthrough_transfers = {t['pc']: t for t in function.get('fallthrough_transfers', [])}
    used_labels = set()
    yield_sites = yield_sites or set()
    hooks = hooks or {}
    if direct_hooks is None:
        direct_hooks = direct_hook_targets(registry, hooks)

    def emit_body(instruction: Instruction, indent: str = '    ') -> list[str]:
        statement = body(instruction)
        if LOG_EMISSION and LOG_EMISSION_VERBOSE and ('recomp_load' in statement or 'recomp_store' in statement):
            log_emission('Memory %s s%s 0x%08x: %s -> %s',
                         function['name'], function['section'], instruction.pc, instruction.assembly, statement,
                         verbose=True)
        # These helpers report guest traps through ctx->pc. Ordinary arithmetic
        # and memory operations do not consume it; memory errors report the address.
        location = ([f"{indent}ctx->pc = UINT32_C(0x{instruction.pc:08x});"]
                    if statement.startswith('mips_fp_') else [])
        return [*location, indent + statement]

    def call_location(indent: str) -> str:
        # Native hooks can suspend inside nested generated calls. Preserve the
        # call site for guest OSThread/debug snapshots; C frames own resumption.
        return f"{indent}ctx->pc = UINT32_C(0x{i.pc:08x});"

    def fallthrough(address: int, indent: str = '    ') -> str:
        return f'{indent}recomp_fellthrough(UINT32_C(0x{address:08x}));'

    def jump(address: int, indent: str = '    ') -> str:
        used_labels.add(f'L_{address:08x}: ;')
        return f'{indent}goto L_{address:08x};'

    def log_call(address: int, kind: str) -> None:
        if not LOG_EMISSION or not LOG_EMISSION_VERBOSE:
            return
        callee = resolve_function(registry, function['section'], address)
        if callee is None:
            log_emission('%s %s s%s 0x%08x -> 0x%08x (resolved by recomp_lookup at runtime)',
                         kind, function['name'], function['section'], i.pc, address, verbose=True)
            return
        handler = hooks.get((callee['section'], callee['vram']))
        log_emission('%s %s s%s 0x%08x -> %s s%s 0x%08x -> %s (%s; %s)',
                     kind, function['name'], function['section'], i.pc,
                     callee['name'], callee['section'], address, handler or cname(callee),
                     'native hook' if handler else 'generated function',
                     'direct native call' if address in direct_hooks else 'via recomp_lookup', verbose=True)

    def emit_call(address: int, indent: str = '        ') -> list[str]:
        hook = direct_hooks.get(address)
        if hook is not None:
            section, handler = hook
            return [f"{indent}recomp_prepare_direct_call(runtime, UINT32_C(0x{address:08x}), {section});",
                    f"{indent}{handler}(runtime, ctx);"]
        return [f"{indent}recomp_lookup(runtime, UINT32_C(0x{address:08x}))(runtime, ctx);"]

    def transfer(address: int, indent: str = '        ') -> list[str]:
        if address in addresses:
            checkpoint = []
            if address <= i.pc:
                call = 'recomp_yield_checkpoint' if i.pc in yield_sites else 'recomp_checkpoint'
                log_emission('Checkpoint %s s%s 0x%08x -> 0x%08x: %s',
                             function['name'], function['section'], i.pc, address, call, verbose=True)
                checkpoint.append(f"{indent}{call}(runtime);")
            return [*checkpoint, jump(address, indent)]
        log_call(address, 'Tail/branch call')
        return [*emit_call(address, indent), f"{indent}return;"]

    for index, i in enumerate(instructions):
        lines.extend([f"L_{i.pc:08x}: ;", f"    /* {i.pc:08x}: {i.assembly} */"])
        if not i.delay:
            lines.extend(emit_body(i))
            if 'instruction_addresses' in function:
                checked_fallthrough = fallthrough_transfers.get(i.pc)
                if checked_fallthrough:
                    lines.extend([*emit_call(checked_fallthrough['target'], '    '), '    return;'])
                else:
                    # Fall through only when the next emitted instruction is
                    # the guest successor. Sparse/reordered bodies can need a jump.
                    if i.pc + 4 in addresses:
                        if index + 1 == len(instructions) or instructions[index + 1].pc != i.pc + 4:
                            lines.append(jump(i.pc + 4))
                    else:
                        lines.append(fallthrough(i.pc))
            continue
        slot = instructions[index + 1]
        lines.append('    {')
        if i.opcode not in ('j', 'jal', 'jr', 'jalr'):
            known = constant_condition(i)
            if known is True:
                lines.extend(emit_body(slot, '        '))
                lines.extend(transfer(target(i)))
                lines.append('    }')
                continue
            # Test before the delay slot can change branch inputs. Each path
            # executes the slot once; branch-likely annuls it when not taken.
            if known is None:
                lines.append(f"        if ({condition(i)}) {{")
                lines.extend(emit_body(slot, '            '))
                lines.extend(transfer(target(i), '            '))
                lines.append('        }')
            if not i.likely:
                lines.extend(emit_body(slot, '        '))
            lines.append(jump(i.pc + 8, '        ') if i.pc + 8 in addresses
                         else fallthrough(i.pc, '        '))
            lines.append('    }')
            continue
        if i.opcode in ('jr', 'jalr'):
            lines.append(f"        uint32_t destination = (uint32_t){reg(i.rs)};")
        if i.opcode in ('jal', 'jalr'):
            lines.append(f"        mips_set(ctx, {31 if i.opcode == 'jal' else i.rd}, {signed_constant(i.pc + 8, 32)});")
        lines.extend(emit_body(slot, '        '))
        checked = transfers.get(i.pc)
        if i.opcode == 'jr' and checked:
            lines.append('        switch (destination) {')
            for address in checked['targets']:
                lines.append(f'        case UINT32_C(0x{address:08x}):')
                if checked['kind'] == 'jump':
                    lines.extend(transfer(address, '            '))
                else:
                    log_call(address, 'Checked indirect tail target')
                    lines.extend([call_location('            '),
                                  *emit_call(address, '            '), '            return;'])
            lines.extend(['        default: recomp_fail("unlisted indirect jump target", destination);', '        }'])
        elif i.opcode == 'jr':
            if i.rs != 31:
                log_emission('Indirect tail %s s%s 0x%08x: %s -> recomp_lookup(runtime, destination)',
                             function['name'], function['section'], i.pc, i.assembly, verbose=True)
            lines.extend(['        if (destination != return_address) {',
                          call_location('            '),
                          '            recomp_lookup(runtime, destination)(runtime, ctx);', '        }', '        return;'])
        elif i.opcode in ('jal', 'jalr'):
            dest = 'destination' if i.opcode == 'jalr' else f"UINT32_C(0x{target(i):08x})"
            if checked and not checked.get('registered_targets'):
                lines.append('        switch (destination) {')
                for address in checked['targets']:
                    log_call(address, 'Checked indirect call target')
                    lines.extend([f'        case UINT32_C(0x{address:08x}):', call_location('            '),
                                  *emit_call(address, '            '), '            break;'])
                lines.extend(['        default: recomp_fail("unlisted indirect call target", destination);', '        }'])
            else:
                if i.opcode == 'jalr':
                    log_emission('Indirect call %s s%s 0x%08x: %s -> recomp_lookup(runtime, destination)',
                                 function['name'], function['section'], i.pc, i.assembly, verbose=True)
                    call = [f"        recomp_lookup(runtime, {dest})(runtime, ctx);"]
                else:
                    log_call(target(i), 'Direct call')
                    call = emit_call(target(i))
                lines.extend([call_location('        '), *call])
            lines.append(jump(i.pc + 8, '        ') if i.pc + 8 in addresses
                         else fallthrough(i.pc, '        '))
        elif i.opcode == 'j': lines.extend(transfer(target(i)))
        lines.append('    }')
    last_pc = instructions[-1].pc if instructions else function['vram']
    lines.extend([fallthrough(last_pc), '}'])
    # Only control-flow destinations need labels. Delay slots remain separately
    # addressable, and call/branch continuations still jump over their inline copy.
    return '\n'.join(line for line in lines if not line.startswith('L_') or line in used_labels)
