"""Game-specific inputs for CMake source generation."""
from pathlib import Path
from common import load_profile, load_rom, load_metadata, write_json
from discover import discover
from translate import translate
from native_hooks import resolve_native_hooks
from game_config import write_game_config


def prepare_full(output: Path,report_only: bool=False):
    project=Path(__file__).resolve().parents[3]
    profile=load_profile(project/'games/sm64-us/full.toml');game=profile['game']
    rom,identity=load_rom(game['rom'],game['rom_sha1'],game['rom_size'])
    write_json(output/'symbols.json',discover(rom,profile))
    metadata=load_metadata(output/'symbols.json',rom)
    report=translate(rom,metadata,profile,output,report_only=report_only)
    write_json(output/'full_boundaries.json',{'rom_sha1':identity['sha1'],'coverage':report['summary'],
        'code_inventory':metadata['code_inventory'],'native_hooks':resolve_native_hooks(rom,profile),
        'remaining':'broader gameplay, course transitions, ending and prolonged-run validation; additional device/microcode variants'})
    if not report_only:
        write_game_config(profile,metadata,output,('gSoundThread','gGameLoopThread','gControllers',
            'gControllerPads','gControllerBits','gEepromProbe','gSaveBuffer','gGlobalTimer',
            'gPhysicalFramebuffers','gPhysicalZBuffer','gGfxVblankQueue','gGameVblankQueue',
            'gCurrentArea','sSegmentTable','gGdCtrl','gCurrLevelNum','gCurrSaveFileNum',
            'sMenuSelectedFile','sLevelScriptCommand','gMarioState','gCamera','gDebugLevelSelect'))
    print(f"Full CPU inventory: {report['summary']}; {metadata['code_inventory']['cpu_words']} CPU instructions; ROM {identity['sha1']}",flush=True)
    return profile,identity,report
