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
nop lui addiu addi addu add subu sub daddiu dadd daddu dsubu
and or xor nor andi ori xori slt sltu slti sltiu
sll srl sra sllv srlv srav dsll dsrl dsra dsll32 dsrl32 dsra32 dsllv dsrlv dsrav
mult multu div divu mfhi mflo mthi mtlo
dmult dmultu ddiv ddivu break syscall
lb lbu lh lhu lw lwu ld sb sh sw sd
lwl lwr ldl ldr swl swr sdl sdr
mtc1 mfc1 dmtc1 dmfc1 lwc1 swc1 ldc1 sdc1
beq bne blez bgtz bltz bgez beql bnel blezl bgtzl bltzl bgezl b
j jal jalr jr
bltzal bgezal bltzall bgezall
""".split())

LINK_BRANCHES = {'bltzal': 'bltz', 'bgezal': 'bgez', 'bltzall': 'bltz', 'bgezall': 'bgez'}

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
                 for integer in ('w', 'l') for fmt in ('s', 'd'))

LOAD_OPS = {'lb', 'lbu', 'lh', 'lhu', 'lw', 'lwu', 'ld'}
LOAD_HELPERS = {1: 'recomp_load8', 2: 'recomp_load16', 4: 'recomp_load32', 8: 'recomp_load64'}
STORE_HELPERS = {1: 'recomp_store8', 2: 'recomp_store16', 4: 'recomp_store32', 8: 'recomp_store64'}

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
    if parts[-1] in ('d', 'l'):
        double_regs.append(i.rd)  # fs
        if parts[0] in FP_BINARY or parts[0] == 'c': double_regs.append(i.rt)  # ft
    if ((len(parts) == 2 and parts[-1] in ('d', 'l')) or (len(parts) == 3 and parts[1] in ('d', 'l'))):
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
    gpr_read_mask: int
    gpr_write_mask: int


def decode(raw: bytes, address: int) -> list[Instruction]:
    result = []
    for offset in range(0, len(raw), 4):
        word = struct.unpack_from(">I", raw, offset)[0]
        pc = address + offset
        instruction = rabbitizer.Instruction(word, pc)
        opcode = instruction.getOpcodeName() if instruction.isValid() else "invalid"
        imm = word & 0xffff
        operands = ((word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31)
        reads = writes = 0
        for r, read, written in zip(operands,
                                    (instruction.readsRs(), instruction.readsRt(), instruction.readsRd()),
                                    (instruction.modifiesRs(), instruction.modifiesRt(), instruction.modifiesRd())):
            if r and read:
                reads |= 1 << r
            if r and written:
                writes |= 1 << r
        # Rabbitizer describes the explicit destination, but JAL writes an
        # implicit link and merge loads also read the old destination value.
        if opcode == 'jal' or opcode in LINK_BRANCHES:
            writes |= 1 << 31
        if opcode in ('lwl', 'lwr', 'ldl', 'ldr') and operands[1]:
            reads |= 1 << operands[1]
        result.append(Instruction(pc, word, opcode, instruction.disassemble(),
                                  instruction.hasDelaySlot(), instruction.isBranchLikely(),
                                  *operands, (word >> 6) & 31, imm - 65536 if imm & 0x8000 else imm,
                                  reads, writes))
    return result


def cname(function: dict) -> str:
    return f"recomp_s{function['section']}_{function['vram']:08x}"


def target(instruction: Instruction) -> int:
    if instruction.opcode in ('j', 'jal'):
        return ((instruction.pc + 4) & 0xf0000000) | ((instruction.word & 0x03ffffff) << 2)
    return (instruction.pc + 4 + instruction.immediate * 4) & 0xffffffff


class FunctionRegistry(dict):
    """Cache unique cross-section targets for large sets of game overlays."""
    def __init__(self, functions: list[dict]):
        super().__init__(((f['section'], f['vram']), f) for f in functions)
        self.unique = {}
        for f in functions:
            address = f['vram']
            self.unique[address] = None if address in self.unique else f


def resolve_function(registry: dict, section: int, address: int) -> dict | None:
    if (section, address) in registry:
        return registry[(section, address)]
    if isinstance(registry, FunctionRegistry):
        return registry.unique.get(address)
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
    registered_calls = {site['pc']: site for site in function.get('registered_direct_calls', [])}
    for site in function.get('registered_direct_calls', []):
        instruction = next((i for i in instructions if i.pc == site['pc']), None)
        if (instruction is None or instruction.opcode != 'jal' or instruction.word != site.get('expected_word')
                or target(instruction) != site.get('target')):
            errors.append(dict(pc=site['pc'], word=site.get('expected_word', 0), assembly='',
                               reason='invalid checked registered direct call'))
    for transfer in function.get('fallthrough_transfers', []):
        i = next((i for i in instructions if i.pc == transfer['pc']), None)
        if (i is None or i.delay or i.word != transfer['expected_word'] or transfer['target'] != i.pc + 4
                or resolve_function(registry, function['section'], transfer['target']) is None):
            errors.append({'pc': transfer['pc'], 'word': transfer['expected_word'], 'assembly': '',
                           'reason': 'invalid checked function fallthrough'})
        if 'resume_pc' in transfer and transfer['resume_pc'] not in addresses:
            errors.append({'pc': transfer['pc'], 'word': transfer['expected_word'], 'assembly': '',
                           'reason': 'checked fallthrough continuation is not local'})
        if type(transfer.get('resume_register', 31)) is not int or not 1 <= transfer.get('resume_register', 31) < 32:
            errors.append({'pc': transfer['pc'], 'word': transfer['expected_word'], 'assembly': '',
                           'reason': 'invalid checked continuation register'})
    for index, i in enumerate(instructions):
        reason = None
        transfer = transfers.get(i.pc)
        registered = transfer and transfer.get('registered_targets') is True
        if i.opcode not in SUPPORTED:
            reason = f"unsupported instruction: {i.opcode}"
        elif fp_error(i, fpu_fr):
            reason = fp_error(i, fpu_fr)
        elif fpu_fr == 0 and ((i.opcode in ('ldc1', 'sdc1') and i.rt % 2)
              or (i.opcode in ('dmfc1', 'dmtc1') and i.rd % 2)):
            reason = 'FR=0 double register must be even'
        elif i.opcode == 'jalr' and i.rd != 31:
            reason = "nonstandard link register requires a local replacement"
        elif transfer and (transfer['expected_word'] != i.word
                           or transfer['kind'] not in ('jump', 'tail', 'call', 'return')
                           or (not transfer['targets'] and not registered and transfer['kind'] != 'return')
                           or (transfer['kind'] == 'return' and transfer['targets'])
                           or len(set(transfer['targets'])) != len(transfer['targets'])
                           or (registered and (transfer['kind'] != 'call' or transfer['targets']))
                           or (transfer['kind'] == 'call' and i.opcode != 'jalr')
                           or (transfer['kind'] != 'call' and (i.opcode != 'jr' or i.rs == 31))):
            reason = 'invalid checked indirect-transfer metadata'
        elif transfer and transfer['kind'] == 'return' and not (any(
                save.pc == transfer.get('save_pc') and save.pc < i.pc
                and save.word == transfer.get('expected_save_word')
                and save.opcode in ('or', 'addu') and save.rs == 31 and save.rt == 0 and save.rd == i.rs
                for save in instructions) or (
                    function.get('return_address_register') == i.rs == transfer.get('entry_register')
                    and function['vram'] == transfer.get('entry_pc')
                    and instructions[0].word == transfer.get('expected_entry_word'))):
            reason = 'invalid checked return save'
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
            elif i.opcode not in ('jr', 'jalr') and constant_condition(i) is not False:
                destination = target(i)
                local = destination in addresses and i.opcode != 'jal' and i.opcode not in LINK_BRANCHES
                if (not local and resolve_function(registry, function['section'], destination) is None
                        and not (i.opcode == 'jal' and i.pc in registered_calls)):
                    reason = f"unresolved target: {destination:#010x}"
                if local and destination % 4:
                    reason = "unaligned branch target"
        if reason:
            errors.append({"pc": i.pc, "word": i.word, "assembly": i.assembly, "reason": reason})
    return errors


def reg(number: int) -> str:
    return f"r{number}" if number else "UINT64_C(0)"


def set_reg(number: int, expression: str, effectful: bool = False) -> str:
    if number == 0:
        # Discard ALU results. Loads still perform the access (MMIO may have side effects).
        return f"(void)({expression});" if effectful else ""
    return f"r{number} = {expression};"


def gpr_set(function: dict, instructions: list[Instruction]) -> list[int]:
    # Immediate bits and FPU register fields are not integer registers. Only
    # cache actual GPR operands and the registers used by checked continuations.
    mask = 0
    for i in instructions:
        reads = i.gpr_read_mask
        if i.delay and i.opcode not in ('j', 'jal', 'jr', 'jalr') and constant_condition(i) is not None:
            reads = 0
        elif i.opcode == 'sltiu' and i.immediate == 0:
            reads = 0
        elif reads and not i.delay and not i.gpr_write_mask and not body(i):
            # ALU results discarded through $zero have no emitted operands.
            # Effectful loads still emit their access and keep the base GPR.
            reads = 0
        mask |= reads | i.gpr_write_mask
    regs = {r for r in range(1, 32) if mask & (1 << r)}
    if 'return_address_stack_offset' in function or function.get('unwind_calls'):
        regs.add(29)
    if not any(key in function for key in ('return_address_stack_offset', 'return_address_fpr')):
        regs.add(function.get('return_address_register', 31))
    if function.get('tail_call_continuations') or function.get('nonlocal_return_calls'):
        regs.add(31)
    regs.update(t.get('resume_register', 31)
                for t in function.get('indirect_transfers', []) + function.get('fallthrough_transfers', [])
                if 'resume_register' in t or 'resume_pc' in t)
    regs.discard(0)
    return sorted(regs)


def add_immediate(value: str, immediate: int | str) -> str:
    if isinstance(immediate, str):
        return f'{value} + (uint64_t)(int64_t)({immediate})'
    if immediate == 0:
        return value
    return f"{value} {'+' if immediate > 0 else '-'} {abs(immediate):#x}"


def signed_constant(value: int, bits: int) -> str:
    value &= (1 << bits) - 1
    if value & (1 << (bits - 1)):
        value -= 1 << bits
    return f"UINT64_C(0x{value & 0xffffffffffffffff:x})"


def memory_address(i: Instruction, immediate: str | None = None) -> str:
    if immediate is not None:
        return add_immediate(reg(i.rs), immediate)
    if i.rs == 0:
        return f"UINT32_C(0x{i.immediate & 0xffffffff:x})"
    return add_immediate(reg(i.rs), i.immediate)


def body(i: Instruction, relocated_immediate: str | None = None) -> str:
    op, a, b = i.opcode, reg(i.rs), reg(i.rt)
    imm = relocated_immediate or f"{i.immediate:#x}"
    logical_imm = f'((uint16_t)({imm}))' if relocated_immediate else f"UINT64_C(0x{i.word & 0xffff:04x})"
    destination = i.rt
    expression = None
    if op == 'nop': return ""
    if op == 'break': return f'recomp_fail("MIPS BREAK {i.word >> 6 & 0xfffff}", UINT32_C(0x{i.pc:08x}));'
    if op == 'syscall': return f'recomp_fail("MIPS SYSCALL {i.word >> 6 & 0xfffff}", UINT32_C(0x{i.pc:08x}));'
    if op == 'mtc1': return f'ctx->fpr[{i.rd}] = {b};'
    if op == 'mfc1': return set_reg(i.rt, f'mips_s32(ctx->fpr[{i.rd}])')
    if op == 'cfc1': return set_reg(i.rt, 'mips_s32(ctx->fcr31)')
    if op == 'ctc1': return f'mips_fp_set_control(ctx, {b});'
    parts = op.split('.')
    if op == 'dmtc1': return f'mips_set_fpr_pair(ctx, {i.rd}, {b});'
    if op == 'dmfc1': return set_reg(i.rt, f'mips_fpr_pair(ctx, {i.rd})')
    # MOV preserves raw bits and FCR31; double moves retain the runtime FR layout.
    if op == 'mov.s': return f'ctx->fpr[{i.shift}] = ctx->fpr[{i.rd}];'
    if op == 'mov.d': return f'mips_set_fpr_pair(ctx, {i.shift}, mips_fpr_pair(ctx, {i.rd}));'
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
        address = memory_address(i, relocated_immediate)
        if op == 'lwc1': return f'ctx->fpr[{i.rt}] = {LOAD_HELPERS[4]}(runtime, {address});'
        if op == 'swc1': return f'{STORE_HELPERS[4]}(runtime, {address}, ctx->fpr[{i.rt}]);'
        if op == 'ldc1': return f'mips_set_fpr_pair(ctx, {i.rt}, {LOAD_HELPERS[8]}(runtime, {address}));'
        return f'{STORE_HELPERS[8]}(runtime, {address}, mips_fpr_pair(ctx, {i.rt}));'
    if op in ('lwl', 'lwr', 'ldl', 'ldr', 'swl', 'swr', 'sdl', 'sdr'):
        address = memory_address(i, relocated_immediate)
        width = 4 if op[1] == 'w' else 8
        left = 'true' if op.endswith('l') else 'false'
        if op.startswith('s'):
            return f'recomp_store_merge(runtime, {address}, {b}, {width}, {left});'
        expression = f'recomp_load_merge(runtime, {address}, {b}, {width}, {left})'
        if width == 4: expression = f'mips_s32({expression})'
        return set_reg(i.rt, expression, effectful=True)
    if op == 'lui':
        expression = (f'mips_s32((uint32_t){logical_imm} << 16)' if relocated_immediate
                      else signed_constant((i.word & 0xffff) << 16, 32))
    elif op in ('addiu', 'addi', 'daddiu'):
        expression = (f"mips_s32({add_immediate(a, relocated_immediate or i.immediate)})" if op == 'addiu'
                      else f"mips_add32({a}, {imm})" if op == 'addi' else add_immediate(a, relocated_immediate or i.immediate))
    elif op in ('addu', 'add', 'subu', 'sub', 'dadd', 'daddu', 'dsubu'):
        destination = i.rd
        sign = '+' if op in ('addu', 'add', 'dadd', 'daddu') else '-'
        # Unsigned 64-bit arithmetic followed by narrowing has the same low
        # 32 bits as narrowing both operands before the wrapping operation.
        expression = (f"mips_s32({a} {sign} {b})" if op in ('addu', 'subu')
                      else f"mips_{'add' if op == 'add' else 'sub'}32({a}, {b})" if op in ('add', 'sub')
                      else f"mips_add64({a}, {b})" if op == 'dadd'
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
        address = memory_address(i, relocated_immediate)
        if op.startswith('s'): return f"{STORE_HELPERS[width]}(runtime, {address}, {b});"
        expression = f"{LOAD_HELPERS[width]}(runtime, {address})"
        if op in ('lb', 'lh', 'lw'): expression = f"mips_s{width * 8}({expression})"
    if expression is None:
        raise RecompError(f"no straight-line emitter for {op}")
    return set_reg(destination, expression, effectful=op in LOAD_OPS or op in ('add', 'addi', 'sub', 'dadd'))


def constant_condition(i: Instruction) -> bool | None:
    """Resolve only outcomes proven by register identity or the zero register."""
    op = LINK_BRANCHES.get(i.opcode, i.opcode.rstrip('l'))
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
    op, a, b = LINK_BRANCHES.get(i.opcode, i.opcode.rstrip('l')), reg(i.rs), reg(i.rt)
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


@dataclass(frozen=True)
class DirectCallTarget:
    section: int
    symbol: str
    registry_index: int


def direct_call_targets(registry: dict, hooks: dict[tuple[int, int], str],
                        selected: list[dict] | None = None) -> dict[int, DirectCallTarget]:
    # A shared guest address must retain runtime loaded-section resolution.
    counts = Counter(address for _, address in registry)
    indices = {(f['section'], f['vram']): index
               for index, f in enumerate(selected if selected is not None else registry.values())}
    return {address: DirectCallTarget(section, hooks.get((section, address), cname(function)),
                                      indices[(section, address)])
            for (section, address), function in registry.items()
            if counts[address] == 1 and (section, address) in indices}


def emit_function(function: dict, instructions: list[Instruction], registry: dict,
                  yield_sites: set[int] | None = None,
                  hooks: dict[tuple[int, int], str] | None = None,
                  direct_targets: dict[int, DirectCallTarget] | None = None) -> str:
    log_emission('Emit %s s%s 0x%08x -> %s (%d instructions)',
                 function['name'], function['section'], function['vram'], cname(function), len(instructions),
                 verbose=True)
    safe_name = function['name'].replace('*/', '* /').replace('\n', ' ')
    gprs = gpr_set(function, instructions)
    written_mask = 0
    for instruction in instructions:
        written_mask |= instruction.gpr_write_mask
    written_gprs = [r for r in gprs if written_mask & (1 << r)]

    def flush(indent: str) -> list[str]:
        # Read-only locals already match ctx. Native calls/checkpoints reload
        # every cached register afterward, including registers they may change.
        return [f'{indent}ctx->gpr[{n}] = r{n};' for n in written_gprs]

    def reload(indent: str) -> list[str]:
        return [f'{indent}r{n} = ctx->gpr[{n}];' for n in gprs]

    def around(statements: list[str], indent: str, resume: bool = True) -> list[str]:
        # Anything here may run native code that reads or writes ctx.
        return [*flush(indent), *statements, *(reload(indent) if resume else [])]

    if 'return_address_stack_offset' in function:
        ra_line = (f"    uint32_t return_address = recomp_load32(runtime, "
                   f"(uint32_t)r29 + UINT32_C(0x{function['return_address_stack_offset']:x}));")
    elif 'return_address_fpr' in function:
        ra_line = f"    uint32_t return_address = ctx->fpr[{function['return_address_fpr']}];"
    else:
        ra_line = f"    uint32_t return_address = (uint32_t)r{function.get('return_address_register', 31)};"

    lines = [f"/* {safe_name} at 0x{function['vram']:08x} */",
             f"void {cname(function)}(Runtime *runtime, MipsContext *ctx) {{",
             *(['    uint64_t ' + ', '.join(f'r{n} = ctx->gpr[{n}]' for n in gprs) + ';'] if gprs else []),
             ra_line,
             "    (void)return_address; (void)runtime; (void)ctx;"]
    if function.get('unwind_calls'):
        lines.append('    uint32_t entry_stack_pointer = (uint32_t)r29;')

    addresses = instruction_addresses(function)
    transfers = {t['pc']: t for t in function.get('indirect_transfers', [])}
    fallthrough_transfers = {t['pc']: t for t in function.get('fallthrough_transfers', [])}
    used_labels = set()
    yield_sites = yield_sites or set()
    hooks = hooks or {}
    if direct_targets is None:
        direct_targets = direct_call_targets(registry, hooks)

    def location(pc: int, indent: str = '    ') -> str:
        return f"{indent}RECOMP_TRACE(runtime, ctx, UINT32_C(0x{pc:08x}));"

    def emit_body(instruction: Instruction, indent: str = '    ') -> list[str]:
        relocated = instruction.pc in function.get('relocation_pcs', [])
        immediate = (f'(int16_t)recomp_load32(runtime, recomp_relocated_address(runtime, '
                     f'{function["section"]}, UINT32_C(0x{instruction.pc:08x})))') if relocated else None
        statement = body(instruction, immediate)
        if LOG_EMISSION and LOG_EMISSION_VERBOSE and ('recomp_load' in statement or 'recomp_store' in statement):
            log_emission('Memory %s s%s 0x%08x: %s -> %s',
                         function['name'], function['section'], instruction.pc, instruction.assembly, statement,
                         verbose=True)
        return [f'{indent}/* {instruction.pc:08x}: {instruction.assembly} */',
                location(instruction.pc, indent), *([indent + statement] if statement else [])]

    def fallthrough(address: int, indent: str = '    ') -> str:
        return f'{indent}recomp_fail("fell off generated function", UINT32_C(0x{address:08x}));'

    def jump(address: int, indent: str = '    ') -> str:
        used_labels.add(f'L_{address:08x}:')
        return f'{indent}goto L_{address:08x};'

    def checked_fallthrough(checked: dict, indent: str) -> list[str]:
        code = emit_call(checked['target'], indent, resume='resume_pc' in checked)
        if 'resume_pc' in checked:
            resume, link = checked['resume_pc'], checked.get('resume_register', 31)
            code.extend([f'{indent}if ((uint32_t)r{link} != UINT32_C(0x{resume:08x}))',
                         f'{indent}    recomp_fail("checked continuation return mismatch", (uint32_t)r{link});',
                         jump(resume, indent)])
        else:
            code.append(indent + 'return;')
        return code

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
                     'known function entry' if address in direct_targets else 'via recomp_lookup', verbose=True)

    def emit_call(address: int, indent: str = '        ', resume: bool = True) -> list[str]:
        # Resuming callers reload locals: the callee may have changed any GPR.
        direct = direct_targets.get(address)
        if direct is not None:
            if (direct.section, address) in hooks:
                call = [f"{indent}recomp_prepare_direct_call(runtime, UINT32_C(0x{address:08x}), {direct.section});",
                        f"{indent}{direct.symbol}(runtime, ctx);"]
            else:
                call = [f"{indent}recomp_call_generated(runtime, ctx, &recomp_functions[{direct.registry_index}]);"]
        else:
            call = [f"{indent}recomp_lookup(runtime, UINT32_C(0x{address:08x}))(runtime, ctx);"]
        return around(call, indent, resume)

    def transfer(address: int, indent: str = '        ') -> list[str]:
        continuation = next((c for c in function.get('tail_call_continuations', []) if c['pc'] == i.pc), None)
        if continuation:
            resume = continuation['resume_pc']
            return [*emit_call(address, indent),
                    f'{indent}if ((uint32_t)r31 != UINT32_C(0x{resume:08x}))',
                    f'{indent}    recomp_fail("checked continuation return mismatch", (uint32_t)r31);',
                    jump(resume, indent)]
        if address in addresses:
            checkpoint = []
            if address <= i.pc:
                force_yield = 'true' if i.pc in yield_sites else 'false'
                log_emission('Checkpoint %s s%s 0x%08x -> 0x%08x: force_yield=%s',
                             function['name'], function['section'], i.pc, address, force_yield, verbose=True)
                # The decision never observes ctx or yields. Only the action
                # needs synchronized registers, including reloads after resuming.
                nested = indent + '    '
                checkpoint = [f'{indent}if (recomp_checkpoint_due(runtime, {force_yield})) {{',
                              *around([f'{nested}runtime->checkpoint(runtime);'], nested),
                              f'{indent}}}']
            return [*checkpoint, jump(address, indent)]
        log_call(address, 'Tail/branch call')
        return [location(i.pc, indent), *emit_call(address, indent, resume=False), f"{indent}return;"]

    prologue = lines
    blocks = []
    delay_slots = {i.pc + 4 for i in instructions if i.delay}
    for index, i in enumerate(instructions):
        lines = [f"L_{i.pc:08x}:"]
        blocks.append((i.pc, lines))
        if not i.delay:
            lines.extend(emit_body(i))
            checked = fallthrough_transfers.get(i.pc)
            if checked:
                lines.extend(checked_fallthrough(checked, '    '))
            elif i.pc + 4 in addresses:
                if index + 1 == len(instructions) or instructions[index + 1].pc != i.pc + 4:
                    lines.append(jump(i.pc + 4))
            else:
                lines.append(fallthrough(i.pc))
            continue

        slot = instructions[index + 1]
        lines.append(f"    /* {i.pc:08x}: {i.assembly} */")
        lines.append(location(i.pc))
        lines.append('    {')
        if i.opcode in LINK_BRANCHES:
            known = constant_condition(i)
            # VR4300 links unconditionally, even on a not-taken likely branch.
            # Capture the predicate before writing r31 (rs may itself be r31).
            if known is None:
                lines.append(f'        bool take_branch = {condition(i)};')
            ra = (f'mips_s32(recomp_relocated_address(runtime, {function["section"]}, UINT32_C(0x{i.pc+8:08x})))'
                  if function.get('relocatable') else signed_constant(i.pc + 8, 32))
            lines.append('        ' + set_reg(31, ra))
            if not i.likely:
                lines.extend(emit_body(slot, '        '))
            if known is not False:
                indent = '        ' if known is True else '            '
                if known is None:
                    lines.append('        if (take_branch) {')
                if i.likely:
                    lines.extend(emit_body(slot, indent))
                lines.extend([location(i.pc, indent), *emit_call(target(i), indent)])
                if known is None:
                    lines.append('        }')
            lines.append(jump(i.pc + 8, '        ') if i.pc + 8 in addresses
                         else fallthrough(i.pc, '        '))
            lines.append('    }')
            continue
        if i.opcode not in ('j', 'jal', 'jr', 'jalr'):
            known = constant_condition(i)
            if known is True:
                lines.extend(emit_body(slot, '        '))
                lines.extend(transfer(target(i)))
                lines.append('    }')
                continue
            if known is None:
                lines.append(f"        if ({condition(i)}) {{")
                lines.extend(emit_body(slot, '            '))
                lines.extend(transfer(target(i), '            '))
                lines.append('        }')
            if not i.likely:
                lines.extend(emit_body(slot, '        '))
            checked = fallthrough_transfers.get(slot.pc)
            if checked:
                lines.extend(checked_fallthrough(checked, '        '))
            else:
                lines.append(jump(i.pc + 8, '        ') if i.pc + 8 in addresses
                             else fallthrough(i.pc, '        '))
            lines.append('    }')
            continue

        if i.opcode in ('jr', 'jalr'):
            # Read the target before the delay slot can change the source register.
            lines.append(f"        uint32_t destination = (uint32_t){reg(i.rs)};")
        if i.opcode in ('jal', 'jalr'):
            ra = (f'mips_s32(recomp_relocated_address(runtime, {function["section"]}, UINT32_C(0x{i.pc+8:08x})))'
                  if function.get('relocatable') else signed_constant(i.pc + 8, 32))
            lines.append('        ' + set_reg(31 if i.opcode == 'jal' else i.rd, ra))
        lines.extend(emit_body(slot, '        '))
        checked = transfers.get(i.pc)

        if i.opcode == 'jr' and checked and checked['kind'] == 'return':
            lines.extend(flush('        '))
            lines.extend(['        if (destination != return_address)',
                          '            recomp_fail("checked return target mismatch", destination);',
                          '        return;'])
        elif i.opcode == 'jr' and checked:
            destination = ('recomp_canonical_function_address(runtime, destination)'
                           if function.get('relocatable') else 'destination')
            lines.append(f'        switch ({destination}) {{')
            for address in checked['targets']:
                lines.append(f'        case UINT32_C(0x{address:08x}):')
                if checked['kind'] == 'jump':
                    lines.extend(transfer(address, '            '))
                else:
                    log_call(address, 'Checked indirect tail target')
                    lines.extend([location(i.pc, '            '),
                                  *emit_call(address, '            ', resume=False), '            return;'])
            lines.append('        default:')
            lines.extend(flush('            '))
            lines.extend(['            recomp_fail("unlisted indirect jump target", destination);', '        }'])
        elif i.opcode == 'jr':
            if i.rs != 31:
                log_emission('Indirect tail %s s%s 0x%08x: %s -> recomp_lookup(runtime, destination)',
                             function['name'], function['section'], i.pc, i.assembly, verbose=True)
            # Flush before the branch: both the lookup-tail and the return path leave the function.
            lines.extend(flush('        '))
            lines.extend(['        if (destination != return_address) {',
                          location(i.pc, '            '),
                          '            recomp_lookup(runtime, destination)(runtime, ctx);', '        }', '        return;'])
        elif i.opcode in ('jal', 'jalr'):
            dest = 'destination' if i.opcode == 'jalr' else f"UINT32_C(0x{target(i):08x})"
            if checked and not checked.get('registered_targets'):
                destination = ('recomp_canonical_function_address(runtime, destination)'
                               if function.get('relocatable') else 'destination')
                lines.append(f'        switch ({destination}) {{')
                for address in checked['targets']:
                    log_call(address, 'Checked indirect call target')
                    lines.extend([f'        case UINT32_C(0x{address:08x}):', location(i.pc, '            '),
                                  *emit_call(address, '            '), '            break;'])
                lines.append('        default:')
                lines.extend(flush('            '))
                lines.extend(['            recomp_fail("unlisted indirect call target", destination);', '        }'])
            else:
                if i.opcode == 'jalr':
                    log_emission('Indirect call %s s%s 0x%08x: %s -> recomp_lookup(runtime, destination)',
                                 function['name'], function['section'], i.pc, i.assembly, verbose=True)
                    call = around([f"        recomp_lookup(runtime, {dest})(runtime, ctx);"], '        ')
                else:
                    log_call(target(i), 'Direct call')
                    call = emit_call(target(i))
                lines.extend([location(i.pc, '        '), *call])
            unwind = next((c for c in function.get('unwind_calls', []) if c['pc'] == i.pc), None)
            if unwind:
                if unwind.get('conditional'):
                    lines.append('        if ((uint32_t)r29 > entry_stack_pointer) return;')
                else:
                    lines.extend(['        if ((uint32_t)r29 <= entry_stack_pointer)',
                                  '            recomp_fail("checked unwind did not restore outer stack", (uint32_t)r29);',
                                  '        return;'])
            if i.pc in function.get('nonlocal_return_calls', []):
                lines.append('        if ((uint32_t)r31 == return_address) return;')
            lines.append(jump(i.pc + 8, '        ') if i.pc + 8 in addresses
                         else fallthrough(i.pc, '        '))
        elif i.opcode == 'j':
            lines.extend(transfer(target(i)))
        lines.append('    }')

    lines = prologue
    for pc, block in blocks:
        # Transfers already execute their slot inline and leave via return/goto.
        # Keep a standalone copy only when another path explicitly enters it.
        if pc not in delay_slots or f'L_{pc:08x}:' in used_labels:
            lines.extend(block)
    if not instructions:
        lines.append(fallthrough(function['vram']))
    lines.append('}')
    # Only control-flow destinations need labels.
    return '\n'.join(line for line in lines if not line.startswith('L_') or line in used_labels)
