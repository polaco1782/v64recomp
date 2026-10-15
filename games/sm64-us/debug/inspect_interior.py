"""Verify the original Castle Grounds-to-lobby door warp and ROM asset replacement."""
from __future__ import annotations
import json
from pathlib import Path
from common import RecompError,write_json
from inspect_audio import inspect_audio
from inspect_f3d import inspect_f3d
from inspect_rom_assets import rom_asset_index,verify_level_asset


def inspect_interior(output: Path,rom: bytes) -> dict:
    records=[json.loads(line) for line in (output/'task_trace.jsonl').read_text().splitlines()]
    states=[r for r in records if r['type']=='castle_state' and r['play_frame']]
    summaries=[r for r in records if r['type']=='interior_summary']
    if len(summaries)!=1:raise RecompError('castle interior validation did not finish')
    order=[]
    for state in states:
        if not order or order[-1]!=state['level']:order.append(state['level'])
    if order!=[16,6]:raise RecompError(f'unexpected door-warp level order: {order}')
    inside=[s for s in states if s['level']==6]
    if len(inside)!=summaries[0]['target_frames'] or len(inside)<60:
        raise RecompError('insufficient castle interior frames')
    final=inside[-1]
    if final['area']!=1 or not final['floor'] or final['health']<0x100 or abs(final['position'][1]-final['floor_height'])>2:
        raise RecompError('Mario did not finish on a valid lobby floor')
    graphics=[r for r in records if r['type']=='graphics_task']
    assets=rom_asset_index(rom);snapshots=[r for r in records if r['type']=='level_snapshot'];checked={}
    if [r['level'] for r in snapshots]!=[16,6]:raise RecompError('missing level replacement snapshots')
    for snapshot in snapshots:
        level=snapshot['level'];ram=(output/f'level_{level}_rdram.bin').read_bytes()
        task=graphics[snapshot['graphics_task']-1]['descriptor']
        geometry=inspect_f3d(ram,task)
        if geometry.get('triangles',0)<200:raise RecompError('level capture lacks world geometry')
        checked[str(level)]={'asset':verify_level_asset(ram,snapshot['segments'],assets),
                             'triangles':geometry['triangles'],'geometry_sha256':geometry['geometry_sha256'],
                             'play_frame':snapshot['play_frame']}
    if checked['16']['asset']['rom_offset']==checked['6']['asset']['rom_offset']:
        raise RecompError('lobby still uses the Castle Grounds ROM asset')
    if checked['16']['geometry_sha256']==checked['6']['geometry_sha256']:
        raise RecompError('door warp did not replace the rendered scene')
    audio=inspect_audio(output)
    result={**summaries[0],'levels':order,'snapshots':checked,'audio_frames':audio['frames']}
    write_json(output/'interior_verification.json',result)
    print(f'PASS: original door warp: Castle Grounds -> castle lobby; {len(inside)} interior frames, ROM assets and audio verified.',flush=True)
    return result
