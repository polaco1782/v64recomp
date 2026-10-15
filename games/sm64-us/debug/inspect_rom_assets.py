"""Read-only ROM asset checks for captured level segments.

This decoder is an acceptance-test oracle. Runtime decompression still executes
the original translated MIPS code, and runtime assets come from the ROM.
"""
from __future__ import annotations

import hashlib
from common import RecompError


def decode_mio0(rom: bytes, offset: int) -> bytes:
    if offset<0 or offset+16>len(rom) or rom[offset:offset+4]!=b'MIO0':
        raise RecompError('invalid MIO0 header')
    size,links,raw=(int.from_bytes(rom[offset+n:offset+n+4],'big') for n in (4,8,12))
    if not 0<size<=4*1024*1024 or not 16<=links<=raw or offset+raw>len(rom):
        raise RecompError('invalid MIO0 size or stream offsets')
    mask_pos,link_pos,raw_pos=offset+16,offset+links,offset+raw
    result=bytearray();mask=bits=0
    while len(result)<size:
        if not bits:
            if mask_pos>=offset+links:raise RecompError('MIO0 mask stream exhausted')
            mask=rom[mask_pos];mask_pos+=1;bits=8
        if mask&128:
            if raw_pos>=len(rom):raise RecompError('MIO0 literal stream exhausted')
            result.append(rom[raw_pos]);raw_pos+=1
        else:
            if link_pos+2>offset+raw:raise RecompError('MIO0 link stream exhausted')
            link=int.from_bytes(rom[link_pos:link_pos+2],'big');link_pos+=2
            count,distance=(link>>12)+3,(link&4095)+1
            if distance>len(result) or len(result)+count>size:raise RecompError('invalid MIO0 back-reference')
            for _ in range(count):result.append(result[-distance])
        mask=(mask<<1)&255;bits-=1
    return bytes(result)


def rom_asset_index(rom: bytes) -> dict[bytes,list[tuple[int,bytes]]]:
    """Index valid compressed blocks by a static prefix, without symbol inputs."""
    result={};offset=0
    while True:
        offset=rom.find(b'MIO0',offset)
        if offset<0:break
        try:
            decoded=decode_mio0(rom,offset)
        except RecompError:
            offset+=4;continue
        result.setdefault(decoded[:256],[]).append((offset,decoded));offset+=4
    return result


def verify_level_asset(ram: bytes, segments: list[int], assets: dict) -> dict:
    if len(segments)!=16:raise RecompError('level snapshot needs 16 segment bases')
    start=segments[7]&0x1fffffff
    if not start or start+256>len(ram):raise RecompError('level segment is not loaded')
    # The game may animate painting vertices or water after loading. Report the
    # full comparison and require the static prefix plus almost all asset bytes.
    prefix=bytes(ram[(start+n)^3] for n in range(256))
    candidates=assets.get(prefix,[])
    if len(candidates)!=1:raise RecompError('level segment has no unique ROM asset match')
    offset,decoded=candidates[0]
    if start+len(decoded)>len(ram):raise RecompError('ROM asset exceeds captured RAM')
    actual=bytes(ram[(start+n)^3] for n in range(len(decoded)))
    changes=sum(a!=b for a,b in zip(actual,decoded))
    if changes>len(decoded)//100:raise RecompError('loaded level differs excessively from its ROM asset')
    return {'segment':7,'ram_address':start,'rom_offset':offset,'decoded_bytes':len(decoded),
            'changed_bytes':changes,'rom_asset_sha256':hashlib.sha256(decoded).hexdigest(),
            'ram_asset_sha256':hashlib.sha256(actual).hexdigest()}
