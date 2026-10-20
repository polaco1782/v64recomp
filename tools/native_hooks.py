"""Shared, fail-closed native OS/library recognition with explicit game overrides."""
from __future__ import annotations

from functools import lru_cache
import hashlib

from common import RecompError
from symbol_database import load_database, native_signatures


@lru_cache(maxsize=1)
def catalog() -> tuple[dict, ...]:
    return native_signatures(load_database())


def resolve_native_hooks(rom: bytes, profile: dict) -> list[dict]:
    """Recognize complete known bodies; a custom hook wins at its address.

    Prefixes only accelerate the search. Their complete body SHA-256 must match,
    including branch offsets and delay slots. Unknown variants stay translated
    unless the game's TOML explicitly supplies a checked replacement.
    """
    custom = profile.get('hooks', [])
    enabled = profile.get('native_hooks', {}).get('enabled', True)
    if type(enabled) is not bool:
        raise RecompError('native_hooks.enabled must be boolean')
    if not enabled:
        return list(custom)
    found = {}
    for region in profile.get('discovery', {}).get('regions', []):
        offset, end = region['rom_offset'], region['rom_offset'] + region['size']
        for signature in catalog():
            prefix = b''.join(w.to_bytes(4, 'big') for w in signature['expected_words'])
            position = rom.find(prefix, offset, end)
            while position != -1:
                size = signature['body_size']
                if (position % 4 == offset % 4 and position + size <= end
                        and hashlib.sha256(rom[position:position + size]).hexdigest() == signature['sha256']):
                    address = region['vram'] + position - offset
                    if address in found:
                        raise RecompError(f'ambiguous shared native hook at {address:#010x}')
                    found[address] = dict(symbol=signature['symbol'], vram=address,
                        kind='replace_function', handler=signature['handler'],
                        expected_words=[int.from_bytes(rom[p:p + 4], 'big')
                                        for p in range(position, position + size, 4)],
                        catalog_body_size=size)
                position = rom.find(prefix, position + 4, end)
    # Keep the customary symbol when unique. Multiple linked copies still have
    # separate registry entries and must not produce ambiguous symbol bindings.
    names = [h['symbol'] for h in found.values()] + [h['symbol'] for h in custom]
    custom_addresses = {h.get('vram') for h in custom}
    result = []
    for hook in custom:
        recognized = found.get(hook.get('vram'))
        result.append(dict(hook, catalog_body_size=recognized['catalog_body_size']) if recognized else hook)
    for address, hook in sorted(found.items()):
        if address in custom_addresses:
            continue
        if names.count(hook['symbol']) > 1:
            hook = dict(hook, symbol=f'{hook["symbol"]}_native_{address:08x}')
        result.append(hook)
    return result


def classify_native_bodies(rom: bytes, sections: list[dict], ranges: list[dict],
                           entries: list[dict], hooks: list[dict]) -> tuple[list[dict], list[dict]]:
    """Derive native OS/library spans from a verified complete CPU inventory.

    The stored inventory continues to describe the original ROM. Recognized
    bodies become native spans in the effective inventory, so adding a common
    signature never requires editing each game's inventory or callback roots.
    """
    bodies = sorted((h for h in hooks if 'catalog_body_size' in h), key=lambda h: h['vram'])
    result = []
    for region in ranges:
        start, end = region['vram'], region['vram'] + region['size']
        selected = [h for h in bodies if start <= h['vram'] < end]
        if not selected or region['kind'] != 'code':
            result.append(region)
            continue
        section = next(s for s in sections if s['name'] == region['section'])
        cursor = start

        def append(a, b, kind, reason):
            if a == b:
                return
            offset = section['rom_offset'] + a - section['vram']
            result.append(dict(region, vram=a, size=b - a, kind=kind, reason=reason,
                               sha256=hashlib.sha256(rom[offset:offset + b - a]).hexdigest()))

        for hook in selected:
            address, size = hook['vram'], hook['catalog_body_size']
            if address < cursor or address + size > end:
                raise RecompError('shared native body crosses inventory ownership')
            append(cursor, address, 'code', region['reason'])
            append(address, address + size, 'native_os', 'Verified shared native library: ' + hook['symbol'])
            cursor = address + size
        append(cursor, end, 'code', region['reason'])
    entries = [e for e in entries if not any(h['vram'] <= e['vram'] < h['vram'] + h['catalog_body_size'] for h in bodies)]
    return result, entries
