"""Game-specific inputs for CMake source generation."""
from common import write_json
from native_hooks import resolve_native_hooks
from game_config import write_game_config

REQUIRE_COMPLETE_INVENTORY = True


def write_config(profile, metadata, output, startup):
    write_game_config(profile,metadata,output,('gSoundThread','gGameLoopThread','gControllers',
        'gControllerPads','gControllerBits','gEepromProbe','gSaveBuffer','gGlobalTimer',
        'gPhysicalFramebuffers','gPhysicalZBuffer','gGfxVblankQueue','gGameVblankQueue',
        'gCurrentArea','sSegmentTable','gGdCtrl','gCurrLevelNum','gCurrSaveFileNum',
        'sMenuSelectedFile','sLevelScriptCommand','gMarioState','gCamera','gDebugLevelSelect'))


def write_report(rom, profile, metadata, report, output):
    write_json(output/'full_boundaries.json',{'rom_sha1':profile['game']['rom_sha1'],'coverage':report['summary'],
        'code_inventory':metadata['code_inventory'],
        'native_hooks':resolve_native_hooks(rom,profile),
        'remaining':'broader gameplay, course transitions, ending and prolonged-run validation; additional device/microcode variants'})
