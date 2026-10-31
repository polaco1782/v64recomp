#!/usr/bin/env python3
"""Compare the recompiled ROM decoder with FFmpeg on original synthetic MP3s.

Requires a Debug media-player executable and FFmpeg with libmp3lame. Fixtures,
captures and logs stay in --output. No music or ROM data is redistributed.
"""
import argparse
from array import array
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import wave


def pcm(path):
    with wave.open(str(path), 'rb') as f:
        assert f.getnchannels() == 2 and f.getsampwidth() == 2
        rate = f.getframerate()
        values = array('h', f.readframes(f.getnframes()))
        if sys.byteorder != 'little': values.byteswap()
        return rate, values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--ffmpeg', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    ffmpeg = [str(args.ffmpeg.resolve()), '-hide_banner', '-loglevel', 'error', '-y']
    cases = [('stereo-cbr',44100,2,3,['-b:a','128k']),
             ('mono-mpeg2',22050,1,3,['-b:a','64k']),
             ('stereo-vbr',48000,2,12,['-q:a','2'])]
    results = []
    for name, rate, channels, seconds, encoding in cases:
        # Alternating levels exercise transient blocks as well as steady synthesis.
        left = '(0.05+0.12*lt(mod(t,0.5),0.2))*sin(2*PI*440*t)'
        right = '0.13*sin(2*PI*733*t)+0.04*sin(2*PI*3127*t)'
        expression = left+(f'|{right}' if channels == 2 else '')
        source = f"aevalsrc='{expression}':s={rate}:d={seconds}"
        mp3, reference, actual = (out/(name+ext) for ext in ('.mp3','-ref.wav','-rom.wav'))
        subprocess.run([*ffmpeg,'-f','lavfi','-i',source,'-c:a','libmp3lame',*encoding,
                        '-write_xing','0',str(mp3)],check=True)
        subprocess.run([*ffmpeg,'-i',str(mp3),'-ac','2','-c:a','pcm_s16le',str(reference)],check=True)
        # Keep diagnostic runs separate from the user's interactive player.
        # Only video is hidden; the real SDL audio device/queue still runs.
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', RECOMP_MEDIA_FILE=str(mp3),
                   RECOMP_MEDIA_SNAPSHOT=str(out/(name+'.bmp')))
        with (out/(name+'.log')).open('w') as log:
            subprocess.run([str(args.exe.resolve()),'--frames','100000','--wav',str(actual),'--mute'],
                           env=env,stdout=log,stderr=subprocess.STDOUT,timeout=90,check=True)
        ref_rate, ref = pcm(reference)
        rom_rate, rom = pcm(actual)
        assert ref_rate == rom_rate == rate, (name,ref_rate,rom_rate)
        assert len(ref) == len(rom), (name,len(ref),len(rom))
        # FFmpeg's -ac 2 pans mono at -3 dB; the player intentionally duplicates it.
        if channels == 1:
            subprocess.run([*ffmpeg,'-i',str(mp3),'-af','pan=stereo|c0=c0|c1=c0',
                            '-c:a','pcm_s16le',str(reference)],check=True)
            _, ref = pcm(reference)
        rms = math.sqrt(sum((a-b)**2 for a,b in zip(ref,rom))/len(ref))
        peak = max(abs(a-b) for a,b in zip(ref,rom))
        assert rms < 2 and peak <= 8, (name,rms,peak)
        assert max(map(abs,rom)) > 1000, (name,'silent capture')
        log = (out/(name+'.log')).read_text()
        assert 'underruns=0' in log, (name,'audio queue drained')
        results.append(dict(name=name,rate=rate,samples=len(rom)//2,mp3_bytes=mp3.stat().st_size,
                            error_rms=rms,error_peak=peak,underruns=0))
        print('PASS:',results[-1],flush=True)
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    # EOF, malformed input, a partial final frame, and a real format transition.
    edge_files = {
        'empty': b'', 'invalid': b'not an MPEG audio stream'*4096,
        'truncated': (out/'stereo-cbr.mp3').read_bytes()[:-200],
        'rate-change': (out/'mono-mpeg2.mp3').read_bytes()+(out/'stereo-cbr.mp3').read_bytes(),
    }
    for name, data in edge_files.items():
        mp3 = out/(name+'.mp3')
        mp3.write_bytes(data)
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', RECOMP_MEDIA_FILE=str(mp3))
        env.pop('RECOMP_MEDIA_SNAPSHOT',None)
        with (out/(name+'.log')).open('w') as log:
            completed = subprocess.run([str(args.exe.resolve()),'--frames','100000','--mute',
                                        '--wav',str(out/(name+'.wav'))],env=env,
                                       stdout=log,stderr=subprocess.STDOUT,timeout=30)
        expected = 2 if name in ('empty','invalid') else 0
        assert completed.returncode == expected, (name,completed.returncode)
        if name == 'rate-change':
            assert 'WAV capture stopped at sample-rate change' in (out/(name+'.log')).read_text()
            pcm(out/(name+'.wav'))  # Header finalized despite stopping capture early.
        print('PASS:',name,'exit',expected,flush=True)


if __name__ == '__main__':
    main()
