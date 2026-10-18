"""ROM/profile validation shared by the standalone build tools."""
from __future__ import annotations

import hashlib
import json
import tomllib
from pathlib import Path


class RecompError(Exception):
    pass


def load_rom(path: Path, expected_sha1: str | None = None,
             expected_size: int | None = None) -> tuple[bytes, dict]:
    data = path.read_bytes()
    if len(data) < 64 or len(data) % 4:
        raise RecompError("ROM must contain a header and a multiple of four bytes")
    magic = data[:4]
    if magic == bytes.fromhex("80371240"):
        normalized, order = data, "big-endian"
    elif magic == bytes.fromhex("37804012"):
        swapped = bytearray(data)
        swapped[0::2], swapped[1::2] = data[1::2], data[0::2]
        normalized, order = bytes(swapped), "byte-swapped"
    elif magic == bytes.fromhex("40123780"):
        swapped = bytearray(data)
        for lane in range(4):
            swapped[lane::4] = data[3 - lane::4]
        normalized, order = bytes(swapped), "word-swapped"
    else:
        raise RecompError(f"unrecognized N64 ROM header: {magic.hex()}")
    digest = hashlib.sha1(normalized).hexdigest()
    if expected_sha1 is not None and digest != expected_sha1.lower():
        raise RecompError(f"ROM SHA-1 mismatch: expected {expected_sha1}, got {digest}")
    if expected_size is not None and len(normalized) != expected_size:
        raise RecompError(f"ROM size mismatch: expected {expected_size}, got {len(normalized)}")
    return normalized, {"sha1": digest, "size": len(normalized), "input_order": order}


def load_profile(path: Path, _parents: tuple[Path, ...] = ()) -> dict:
    path = path.resolve()
    if path in _parents:
        raise RecompError('cyclic profile inheritance')
    with path.open("rb") as stream:
        profile = tomllib.load(stream)
    parent = profile.pop('extends', None)
    if parent is not None:
        if not isinstance(parent, str):
            raise RecompError('profile extends must name a file')
        base = load_profile(path.parent / parent, (*_parents, path))

        def merge(left, right, location=()):
            result = dict(left)
            for key, value in right.items():
                site = (*location, key)
                if isinstance(value, dict) and isinstance(result.get(key), dict):
                    result[key] = merge(result[key], value, site)
                elif site in (('hooks',), ('boundaries',), ('checkpoints',), ('discovery', 'roots'),
                              ('discovery', 'transfers'), ('discovery', 'jump_tables'),
                              ('discovery', 'function_tables'), ('discovery', 'fallthroughs'),
                              ('discovery', 'tail_continuations')):
                    if not isinstance(value, list) or not isinstance(result.get(key, []), list):
                        raise RecompError(f'profile field {".".join(site)} must be a list')
                    result[key] = [*result.get(key, []), *value]
                else:
                    result[key] = value
            return result

        profile = merge(base, profile)
    game = profile.get("game", {})
    required = ("name", "rom_sha1", "rom_size", "ram_size", "rom",
                "code_sections", "entry_symbol")
    missing = [key for key in required if key not in game]
    if missing:
        raise RecompError(f"profile missing game fields: {', '.join(missing)}")
    if not isinstance(game["code_sections"], list) or not game["code_sections"]:
        raise RecompError("profile needs an explicit nonempty code_sections list")
    game = dict(game)
    game["rom"] = (path.parent / game["rom"]).resolve()
    discovery = dict(profile.get('discovery', {}))
    for key in ('inventory', 'entry_inventory'):
        if key in discovery:
            if not isinstance(discovery[key], (str, Path)):
                raise RecompError(f'{key} must name a file')
            discovery[key] = (path.parent / discovery[key]).resolve()
    return {**profile, "game": game, 'discovery':discovery}


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def validate_hook_prefix(rom: bytes, offset: int, address: int, available: int, hook: dict) -> int:
    """Verify a replacement's explicit entry address and optional ROM fingerprint."""
    if hook.get('vram', address) != address:
        raise RecompError(f"hook address mismatch: {hook['symbol']}")
    words = hook.get('expected_words', [])
    if (not isinstance(words, list) or any(type(w) is not int or not 0 <= w <= 0xffffffff for w in words)
            or 'vram' in hook and not words or len(words) * 4 > available):
        raise RecompError(f"invalid hook fingerprint: {hook['symbol']}")
    expected = b''.join(w.to_bytes(4, 'big') for w in words)
    if rom[offset:offset + len(expected)] != expected:
        raise RecompError(f"hook fingerprint mismatch: {hook['symbol']}")
    return len(expected)


