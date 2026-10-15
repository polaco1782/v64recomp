#!/usr/bin/env python3
"""Find callback roots in the pinned game's data, excluding checked switch tables."""
import argparse
import json
from pathlib import Path
import sys

PROJECT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(PROJECT/'tools'))
from common import RecompError, load_metadata, load_profile, load_rom, write_json
from metadata_candidates import materialized_entries, pointer_entries
sys.path.insert(0,str(PROJECT/'games/oot-us-v12/release'))
from prepare import prepare


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--metadata', type=Path, default=PROJECT/'build/oot-us-v12-debug/generated/oot-us-v12/symbols.json')
    parser.add_argument('--output', type=Path, default=PROJECT/'build/oot-us-v12/callback-candidates.json')
    parser.add_argument('--apply', action='store_true', help='add candidates to the partial entry inventory; validate with --report-only afterward')
    args = parser.parse_args()
    base = Path(__file__).resolve().parent.parent
    profile = load_profile(base/'startup.toml')
    game = profile['game']
    rom, _ = load_rom(game['rom'], game['rom_sha1'], game['rom_size'])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    rom, profile = prepare(rom, profile, args.output.parent)
    metadata = load_metadata(args.metadata, rom)
    text, data = [], []
    sections = {s['name']: s for s in metadata['sections']}
    # Boot's data contains exception-vector tables, handled by native OS services.
    for name, text_size, data_start in [('.code', 0xd1f20, 0xd6610)]:
        section = sections[name]
        text.append(dict(section, size=text_size))
        data.append(dict(section, vram=section['vram']+data_start,
            rom_offset=section['rom_offset']+data_start, size=section['size']-data_start))
    for overlay in json.loads((base/'overlays.json').read_text()):
        section = sections[overlay['name']]
        if overlay['text_size']:
            text.append(dict(section, size=overlay['text_size']))
        start = overlay['text_size']
        data.append(dict(section, vram=section['vram']+start,
            rom_offset=section['rom_offset']+start, size=overlay['relocation_offset']-start))
    entries = pointer_entries(rom, metadata, text, data, profile['discovery'].get('jump_tables', []))
    combined = {entry['vram']: entry for entry in entries}
    for entry in materialized_entries(rom, metadata, text):
        if entry['vram'] in combined:
            combined[entry['vram']].update(entry)
        else:
            combined[entry['vram']] = entry
    entries = [combined[address] for address in sorted(combined)]
    write_json(args.output, dict(format_version=1, rom_sha1=metadata['rom_sha1'], entries=entries))
    if args.apply:
        path = base/'startup-entries.json'
        inventory = json.loads(path.read_text())
        if inventory['rom_sha1'] != metadata['rom_sha1']:
            raise RecompError('entry inventory belongs to a different analysis image')
        known = {entry['vram'] for entry in inventory['entries']}
        inventory['entries'].extend(entry for entry in entries if entry['vram'] not in known)
        inventory['entries'].sort(key=lambda entry: entry['vram'])
        write_json(path, inventory)
    print(f'{len(entries)} callback candidates; {args.output}')


if __name__ == '__main__':
    main()
