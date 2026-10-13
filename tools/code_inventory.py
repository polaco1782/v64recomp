"""Validate checked ROM code/data classifications and retain unreachable CPU words."""
from __future__ import annotations

from collections import Counter
import hashlib
import json
from common import RecompError


def load_entry_inventory(path, rom: bytes, sections: list[dict]) -> list[dict]:
    """Checked function roots for a partial conversion, without classifying gaps."""
    document = json.loads(path.read_text())
    if (document.get('format_version') != 1
            or document.get('rom_sha1') != hashlib.sha1(rom).hexdigest()
            or document.get('complete') is not False or not isinstance(document.get('entries'), list)):
        raise RecompError('invalid checked function entry inventory')
    result, seen = [], set()
    for entry in document['entries']:
        address = entry.get('vram') if isinstance(entry, dict) else None
        section = next((s for s in sections if type(address) is int
                        and s['vram'] <= address <= s['vram'] + s['size'] - 16), None)
        if section is None or address % 4 or address in seen:
            raise RecompError('invalid or duplicate checked function entry')
        offset = section['rom_offset'] + address - section['vram']
        if hashlib.sha256(rom[offset:offset+16]).hexdigest() != entry.get('sha256'):
            raise RecompError(f'function entry fingerprint mismatch at {address:#010x}')
        if entry.get('origin') not in ('direct-jal', 'stack-prologue', 'checked-root'):
            raise RecompError('checked function entry needs an explicit origin')
        seen.add(address)
        result.append({'vram':address})
    return result


def load_inventory(path,rom: bytes,sections: list[dict]) -> tuple[list[dict],list[dict]]:
    inventory=json.loads(path.read_text())
    if inventory.get('format_version')!=1 or inventory.get('rom_sha1')!=hashlib.sha1(rom).hexdigest():
        raise RecompError('code inventory belongs to a different ROM')
    ranges=inventory.get('ranges')
    if not isinstance(ranges,list):raise RecompError('code inventory needs explicit ranges')
    names={s['name'] for s in sections}
    if any(r.get('section') not in names for r in ranges):raise RecompError('inventory names an unknown section')
    for s in sections:
        selected=[r for r in ranges if r['section']==s['name']]
        cursor=s['vram']
        for r in selected:
            a,n=r.get('vram'),r.get('size')
            if (type(a) is not int or type(n) is not int or a!=cursor or a%4 or n<=0 or n%4
                    or a+n>s['vram']+s['size'] or r.get('kind') not in ('code','data','native_os','rsp')
                    or not isinstance(r.get('reason'),str) or not r['reason']):
                raise RecompError('inventory ranges must classify each section exactly once')
            offset=s['rom_offset']+a-s['vram']
            if hashlib.sha256(rom[offset:offset+n]).hexdigest()!=r.get('sha256'):
                raise RecompError(f'code inventory fingerprint mismatch at {a:#010x}')
            cursor=a+n
        if cursor!=s['vram']+s['size']:raise RecompError('code inventory leaves mapped bytes unclassified')
    entries=inventory.get('entries',[])
    if not isinstance(entries,list):raise RecompError('inventory entries must be a list')
    known=set()
    for entry in entries:
        address=entry.get('vram') if isinstance(entry,dict) else None
        if (type(address) is not int or address%4 or address in known
                or not any(r['kind']=='code' and r['vram']<=address<r['vram']+r['size'] for r in ranges)
                or 'name' in entry and (not isinstance(entry['name'],str) or not entry['name'])):
            raise RecompError('inventory entries must be unique addresses inside CPU text')
        known.add(address)
    return ranges,entries


def complete_code(rom: bytes,sections: list[dict],functions: list[dict],ranges: list[dict],
                  hooks: set[tuple[int,int]]) -> dict:
    counts=Counter();retained=padding=0
    for r in ranges:
        counts[r['kind']]+=r['size']
        s=next(s for s in sections if s['name']==r['section'])
        a,b=r['vram'],r['vram']+r['size']
        entries=sorted((f for f in functions if f['section']==s['id'] and a<=f['vram']<b),key=lambda f:f['vram'])
        if r['kind']!='code':
            if any((f['section'],f['vram']) not in hooks for f in entries):
                raise RecompError('ordinary function entry lies in a non-code inventory range')
            continue
        if any((f['section'],f['vram']) in hooks for f in entries):
            raise RecompError('replacement body must be classified as native OS code')
        first=entries[0]['vram'] if entries else b
        offset=s['rom_offset']+a-s['vram']
        if any(rom[offset:offset+first-a]):raise RecompError(f'code inventory needs an entry at {a:#010x}')
        padding+=first-a
        for index,f in enumerate(entries):
            end=entries[index+1]['vram'] if index+1<len(entries) else b
            if any(pc<f['vram'] or pc>=end for pc in f['instruction_addresses']):
                raise RecompError('code inventory exposes overlapping function ownership')
            retained+=(end-f['vram'])//4-len(f['instruction_addresses'])
            f['instruction_addresses']=list(range(f['vram'],end,4))
            f['size']=end-f['vram'];f['size_source']='checked-code-inventory'
            f['sha256']=hashlib.sha256(rom[f['rom_offset']:f['rom_offset']+f['size']]).hexdigest()
    return {'classified_bytes':sum(counts.values()),'bytes_by_kind':dict(counts),
            'retained_unreachable_words':retained,'leading_padding_bytes':padding,
            'cpu_words':(counts['code']-padding)//4,'complete':True}
