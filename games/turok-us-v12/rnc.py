"""Bounded RNC method 1 image preparation (LSB Huffman/LZ stream)."""
from common import RecompError


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xa001 if crc & 1 else 0)
    return crc


def decode_rnc1(data: bytes) -> bytes:
    if len(data) < 18 or data[:4] != b'RNC\x01':
        raise RecompError('expected RNC method 1 header')
    size, packed = (int.from_bytes(data[p:p+4], 'big') for p in (4, 8))
    if not 0 < size <= 0x800000 or 18 + packed > len(data):
        raise RecompError('RNC image size outside bounds')
    source = data[18:18+packed]
    if crc16(source) != int.from_bytes(data[14:16], 'big'):
        raise RecompError('RNC packed CRC mismatch')
    pos, buffer, left = 0, 0, 0
    out = bytearray()

    def bits(count):
        nonlocal pos, buffer, left
        value = 0
        for index in range(count):
            if not left:
                if pos + 2 > len(source):
                    raise RecompError('RNC truncated bit stream')
                buffer = int.from_bytes(source[pos:pos+4], 'little')
                pos += 2
                left = 16
            value |= (buffer & 1) << index
            buffer >>= 1
            left -= 1
        return value

    def table():
        count = bits(5)
        if count > 16:
            raise RecompError('RNC Huffman table too large')
        lengths = [bits(4) for _ in range(count)]
        code, result = 0, []
        for width in range(1, 16):
            for symbol, length in enumerate(lengths):
                if length == width:
                    reverse = int(f'{code:016b}'[::-1], 2)
                    result.append((width, reverse, symbol))
                    code += 1 << (16-width)
        return result

    def value(table):
        # buffer includes the next word as lookahead; literal runs refresh it.
        for width, code, symbol in table:
            if buffer & ((1 << width)-1) == code:
                bits(width)
                return symbol if symbol < 2 else (1 << (symbol-1)) | bits(symbol-1)
        raise RecompError('invalid RNC Huffman code')

    if bits(2):
        raise RecompError('protected RNC streams unsupported')
    for _ in range(data[17]):
        literals, offsets, lengths = table(), table(), table()
        chunks = bits(16)
        if not chunks:
            raise RecompError('empty RNC chunk')
        for index in range(chunks):
            count = value(literals)
            if pos + count > len(source) or len(out) + count > size:
                raise RecompError('RNC literal outside image')
            if count:
                out.extend(source[pos:pos+count])
                pos += count
                buffer = (buffer & ((1 << left)-1)) | (int.from_bytes(source[pos:pos+3], 'little') << left)
            if index + 1 < chunks:
                distance, count = value(offsets)+1, value(lengths)+2
                if distance > len(out) or len(out)+count > size:
                    raise RecompError('RNC match outside image')
                for _ in range(count):
                    out.append(out[-distance])
    if len(out) != size or crc16(out) != int.from_bytes(data[12:14], 'big'):
        raise RecompError('RNC decoded size/CRC mismatch')
    return bytes(out)
