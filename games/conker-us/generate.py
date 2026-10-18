"""Prepare Conker US paged RareZip images; the runtime keeps the original ROM.

Layout reference: https://github.com/mkst/conker (US SHA-1 matches profile).
The offset table contains a data offset followed by XOR-obfuscated page offsets.
"""
import hashlib
import struct
import zlib
from common import RecompError, write_json


def images(rom):
    def unzip(offset, end):
        size = int.from_bytes(rom[offset:offset+4], 'big')
        if not 0 < size <= 0x400000:
            raise RecompError(f'Conker invalid RareZip size at {offset:#x}')
        decoder = zlib.decompressobj(-15)
        raw = decoder.decompress(rom[offset+4:end], size+1)
        if not decoder.eof or len(raw) != size:
            raise RecompError(f'Conker RareZip size mismatch at {offset:#x}')
        return raw
    offsets = [struct.unpack_from('>I', rom, 0x42454+i*4)[0] ^ 0x8039ccca for i in range(508)]
    if offsets[0] != 0x800 or offsets[-1] != 0x144700 or any(a >= b for a,b in zip(offsets, offsets[1:])):
        raise RecompError('Conker page table mismatch')
    pages = [unzip(0x42450+a, 0x42450+b) for a,b in zip(offsets, offsets[1:])]
    if any(len(page) != (0x130 if i == 506 else 0x1000) for i,page in enumerate(pages)):
        raise RecompError('Conker decoded page size mismatch')
    code = b''.join(pages)
    data = unzip(0x188328, 0x19c7d8)
    for raw, expected in [
        (code, '0d18b3a2e8d8f13a7697bba714a3ad75902656871a9d11593c2607cb39c4665d'),
        (data, '0bbc00e1adfac5ffe6117f5be9c941575f87b341a77d808004603ac1cb87c670')]:
        if hashlib.sha256(raw).hexdigest() != expected:
            raise RecompError('Conker prepared image fingerprint mismatch')
    return [('.game', 0x15000000, code), ('.game_data', 0x80082b20, data),
            ('.debugger', 0x16000000, rom[0x19ea88:0x1a37e0])]


def prepare(rom, profile, output):
    image = bytearray(rom)
    regions = list(profile['discovery']['regions'])
    for name,address,raw in images(rom):
        regions.append(dict(name=name,vram=address,rom_offset=len(image),size=len(raw)))
        image.extend(raw)
    return bytes(image), {**profile, 'discovery': {**profile['discovery'], 'regions': regions}}


def write_config(profile, metadata, output, startup):
    constants = {'ENTRY': metadata['entry_vram'], 'ROM_SIZE': profile['game']['rom_size'],
                 'RAM_SIZE': profile['game']['ram_size'],
                 **{k.upper(): v for k,v in profile['bootstrap'].items() if type(v) is int}}
    lines = ['/* Generated from the pinned Conker US profile. */']
    lines += [f'#define BOOT_{k} UINT32_C(0x{v:x})' for k,v in constants.items()]
    lines += ['static const uint8_t boot_sha1[20] = {' + ','.join(str(b) for b in bytes.fromhex(profile['game']['rom_sha1'])) + '};',
              'static const uint32_t boot_parameters[][2] = {']
    lines += [f'    {{{k}, 0x{v:x}}},' for k,v in profile['bootstrap']['parameters'].items()]
    lines += ['};']
    for name,address,raw in images(profile['game']['rom'].read_bytes()):
        tag = name[1:].upper()
        lines += [f'#define CONKER_{tag}_SIZE 0x{len(raw):x}', f'#define CONKER_{tag}_ADDRESS 0x{address:x}',
                  'static const uint8_t conker_'+tag.lower()+'_sha256[32] = {' + ','.join(str(b) for b in hashlib.sha256(raw).digest()) + '};']
    (output/'conker_config.h').write_text('\n'.join(lines)+'\n')


def configure_translation(rom, metadata, profile, output):
    from translate import translate
    audit = translate(rom,metadata,profile,output,report_only=True)
    functions = {(f['section'],f['vram']): f for f in metadata['functions']}
    pending = []
    for f in audit['functions']:
        if f['status'] != 'unsupported': continue
        original = functions[f['section'],f['vram']]
        offset = original['rom_offset']
        pending.append(dict(symbol=original['name'],vram=original['vram'],kind='unsupported_function',
            reason='Conker pending conversion: '+f['errors'][0]['reason'],
            expected_words=[int.from_bytes(rom[offset:offset+4],'big')]))
    profile.setdefault('boundaries',[]).extend(pending)
    write_json(output/'pending-boundaries.json',dict(complete_conversion=False,boundaries=pending))


def finalize_metadata(rom, metadata, profile):
    # These collision routines deliberately use JR RA as an internal call/return
    # mechanism. The outer caller is saved in FPR15, while RA names a loop label.
    functions = {f['vram']: f for f in metadata['functions']}
    epilogue = functions[0x150a3f5c]
    off = epilogue['rom_offset']
    if rom[off+0x50:off+0x60] != bytes.fromhex('441f78000000000003e0000800000000'):
        raise RecompError('Conker shared collision epilogue fingerprint mismatch')
    epilogue['return_address_fpr'] = 15
    # The bytecode abort restores the interpreter frame saved at 0x800d3858.
    # Its epilogue returns to the interpreter's caller, bypassing the callback.
    epilogue = functions[0x150ae0ac]
    off = epilogue['rom_offset']
    if rom[off:off+4] != bytes.fromhex('8fbf0000') or rom[off+0x44:off+0x4c] != bytes.fromhex('03e0000827bd0048'):
        raise RecompError('Conker bytecode epilogue fingerprint mismatch')
    epilogue['return_address_stack_offset'] = 0
    callback = functions[0x1509f850]
    sites = [0x1509f9a8, 0x1509fbb4]
    for pc in sites:
        off = callback['rom_offset'] + pc - callback['vram']
        if rom[off:off+4] != bytes.fromhex('0d42b8a0'):
            raise RecompError('Conker bytecode abort call fingerprint mismatch')
    callback['unwind_calls'] = [dict(pc=pc, expected_word=0x0d42b8a0) for pc in sites]
    decoder = functions[0x150adaf0]
    off = decoder['rom_offset'] + 0x150adffc - decoder['vram']
    if rom[off:off+4] != bytes.fromhex('0060f809'):
        raise RecompError('Conker bytecode callback dispatch fingerprint mismatch')
    decoder['nonlocal_return_calls'] = [0x150adffc]
    # Four wrappers tail-enter this dispatcher. Ordinary callbacks resume here;
    # an abort has already restored the interpreter's outer frame and RA.
    dispatcher = functions[0x150ae140]
    off = dispatcher['rom_offset'] + 0x150ae180 - dispatcher['vram']
    if rom[off:off+4] != bytes.fromhex('0060f809'):
        raise RecompError('Conker nested bytecode dispatch fingerprint mismatch')
    dispatcher['unwind_calls'] = [dict(pc=0x150ae180, expected_word=0x0060f809, conditional=True)]