def load_metadata(path: Path, rom: bytes) -> dict:
    metadata = json.loads(path.read_text())
    if metadata.get("format_version") != 1:
        raise RecompError("unsupported function metadata version")
    if metadata.get("rom_sha1") != hashlib.sha1(rom).hexdigest():
        raise RecompError("metadata belongs to a different ROM")
    if metadata.get("rom_size") != len(rom):
        raise RecompError("metadata ROM size mismatch")
    sections = {s["id"]: s for s in metadata["sections"]}
    if not sections or sorted(sections) != list(range(len(metadata["sections"]))):
        raise RecompError("section IDs must be unique and contiguous")
    for section in sections.values():
        address, size, offset = section['vram'], section['size'], section['rom_offset']
        if (address < 0 or address % 4 or size <= 0 or size % 4
                or address + size > 0x100000000 or offset < 0 or offset % 4
                or offset + size > len(rom)):
            raise RecompError(f"invalid section mapping: {section['name']}")
        if section.get('sha256') != hashlib.sha256(rom[offset:offset + size]).hexdigest():
            raise RecompError(f"section bytes do not match metadata: {section['name']}")
    seen = set()
    claimed = set()
    for function in metadata["functions"]:
        section = sections.get(function["section"])
        if section is None:
            raise RecompError(f"unknown section for {function['name']}")
        address, size, offset = function["vram"], function["size"], function["rom_offset"]
        if address % 4 or size <= 0 or size % 4 or offset % 4:
            raise RecompError(f"invalid function alignment/size: {function['name']}")
        if 'return_address_stack_offset' in function:
            slot=function['return_address_stack_offset']
            if type(slot) is not int or not 0<=slot<=0x7fff or slot%4:
                raise RecompError(f"invalid shared return stack slot: {function['name']}")
        if 'return_address_fpr' in function:
            reg = function['return_address_fpr']
            if type(reg) is not int or not 0 <= reg < 32 or 'return_address_stack_offset' in function:
                raise RecompError(f"invalid shared return FPR: {function['name']}")
        if 'return_address_register' in function:
            reg = function['return_address_register']
            if (type(reg) is not int or not 1 <= reg < 31 or
                    any(k in function for k in ('return_address_stack_offset', 'return_address_fpr'))):
                raise RecompError(f"invalid entry link register: {function['name']}")
        for site in function.get('tail_call_continuations', []):
            pc = site['pc']
            word = site['expected_word']
            op = word >> 26
            conditional = (op in (4, 5, 6, 7, 20, 21, 22, 23)
                           or op == 1 and (word >> 16 & 31) in (0, 1, 2, 3)
                           or op == 17 and (word >> 21 & 31) == 8)
            if (pc not in function['instruction_addresses'] or site['resume_pc'] not in function['instruction_addresses']
                    or site['expected_word'] != int.from_bytes(rom[offset+pc-address:offset+pc-address+4], 'big')
                    or not (op == 2 or conditional)):
                raise RecompError(f"invalid checked tail continuation: {function['name']}")
        for pc in function.get('nonlocal_return_calls', []):
            if type(pc) is not int or pc % 4 or not address <= pc < address + size:
                raise RecompError(f"invalid nonlocal return call: {function['name']}")
            word = int.from_bytes(rom[offset + pc - address:offset + pc - address + 4], 'big')
            if word >> 26 != 3 and word & 0xfc1fffff != 0x0000f809:
                raise RecompError(f"nonlocal return site is not a call: {function['name']}")
        for site in function.get('unwind_calls', []):
            pc = site['pc']
            word = site['expected_word']
            if (pc not in function['instruction_addresses']
                    or (word >> 26 != 3 and word & 0xfc1fffff != 0x0000f809)
                    or type(site.get('conditional', False)) is not bool
                    or int.from_bytes(rom[offset+pc-address:offset+pc-address+4], 'big') != site['expected_word']):
                raise RecompError(f"invalid checked unwind call: {function['name']}")
        if not (section["vram"] <= address and address + size <= section["vram"] + section["size"]):
            raise RecompError(f"function outside its section: {function['name']}")
        if offset != section["rom_offset"] + address - section["vram"] or offset + size > len(rom):
            raise RecompError(f"invalid ROM mapping: {function['name']}")
        key = (function["section"], address)
        if key in seen:
            raise RecompError(f"duplicate function address: {function['name']}")
        seen.add(key)
        raw = rom[offset:offset + size]
        if function["sha256"] != hashlib.sha256(raw).hexdigest():
            raise RecompError(f"function bytes do not match metadata: {function['name']}")
        words = function.get('instruction_addresses', list(range(address, address + size, 4)))
        if not words or words != sorted(set(words)) or address not in words:
            raise RecompError(f"invalid instruction address list: {function['name']}")
        for pc in words:
            if pc % 4 or not address <= pc < address + size:
                raise RecompError(f"instruction outside function: {function['name']}")
            key = (function['section'], pc)
            if key in claimed:
                raise RecompError('overlapping function ranges or instruction ownership')
            claimed.add(key)
    return metadata
