"""Read checked Kyoto USO directories and their optional Yay0 payloads."""
import struct

from common import RecompError


def sections_at(rom: bytes, module: int, header_word: int = 0x12345678) -> dict:
    if module < 0 or rom[module:module + 4] != header_word.to_bytes(4, 'big'):
        raise RecompError('1080 USO header mismatch')
    sections, position = {}, module + 4
    for _ in range(16):
        if position + 12 > len(rom):
            raise RecompError('1080 truncated USO directory')
        kind, size, flags = struct.unpack_from('>3I', rom, position)
        if kind == 11:
            return sections
        if kind in sections or kind > 13 or (kind != 8 and position + 12 + size > len(rom)):
            raise RecompError('1080 malformed USO directory')
        sections[kind] = (position + 12, size, flags)
        # Stored compressed sizes are exact byte lengths, without word padding.
        position += 12 + (0 if kind == 8 else size)
    raise RecompError('1080 unterminated USO directory')


def section_bytes(rom: bytes, section: tuple) -> bytes:
    offset, size, flags = section
    data = rom[offset:offset + size]
    if not flags & 0x10000:
        return data
    if len(data) < 16 or data[:4] != b'Yay0':
        raise RecompError('1080 compressed USO section has no Yay0 header')
    length, links, chunks = struct.unpack_from('>3I', data, 4)
    if not 0 < length <= 0x400000 or not 16 <= links <= chunks <= len(data):
        raise RecompError('1080 invalid Yay0 stream ranges')
    masks, mask_end, link_end, mask, bits = 16, links, chunks, 0, 0
    output = bytearray()
    while len(output) < length:
        if not bits:
            if masks + 4 > mask_end:
                raise RecompError('1080 truncated Yay0 mask stream')
            mask = struct.unpack_from('>I', data, masks)[0]
            masks += 4
            bits = 32
        if mask & 0x80000000:
            if chunks >= len(data):
                raise RecompError('1080 truncated Yay0 literal stream')
            output.append(data[chunks])
            chunks += 1
        else:
            if links + 2 > link_end:
                raise RecompError('1080 truncated Yay0 link stream')
            link = struct.unpack_from('>H', data, links)[0]
            links += 2
            count, distance = link >> 12, (link & 0xfff) + 1
            if not count:
                if chunks >= len(data):
                    raise RecompError('1080 truncated Yay0 length stream')
                count = data[chunks] + 18
                chunks += 1
            else:
                count += 2
            if distance > len(output) or count > length - len(output):
                raise RecompError('1080 invalid Yay0 back reference')
            for _ in range(count):
                output.append(output[-distance])
        mask = (mask << 1) & 0xffffffff
        bits -= 1
    return bytes(output)
