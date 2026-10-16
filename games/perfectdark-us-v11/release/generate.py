"""Expand the pinned RareZip CPU images for static translation only.

The executable retains the original cartridge and runs its original loaders.
Layout reference: https://github.com/n64decomp/perfect_dark (ntsc-final).
"""
from pathlib import Path
import hashlib
import struct
import zlib
from common import RecompError, write_json


def images(rom):
    def unzip(offset):
        if rom[offset:offset+2] != b'\x11\x73':
            raise RecompError(f'Perfect Dark RareZip header missing at {offset:#x}')
        size = int.from_bytes(rom[offset+2:offset+5], 'big')
        if not 0 < size <= 0x800000:
            raise RecompError('Perfect Dark invalid RareZip size')
        decoder = zlib.decompressobj(-15)
        data = decoder.decompress(rom[offset+5:], size+1)
        if not decoder.eof or len(data) != size:
            raise RecompError('Perfect Dark RareZip size mismatch')
        return data
    lib = rom[0x1050:0x3050] + unzip(0x3050)
    data = unzip(0x39850)
    parts = []
    for index in range(444):
        offset = struct.unpack_from('>I', rom, 0x4fc40+index*4)[0]
        part = unzip(0x4fc40+offset+2)
        parts.append(part)
        if len(part) != 0x1000:
            break
    result = [('.lib', 0x70001050, lib), ('.inflate', 0x70200000, rom[0x4e850:0x4fc40]),
              ('.data', 0x80059fe0, data), ('.game', 0x7f000000, b''.join(parts))]
    for name, raw, expected in [
        ('.lib', lib, 'bd930d8c530be04b320d10f3a72594f106f0c54f0a580a93c747e2f9adb8cf5b'),
        ('.data', data, 'c789f6fe1eeebff0ca4a89006333147e8d53452a83db3e4409fe6e64f4d615f3'),
        ('.game', result[-1][2], '0ee3c0a8bb3c7f006ec1e81d5400ac93d6d9d27c4062c934e03d7caa70e8d7d0')]:
        if hashlib.sha256(raw).hexdigest() != expected:
            raise RecompError(f'Perfect Dark decoded {name} fingerprint mismatch')
    return result


def prepare(rom, profile, output):
    image = bytearray(rom)
    regions = list(profile['discovery']['regions'])
    for name, address, raw in images(rom):
        regions.append(dict(name=name, vram=address, rom_offset=len(image), size=len(raw)))
        image.extend(raw)
    return bytes(image), {**profile, 'discovery': {**profile['discovery'], 'regions': regions}}


def write_config(profile, metadata, output, startup):
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': profile['game']['rom_size'],
                 'RAM_SIZE': profile['game']['ram_size'], **{k.upper(): v for k,v in profile['bootstrap'].items() if type(v) is int}}
    lines = ['/* Generated from the pinned Perfect Dark US 1.1 profile. */']
    lines += [f'#define BOOT_{k} UINT32_C(0x{v:x})' for k,v in constants.items()]
    lines += ['static const uint8_t boot_sha1[20] = {' + ','.join(str(b) for b in bytes.fromhex(profile['game']['rom_sha1'])) + '};',
              'static const uint32_t boot_parameters[][2] = {']
    lines += [f'    {{{k}, 0x{v:x}}},' for k,v in profile['bootstrap']['parameters'].items()]
    lines += ['};']
    for name,address,raw in images(profile['game']['rom'].read_bytes()):
        tag=name[1:].upper()
        lines += [f'#define PD_{tag}_SIZE 0x{len(raw):x}',
                  f'#define PD_{tag}_ADDRESS 0x{address:x}',
                  'static const uint8_t pd_'+tag.lower()+'_sha256[32] = {' + ','.join(str(b) for b in hashlib.sha256(raw).digest()) + '};']
    (output/'perfectdark_config.h').write_text('\n'.join(lines)+'\n')


def configure_translation(rom, metadata, profile, output):
    from translate import translate
    audit=translate(rom,metadata,profile,output,report_only=True)
    functions={(f['section'],f['vram']): f for f in metadata['functions']}
    pending=[]
    for f in audit['functions']:
        if f['status']!='unsupported': continue
        original=functions[f['section'],f['vram']]
        offset=original['rom_offset']
        pending.append(dict(symbol=original['name'],vram=original['vram'],kind='unsupported_function',
            reason='Perfect Dark pending conversion: '+f['errors'][0]['reason'],
            expected_words=[int.from_bytes(rom[offset:offset+4],'big')]))
    profile.setdefault('boundaries',[]).extend(pending)
    write_json(output/'pending-boundaries.json',dict(complete_conversion=False,boundaries=pending))


def finalize_metadata(rom, metadata, profile):
    # This shared epilogue is entered by fallthrough, after internal JALs have
    # overwritten RA. Its architectural caller comes from the model frame.
    function=next(f for f in metadata['functions'] if f['vram']==0x700195cc)
    offset=function['rom_offset']
    if (int.from_bytes(rom[offset+0x28:offset+0x2c],'big')!=0x8fbf07a8
            or rom[offset+0x5c:offset+0x64]!=bytes.fromhex('03e0000827bd0800')):
        raise RecompError('Perfect Dark model return ABI fingerprint mismatch')
    function['return_address_stack_offset']=0x7a8
    # Both frame readers can branch to the outer model epilogue. Propagate
    # that restored caller RA through the corresponding native call frames.
    model=next(f for f in metadata['functions'] if f['vram']==0x70018680)
    sites={0x7001879c:0x0c00658d, 0x7001886c:0x0c006637,
           0x70018910:0x0c00658d, 0x700189c4:0x0c006637}
    for pc,expected in sites.items():
        offset=model['rom_offset']+pc-model['vram']
        if int.from_bytes(rom[offset:offset+4],'big')!=expected:
            raise RecompError('Perfect Dark nonlocal return call fingerprint mismatch')
    model['nonlocal_return_calls']=list(sites)
