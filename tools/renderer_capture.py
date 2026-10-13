"""Validate native rendered pixels and export lossless PNG captures."""
from pathlib import Path
import struct,zlib
from common import RecompError


def verify_ppm(path: Path, allow_grayscale: bool = False) -> dict:
    with path.open('rb') as file:
        if file.readline() != b'P6\n':
            raise RecompError('capture is not a binary PPM')
        width, height = map(int, file.readline().split())
        if file.readline() != b'255\n' or not 0 < width <= 8192 or not 0 < height <= 8192:
            raise RecompError('invalid capture dimensions/range')
        pixels = file.read()
    if len(pixels) != width * height * 3:
        raise RecompError('truncated pixel capture')
    colored = sum(max(pixels[i:i+3])-min(pixels[i:i+3])>24 for i in range(0,len(pixels),3))
    visible = sum(max(pixels[i:i+3])>16 for i in range(0,len(pixels),3)) if allow_grayscale else 0
    if allow_grayscale and visible < max(1, width * height // 1000):
        raise RecompError('pixel capture lacks a visible startup image')
    if not allow_grayscale and colored < width * height // 100:
        raise RecompError('pixel capture lacks the colored scene')
    # Lossless PNG export of the measured pixels, using only the standard library.
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    rows = b''.join(b'\0'+pixels[y*width*3:(y+1)*width*3] for y in range(height))
    png = b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))
    png += chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')
    path.with_suffix('.png').write_bytes(png)
    return {'width':width,'height':height,'colored_pixels':colored,
            **({'visible_pixels':visible} if allow_grayscale else {}),'png':str(path.with_suffix('.png'))}
