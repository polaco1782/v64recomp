"""Bounded decoder for Hexen's ROM LZH stream (original decoder at 8004d078).

The adaptive tree has 382 symbols and a 4096-byte dictionary, with matches of
3..128 bytes. Unlike conventional LZHUF streams, bits arrive least significant
first. Position prefix tables are read from the pinned ROM, not copied assets.
This prepares static analysis; the executable runs the original guest decoder.
"""
from common import RecompError


def decode_lzh(data: bytes, size: int, codes: bytes, lengths: bytes) -> bytes:
    if not 0 < size <= 0x400000 or len(codes) != 256 or len(lengths) != 256:
        raise RecompError('invalid Hexen LZH bounds/tables')
    leaves, nodes, root = 382, 763, 762
    freq = [1] * leaves + [0] * (nodes - leaves) + [65535]
    son = [i + nodes for i in range(leaves)] + [0] * (nodes - leaves)
    parent = [0] * (nodes + leaves)
    for i in range(leaves):
        parent[i + nodes] = i
    child = 0
    for i in range(leaves, nodes):
        freq[i] = freq[child] + freq[child + 1]
        son[i] = child
        parent[child] = parent[child + 1] = i
        child += 2
    parent[root] = 0
    bitpos = 0

    def bits(count):
        nonlocal bitpos
        if bitpos + count > len(data) * 8:
            raise RecompError('truncated Hexen LZH stream')
        value = 0
        for i in range(count):
            value |= ((data[bitpos >> 3] >> (bitpos & 7)) & 1) << i
            bitpos += 1
        return value

    def rebuild():
        # Preserve sorted leaves, halve frequencies, then merge the tree again.
        j = 0
        for i in range(nodes):
            if son[i] >= nodes:
                freq[j] = (freq[i] + 1) // 2
                son[j] = son[i]
                j += 1
        child = 0
        for j in range(leaves, nodes):
            value = freq[child] + freq[child + 1]
            k = j - 1
            while value < freq[k]:
                k -= 1
            k += 1
            freq[k + 1:j + 1] = freq[k:j]
            son[k + 1:j + 1] = son[k:j]
            freq[k], son[k] = value, child
            child += 2
        for i in range(nodes):
            child = son[i]
            parent[child] = i
            if child < nodes:
                parent[child + 1] = i

    def update(symbol):
        if freq[root] == 0x8000:
            rebuild()
        c = parent[symbol + nodes]
        while True:
            freq[c] += 1
            value = freq[c]
            if value > freq[c + 1]:
                other = c + 1
                while value > freq[other + 1]:
                    other += 1
                freq[c], freq[other] = freq[other], value
                a, b = son[c], son[other]
                parent[a] = other
                if a < nodes:
                    parent[a + 1] = other
                parent[b] = c
                if b < nodes:
                    parent[b + 1] = c
                son[c], son[other] = b, a
                c = other
            c = parent[c]
            if c == 0:
                break

    ring = bytearray(4096)
    ring[:4096 - 128] = b' ' * (4096 - 128)
    cursor = 4096 - 128
    output = bytearray()
    while len(output) < size:
        c = son[root]
        while c < nodes:
            c = son[c + bits(1)]
        c -= nodes
        update(c)
        if c < 256:
            raw = bytes([c])
        else:
            prefix = bits(8)
            count = lengths[prefix] - 2
            if not 0 <= count <= 6:
                raise RecompError('invalid Hexen LZH position prefix')
            position = (codes[prefix] << 6) | (((prefix << count) + bits(count)) & 63)
            start = (cursor - position - 1) & 4095
            count = c - 253
            if len(output) + count > size:
                raise RecompError('Hexen LZH match exceeds decoded size')
            # Ring updates must be visible within an overlapping match.
            for i in range(count):
                value = ring[(start + i) & 4095]
                output.append(value)
                ring[cursor] = value
                cursor = (cursor + 1) & 4095
            continue
        output.extend(raw)
        ring[cursor] = c
        cursor = (cursor + 1) & 4095
    return bytes(output)
