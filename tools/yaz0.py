"""Decode the N64 Yaz0 streams used by the Zelda boot loaders."""
from common import RecompError


def yaz0(data: bytes, expected: int) -> bytes:
    if len(data) < 16 or data[:4] != b'Yaz0' or int.from_bytes(data[4:8], 'big') != expected:
        raise RecompError('invalid Yaz0 header')

    output = bytearray()
    pos = 16
    try:
        while len(output) < expected:
            flags = data[pos]
            pos += 1
            for bit in range(7, -1, -1):
                if len(output) == expected:
                    break
                if flags & (1 << bit):
                    output.append(data[pos])
                    pos += 1
                    continue

                first, second = data[pos:pos + 2]
                pos += 2
                distance = ((first & 15) << 8 | second) + 1
                length = first >> 4
                if length:
                    length += 2
                else:
                    length = data[pos] + 0x12
                    pos += 1
                if distance > len(output) or len(output) + length > expected:
                    raise RecompError('invalid Yaz0 backreference')
                # Yaz0 allows a match to overlap the bytes currently being copied.
                for _ in range(length):
                    output.append(output[-distance])
    except (IndexError, ValueError) as error:
        raise RecompError('truncated Yaz0 stream') from error
    return bytes(output)
