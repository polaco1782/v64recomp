#!/usr/bin/env python3
"""Convert a checked ROM-bound controller JSON to the native debug input format."""
import argparse
import json
from pathlib import Path
from common import RecompError, load_profile
from generate_game import GAMES, PROJECT


def main() -> None:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('output',type=Path)
    parser.add_argument('--game',required=True,choices=GAMES)
    args=parser.parse_args()
    profile=load_profile(PROJECT/'games'/args.game/('full.toml' if args.game=='sm64-us' else 'startup.toml'))
    document=json.loads(args.source.read_text())
    if document.get('format_version')!=1 or document.get('rom_sha1')!=profile['game']['rom_sha1']:
        raise RecompError('controller replay belongs to a different ROM/version')
    events=document.get('events')
    if not isinstance(events,list) or not events:raise RecompError('controller replay needs events')
    previous=0 if args.game=='goldeneye-us' else -1
    rows=[]
    for e in events:
        values=[e.get('frame'),e.get('buttons'),e.get('stick_x',0),e.get('stick_y',0)]
        if any(type(v) is not int for v in values):raise RecompError('controller values must be integers')
        frame,buttons,x,y=values
        if not previous<frame<=0xffffffff or not 0<=buttons<=65535 or not -128<=x<=127 or not -128<=y<=127:
            raise RecompError('invalid frame or controller value')
        if args.game=='goldeneye-us' and frame>36000:raise RecompError('GoldenEye debug frame limit is 36000')
        previous=frame;rows.append(' '.join(map(str,values)))
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text('\n'.join(rows)+'\n')

if __name__=='__main__':main()
