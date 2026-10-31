"""Decode the pinned Media Player ELF and its embedded libdragon SYMT v3.

Format reference: DragonMinded/libdragon bb89f105e5b636dda0610885595511f4313fa0e0,
boot/loader.c and src/symtable.c. No external checkout is a build input.
"""
import struct

from common import RecompError


def require(ok, message):
    if not ok:
        raise RecompError('Media Player: ' + message)


def lz4_decode(source: bytes, limit: int) -> bytes:
    out = bytearray()
    pos = 0

    def length(n):
        nonlocal pos
        if n == 15:
            while True:
                require(pos < len(source), 'truncated LZ4 length')
                b = source[pos]
                pos += 1
                n += b
                require(n <= limit, 'oversized LZ4 length')
                if b != 255:
                    break
        return n

    while pos < len(source):
        token = source[pos]
        pos += 1
        n = length(token >> 4)
        require(pos + n <= len(source) and len(out) + n <= limit, 'invalid LZ4 literal')
        out.extend(source[pos:pos+n])
        pos += n
        if pos == len(source):
            break
        require(pos + 2 <= len(source), 'truncated LZ4 offset')
        distance = int.from_bytes(source[pos:pos+2], 'little')
        pos += 2
        n = length(token & 15) + 4
        require(0 < distance <= len(out) and len(out) + n <= limit, 'invalid LZ4 match')
        for _ in range(n):
            out.append(out[-distance])
    return bytes(out)


def read_symbols(data: bytes) -> list[dict]:
    require(len(data) >= 80 and data[:4] == b'SYMT', 'missing symbol table')
    h = struct.unpack_from('>19I', data, 4)
    (version, count, chunks, chunk_off, file_tab, func_tab, huff_off,
     file_blob, func_blob, stream_off, files, funcs, file_blocks, func_blocks,
     huff_size, file_size, func_size, stream_size, _) = h
    require(version == 3 and stream_off + stream_size == len(data), 'unsupported SYMT layout')
    table = data[huff_off:huff_off+huff_size]
    max_bits = table[0]
    require(6 <= max_bits <= 16, 'invalid Huffman maximum')
    nlong = max_bits - 6
    first_code = struct.unpack_from('>' + 'H'*nlong, table, 130)
    first_symbol = struct.unpack_from('>' + 'H'*(nlong+1), table, 130+2*nlong)
    alphabet = table[132+4*nlong:]
    codes = {}
    for i in range(64):
        symbol, bits = table[2+2*i:4+2*i]
        if bits:
            codes[bits, i >> (6-bits)] = symbol
    for i in range(nlong):
        for j in range(first_symbol[i], first_symbol[i+1]):
            codes[i+7, first_code[i] + j-first_symbol[i]] = alphabet[j]

    class Bits:
        def __init__(self, blob):
            self.blob, self.pos = blob, 0

        def bit(self):
            require(self.pos < len(self.blob)*8, 'truncated Huffman stream')
            v = (self.blob[self.pos//8] >> (7-self.pos%8)) & 1
            self.pos += 1
            return v

        def prefix_delta(self):
            zeros = 0
            while not self.bit():
                zeros += 1
                require(zeros <= 16, 'invalid string prefix')
            value = 1
            for _ in range(zeros):
                value = value*2 + self.bit()
            value -= 1
            return (value >> 1) ^ -(value & 1)

        def character(self):
            code = 0
            for bits in range(1, max_bits+1):
                code = code*2 + self.bit()
                if (bits, code) in codes:
                    return codes[bits, code]
            raise RecompError('Media Player: invalid Huffman code')

    def strings(tab, blob, size, nblocks, total):
        indexes = [struct.unpack_from('>II', data, tab+8*i) for i in range(nblocks)]
        indexes.append((total, size))
        result = []
        for (start, offset), (end, next_offset) in zip(indexes, indexes[1:]):
            require(start == len(result) and offset < next_offset <= size, 'invalid string block')
            bits = Bits(data[blob+offset:blob+next_offset])
            previous, prefix = b'', 0
            for _ in range(start, end):
                prefix += bits.prefix_delta()
                require(0 <= prefix <= len(previous), 'invalid front-coded prefix')
                current = bytearray(previous[:prefix])
                while (ch := bits.character()):
                    current.append(ch)
                    require(len(current) <= 4096, 'oversized symbol name')
                previous = bytes(current)
                result.append(previous.decode('utf-8'))
        return result

    filenames = strings(file_tab, file_blob, file_size, file_blocks, files)
    names = strings(func_tab, func_blob, func_size, func_blocks, funcs)
    result = []
    seen = 0
    for i in range(chunks):
        addr, offset = struct.unpack_from('>II', data, chunk_off+8*i)
        pos = stream_off + offset

        def varint():
            nonlocal pos
            value, shift = 0, 0
            while True:
                require(pos < len(data) and shift < 64, 'invalid symbol varint')
                b = data[pos]
                pos += 1
                value |= (b & 127) << shift
                if not b & 128:
                    return value
                shift += 7

        def signed():
            v = varint()
            return (v >> 1) ^ -(v & 1)

        varint()  # First symbol's offset within its containing function.
        file_id = func_id = line = 0
        while True:
            require(pos < len(data), 'truncated symbol chunk')
            op = data[pos]
            pos += 1
            if op == 0x18:
                break
            file_id += signed() if op & 128 else 0
            func_id += signed() if op & 64 else 0
            line += signed() if op & 32 else 0
            addr += ((varint()+7) if op & 7 == 7 else op & 7)*4
            seen += 1
            require(0 <= func_id < len(names) and 0 <= file_id < len(filenames), 'invalid symbol index')
            if op & 16:
                require(not op & 8 and addr % 4 == 0, 'invalid function symbol')
                result.append(dict(vram=addr, name=names[func_id], file=filenames[file_id], line=line))
    require(seen == count, 'symbol count mismatch')
    return result
