"""Validate stereo PCM captures and compare game-triggered sound changes."""
from __future__ import annotations
from array import array
import hashlib
import json
import math
from pathlib import Path
import sys
import wave
from common import RecompError,write_json


def inspect_audio(output: Path) -> dict:
    stats=json.loads((output/'audio_results.json').read_text())
    with wave.open(str(output/'audio.wav'),'rb') as stream:
        if (stream.getnchannels(),stream.getsampwidth(),stream.getcomptype())!=(2,2,'NONE'):
            raise RecompError('audio capture is not 16-bit stereo PCM')
        frequency,frames=stream.getframerate(),stream.getnframes()
        pcm=stream.readframes(frames)
    if frequency!=stats['frequency'] or frames!=stats['frames'] or len(pcm)!=frames*4:
        raise RecompError('WAV layout differs from AI output statistics')
    samples=array('h');samples.frombytes(pcm)
    if sys.byteorder!='little':samples.byteswap()
    if frames<frequency//2 or max(map(abs,samples),default=0)<32:
        raise RecompError('audio capture is too short or silent')
    if sum(x!=0 for x in samples)!=stats['nonzero_samples']:
        raise RecompError('WAV samples differ from the delivered AI stream')
    rms=math.sqrt(sum(x*x for x in samples)/len(samples))
    if rms<10 or stats['clipped_samples']>len(samples)//100:
        raise RecompError('audio capture has insufficient signal or excessive clipping')
    result={**stats,'seconds':frames/frequency,'rms':rms,'sha256':hashlib.sha256(pcm).hexdigest()}
    write_json(output/'audio_verification.json',result)
    print(f'PASS: PCM: {frames} stereo frames at {frequency} Hz; RMS {rms:.1f}, peak {stats["peak"]}.',flush=True)
    return result


def compare_jump(idle: Path,jump: Path) -> dict:
    def read(directory):
        with wave.open(str(directory/'audio.wav'),'rb') as stream:
            return stream.getframerate(),stream.readframes(stream.getnframes())
    rate,base=read(idle);other_rate,changed=read(jump)
    if rate!=other_rate or len(base)!=len(changed):raise RecompError('sound comparisons have different timelines')
    first=next((i//4 for i,(a,b) in enumerate(zip(base,changed)) if a!=b),None)
    # Both runs use the same file and inputs until A is pressed at play frame 30.
    if first is None or not rate*0.8<first<rate*1.3:
        raise RecompError(f'jump audio did not diverge shortly after A: first frame {first}')
    count=sum(base[i:i+4]!=changed[i:i+4] for i in range(0,len(base),4))
    if count<rate//10:raise RecompError('jump sound changed too few PCM frames')
    result={'first_changed_frame':first,'first_changed_seconds':first/rate,'changed_stereo_frames':count}
    print(f'PASS: A/jump changes audio after {first/rate:.3f}s ({count} changed stereo frames).',flush=True)
    return result
