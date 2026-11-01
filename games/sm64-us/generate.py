"""Game-specific inputs for CMake source generation."""
from common import write_json
from native_hooks import resolve_native_hooks
from game_config import write_game_config
import hashlib
from common import RecompError

REQUIRE_COMPLETE_INVENTORY = True


def write_config(profile, metadata, output, startup):
    write_game_config(profile,metadata,output,('gSoundThread','gGameLoopThread','gControllers',
        'gControllerPads','gControllerBits','gEepromProbe','gSaveBuffer','gGlobalTimer',
        'gPhysicalFramebuffers','gPhysicalZBuffer','gGfxVblankQueue','gGameVblankQueue',
        'gCurrentArea','sSegmentTable','gGdCtrl','gCurrLevelNum','gCurrSaveFileNum',
        'sMenuSelectedFile','sLevelScriptCommand','gMarioState','gCamera','gDebugLevelSelect'))
    spec = profile['graphics']
    rom = profile['game']['rom'].read_bytes()
    def data(address, size):
        section = next(s for s in metadata['sections'] if s['vram'] <= address and address + size <= s['vram'] + s['size'])
        offset = section['rom_offset'] + address - section['vram']
        return rom[offset:offset + size]
    lines = ['/* Graphics task identity checked against the pinned SM64 US ROM. */']
    for name in ('code','data'):
        item = spec[name]
        if hashlib.sha256(data(item['vram'],item['size'])).hexdigest() != item['sha256']:
            raise RecompError('SM64 graphics microcode fingerprint mismatch: ' + name)
        lines.extend([f'#define SM64_GFX_{name.upper()}_SIZE UINT32_C(0x{item["size"]:x})',
                      f'#define SM64_GFX_{name.upper()}_SHA256 "{item["sha256"]}"'])
    with (output/'game_config.h').open('a') as config:
        config.write('\n'.join(lines)+'\n')


def write_report(rom, profile, metadata, report, output):
    write_json(output/'full_boundaries.json',{'rom_sha1':profile['game']['rom_sha1'],'coverage':report['summary'],
        'code_inventory':metadata['code_inventory'],
        'native_hooks':resolve_native_hooks(rom,profile),
        'remaining':'broader gameplay, course transitions, ending and prolonged-run validation; additional device/microcode variants'})
