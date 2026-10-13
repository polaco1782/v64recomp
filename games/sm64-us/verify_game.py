"""Acceptance checks of the full native SM64 program and isolated save devices."""
from pathlib import Path
import json,subprocess
from common import RecompError,write_json
from inspect_f3d import inspect_f3d
from inspect_audio import inspect_audio,compare_jump


def verify_game(output: Path) -> dict:
    records=[json.loads(line) for line in (output/'task_trace.jsonl').read_text().splitlines()]
    if any(r['type']=='interior_summary' for r in records):
        from inspect_interior import inspect_interior
        from common import load_profile,load_rom
        game=load_profile(Path(__file__).resolve().parent/'full.toml')['game']
        rom,_=load_rom(game['rom'],game['rom_sha1'],game['rom_size'])
        return inspect_interior(output,rom)
    states=[r for r in records if r['type']=='castle_state']
    graphics=[r for r in records if r['type']=='graphics_task']
    summaries=[r for r in records if r['type']=='castle_summary']
    if len(summaries)!=1:raise RecompError('Castle Grounds did not finish validation')
    result=summaries[0]
    playable=[r for r in states if r['play_frame']]
    if len(playable)!=result['play_frames'] or len(playable)<60:
        raise RecompError('insufficient playable Castle Grounds frames')
    for prefix,state in (('first_playable',playable[0]),('last_playable',playable[-1])):
        inspected=inspect_f3d((output/f'{prefix}_rdram.bin').read_bytes(),graphics[state['graphics_task']-1]['descriptor'])
        if inspected.get('triangles',0)<300 or inspected.get('vertices_loaded',0)<300:
            raise RecompError('Castle Grounds capture lacks world geometry')
        if not all(n in inspected['referenced_lists'] for n in ('0x07006d70','0x070095f0')):
            raise RecompError('Castle Grounds model lists were not submitted')
        write_json(output/f'{prefix}_inspection.json',inspected)
    final=playable[-1]
    if not final['floor'] or final['health']<0x100 or abs(final['position'][1]-final['floor_height'])>2:
        raise RecompError('Mario did not finish standing on a valid floor')
    result={**result,'last_triangles':inspected['triangles'],'actions':sorted({r['action'] for r in states}),
        'opening_cutscene_seen':any(r['action']==0x04001301 for r in states)}
    if (output/'audio_results.json').exists():inspect_audio(output)
    write_json(output/'castle_verification.json',result)
    print(f"PASS: Castle Grounds geometry: {inspected['triangles']} triangles; {result['play_frames']} playable frames.",flush=True)
    return result


def active_save_fixture(formatted: bytes) -> bytes:
    """Independent test fixture: mark file A active and sign both 56-byte copies.

    All menu data and other files come from the original game's formatting run.
    Only an isolated acceptance-test device uses this fixture.
    """
    if len(formatted)!=512:raise RecompError('fixture requires a 512-byte EEPROM')
    data=bytearray(formatted)
    for offset in (0,56):
        data[offset+8:offset+12]=(int.from_bytes(data[offset+8:offset+12],'big')|1).to_bytes(4,'big')
        data[offset+52:offset+54]=bytes.fromhex('4441')
        data[offset+54:offset+56]=(sum(data[offset:offset+54])&0xffff).to_bytes(2,'big')
    return bytes(data)


def verify_suite(binary: Path,profile: dict,output: Path,frames: int):
    root=output/'cases';root.mkdir(parents=True,exist_ok=True)
    fresh=root/'fresh.eeprom';fresh.unlink(missing_ok=True)
    def run(name,save,mode):
        dest=root/name;dest.mkdir(parents=True,exist_ok=True)
        subprocess.run([str(binary),str(profile['game']['rom']),str(dest),str(save),str(frames),mode],check=True,timeout=240)
        return verify_game(dest)
    results={'fresh':run('fresh',fresh,'idle')}
    if not results['fresh']['opening_cutscene_seen'] or results['fresh']['world_frames']<900:
        raise RecompError('fresh file did not run the original opening cutscene')
    fixture=active_save_fixture(fresh.read_bytes())
    for name in ('idle','move','jump'):
        save=root/f'{name}.eeprom';save.write_bytes(fixture)
        results[name]=run(name,save,name)
        if results[name]['opening_cutscene_seen'] or save.read_bytes()!=fixture:
            raise RecompError('existing-file test changed EEPROM or replayed the fresh-file opening')
    if results['idle']['distance']>1:raise RecompError('idle Mario moved without stick input')
    if not all(results[name]['start_position']==results['idle']['start_position'] for name in ('move','jump')):
        raise RecompError('controller comparisons did not start from the same Mario position')
    if results['move']['distance']<100 or 0x04000440 not in results['move']['actions']:
        raise RecompError('analog stick did not produce walking and displacement')
    if results['jump']['max_height']-results['idle']['max_height']<100 or not any(a&0x800 for a in results['jump']['actions']):
        raise RecompError('A did not produce an airborne action and vertical movement')
    audio=compare_jump(root/'idle',root/'jump')
    write_json(output/'audio_suite.json',audio)
    save=root/'interior.eeprom';save.write_bytes(fixture)
    results['interior']=run('interior',save,'interior')
    if save.read_bytes()!=fixture:raise RecompError('door-warp test changed the isolated EEPROM')
    write_json(output/'castle_suite.json',results)
    print('PASS: fresh introduction, existing-file entry, walking, jumping, landing and castle door warp.',flush=True)

