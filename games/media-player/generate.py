"""Prepare Media Player V2 from its pinned embedded ELF and symbol table."""
from copy import deepcopy
import hashlib
from pathlib import Path
import struct

from common import write_json
from prepare import lz4_decode, read_symbols, require


def prepare(rom: bytes, profile: dict, output: Path) -> tuple[bytes, dict]:
    profile = deepcopy(profile)
    d = profile['discovery']
    boot = profile['bootstrap']
    elf = rom[d['elf_offset']:]
    header = struct.unpack_from('>16sHHIIIIIHHHHHH', elf)
    require(header[0][:7] == b'\x7fELF\x01\x02\x01' and header[2] == 8 and
            header[4] == d['rom_entry'] and header[7] == 0x20002001 and
            header[9:11] == (32, 3), 'ELF header/O64 ABI mismatch')
    image = bytearray(rom)
    for segment in d['segments']:
        p = struct.unpack_from('>8I', elf, header[5]+32*segment['index'])
        kind, offset, compressed_address, address, size, memory_size, flags, alignment = p
        require(kind == 1 and flags & 0x1000 and address == segment['vram'] and
                memory_size == segment['memory_size'] and offset + size <= len(elf), 'ELF segment mismatch')
        require(d['elf_offset']+offset == boot['compressed_offset'] and size == boot['compressed_size'] and
                address == boot['segment_vram'] and segment['size'] == boot['segment_size'],
                'runtime and analysis ELF mappings differ')
        raw = lz4_decode(elf[offset:offset+size], segment['size'])
        require(len(raw) == segment['size'] and hashlib.sha256(raw).hexdigest() == segment['sha256'],
                'decoded segment fingerprint mismatch')
        d['regions'].append(dict(name=segment['name'], vram=address, rom_offset=len(image), size=len(raw)))
        image.extend(raw)
    symbols = read_symbols(rom[d['symbol_offset']:d['symbol_offset']+d['symbol_size']])
    for name, address in [('mp3dec_decode_frame', d['entry_vram']), ('mp3dec_init', boot['decoder_init'])]:
        require(any(s['name'] == name and s['vram'] == address for s in symbols),
                f'embedded symbol mismatch: {name}')
    # Only the codec and its statically discovered callees are in this target.
    # The original SD/browser/RSPQ interface is deliberately outside its scope.
    d['roots'] = [dict(name='mp3dec_init', vram=boot['decoder_init'], section='.main')]
    write_json(output/'embedded-symbols.json', symbols)
    return bytes(image), profile


def write_config(profile: dict, metadata: dict, output: Path, startup: bool) -> None:
    game = profile['game']
    constants = dict(profile['bootstrap'], entry=metadata['entry_vram'], rom_size=game['rom_size'],
                     ram_size=game['ram_size'])
    lines = ['/* Generated from the pinned Media Player profile. */']
    lines += [f'#define BOOT_{k.upper()} UINT32_C(0x{v:x})' for k, v in constants.items()]
    lines += ['static const uint8_t boot_sha1[20] = {' + ','.join(str(b) for b in bytes.fromhex(game['rom_sha1']))+'};']
    (output/'media_player_config.h').write_text('\n'.join(lines)+'\n')
