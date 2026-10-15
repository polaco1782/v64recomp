"""Bounded, read-only inspection of SM64 F3D lists in captured word-swapped RDRAM.

This validates list control flow and vertex/matrix references. It neither rasterizes
nor implements clipping, lighting, texture sampling or conditional culling.
"""
from __future__ import annotations

from collections import Counter
import hashlib

from common import RecompError


def inspect_f3d(ram: bytes, descriptor: list[int], command_limit: int = 100000) -> dict:
    if (len(descriptor) != 16 or any(type(w) is not int or not 0 <= w <= 0xffffffff for w in descriptor)
            or descriptor[0] != 1 or descriptor[4] != 0x8032b330
            or descriptor[5] != 0x1000 or descriptor[6] != 0x80339ac0
            or descriptor[7] != 0x800 or len(ram) % 4 or command_limit <= 0):
        raise RecompError('inspection requires the SM64 F3D descriptor and word-swapped RDRAM')
    segments = [0] * 16
    populated = [False] * 16
    matrices = []
    stats = Counter()
    calls = Counter()
    geometry = hashlib.sha256()

    def region(address: int, size: int, alignment: int = 1) -> int:
        if address < 0 or size < 0 or address % alignment or address + size > len(ram):
            raise RecompError(f'F3D reference outside/aligned incorrectly in RDRAM: {address:#x}+{size:#x}')
        return address

    def word(address: int) -> int:
        region(address, 4, 4)
        return int.from_bytes(ram[address:address+4], 'little')

    def canonical(address: int, size: int) -> bytes:
        region(address, size)
        return bytes(ram[(address+n) ^ 3] for n in range(size))

    def resolve(address: int) -> int:
        return segments[(address >> 24) & 15] + (address & 0xffffff)

    root = descriptor[12] & 0x1fffffff
    size = descriptor[13]
    if not size or size % 8:
        raise RecompError('invalid F3D root list size')
    region(root, size, 8)
    # Only the root has an explicit byte count. Nested lists end at ENDDL;
    # command/depth limits bound malformed branches and recursion.
    pc, root_end, stack = root, root + size, []
    state_ops = {0x00, 0xb2, 0xb3, 0xb4, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbd,
                 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec,
                 0xed, 0xee, 0xef, 0xf0, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6,
                 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff}
    while stats['commands'] < command_limit:
        if root_end is not None and pc + 8 > root_end:
            raise RecompError('F3D root list exhausted without ENDDL')
        region(pc, 8, 8)
        a, b = word(pc), word(pc+4)
        pc += 8
        op = a >> 24
        stats['commands'] += 1
        if op == 0xbc:
            index, offset = a & 255, (a >> 8) & 65535
            if index == 6:
                if offset % 4 or offset >= 64:
                    raise RecompError('invalid F3D segment update')
                segments[offset//4] = b & 0xffffff
        elif op == 6:
            parameter = (a >> 16) & 255
            if parameter not in (0, 1) or a & 65535:
                raise RecompError('invalid F3D display-list call/branch')
            address = region(resolve(b), 8, 8)
            calls[f'0x{b:08x}'] += 1
            stats['display_list_calls'] += 1
            if not parameter:
                if len(stack) >= 32:
                    raise RecompError('F3D call depth exceeded')
                stack.append((pc, root_end))
            pc, root_end = address, None
        elif op == 0xb8:
            if stack:
                pc, root_end = stack.pop()
            else:
                return {**stats, 'referenced_lists': dict(calls), 'matrices': matrices,
                        'geometry_sha256': geometry.hexdigest()}
        elif op in (1, 3):
            size = a & 65535
            address = region(resolve(b), size, 8)
            if op == 1:
                if size != 64:
                    raise RecompError('invalid F3D matrix DMA size')
                data = canonical(address, size)
                diagonal = []
                for n in (0, 5, 10, 15):
                    value = int.from_bytes(data[n*2:n*2+2], 'big', signed=True)
                    value += int.from_bytes(data[32+n*2:34+n*2], 'big') / 65536
                    diagonal.append(value)
                matrices.append({'address': address, 'flags': (a >> 16) & 255,
                                 'sha256': hashlib.sha256(data).hexdigest(), 'diagonal': diagonal})
                stats['matrix_loads'] += 1
            else:
                stats['movemem_loads'] += 1
        elif op == 4:
            parameter = (a >> 16) & 255
            count, first = (parameter >> 4) + 1, parameter & 15
            if first + count > 16 or a & 65535 != count * 16:
                raise RecompError('invalid original-F3D vertex DMA')
            address = region(resolve(b), count * 16, 8)
            geometry.update(canonical(address, count * 16))
            populated[first:first+count] = [True] * count
            stats['vertex_loads'] += 1
            stats['vertices_loaded'] += count
        elif op == 0xbf:
            indices = [(b >> shift) & 255 for shift in (16, 8, 0)]
            if any(i % 10 or i//10 >= 16 or not populated[i//10] for i in indices):
                raise RecompError('F3D triangle references an unloaded/invalid vertex')
            geometry.update(b.to_bytes(4, 'big'))
            stats['triangles'] += 1
        elif op not in state_ops:
            raise RecompError(f'F3D inspection does not support opcode {op:#x}')
    raise RecompError('F3D command budget exceeded')
