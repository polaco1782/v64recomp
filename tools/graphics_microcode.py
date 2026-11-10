"""Emit verified GLES2 graphics identities and static C11 RSP programs from a game's ROM-bound graphics profile."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

from common import RecompError, load_rom


def checked_span(name: str, span: dict, rom: bytes, images: dict[str, bytes] | None, limit: int) -> bytes:
    if 'image' in span:
        if not images or span['image'] not in images or 'rom_offset' in span:
            raise RecompError('unresolved GLES2 graphics image')
        source, offset = images[span['image']], span['offset']
    else:
        source, offset = rom, span['rom_offset']
    size = span['size']
    if (type(offset) is not int or type(size) is not int or offset < 0 or
            not 1 <= size <= limit or offset + size > len(source) or
            hashlib.sha256(source[offset:offset + size]).hexdigest() != span['sha256']):
        raise RecompError(f'GLES2 {name} ROM fingerprint mismatch')
    return source[offset:offset + size]


def generated_sources(profile: dict, output: Path) -> list[Path]:
    return [output / 'game_rsp.c'] if profile.get('graphics', {}).get('rsp') else []


def append_graphics_config(profile: dict, output: Path, images: dict[str, bytes] | None = None) -> None:
    """Append a checked static microcode table to the game's boot configuration.

    rom_offset refers to the original ROM. A compressed game's generation hook
    can supply verified named images; its profile then uses image and offset.
    Identifier strings alone never authorize native execution.
    """
    game = profile['game']
    rom, _ = load_rom(game['rom'], game['rom_sha1'], game['rom_size'])
    entries = profile['graphics']['microcodes']
    if not 1 <= len(entries) <= 32:
        raise RecompError('invalid GLES2 graphics profile count')
    lines = []
    rsp = profile['graphics'].get('rsp')
    if not rsp:
        raise RecompError('graphics profile requires a compiled RSP program')
    from rsp_recompile import emit_program
    sources = [(entry['name'], checked_span(entry['name'], entry, rom, images, 65536))
               for entry in rsp['images']]
    emit_program(output, sources)
    lines.append('extern const RecompRspProgram game_rsp_program;')
    lines.append('static const RecompGraphicsMicrocode game_graphics_microcodes[] = {')
    for entry in entries:
        for name in ('code', 'data'):
            checked_span(f'{entry["name"]} {name}', entry[name], rom, images, 8192)
        name = json.dumps(entry['name'], ensure_ascii=True)
        code, data = entry['code'], entry['data']
        native = ', .rsp = &game_rsp_program'
        lines.append(f'    {{.name = {name}, '
                     f'.code_size = 0x{code["size"]:x}, .data_size = 0x{data["size"]:x}, '
                     f'.code_sha256 = "{code["sha256"]}", .data_sha256 = "{data["sha256"]}"{native}}},')
    lines.append('};')
    sizes = profile['graphics'].get('framebuffers')
    if sizes is not None:
        if not 1 <= len(sizes) <= 16:
            raise RecompError('invalid GLES2 framebuffer inventory')
        lines.append('static const RecompFramebufferSize game_graphics_framebuffers[] = {')
        widths = set()
        for item in sizes:
            width, height = item['width'], item['height']
            if (type(width) is not int or type(height) is not int or not 1 <= width <= 1024 or
                    not 1 <= height <= 1024 or width in widths):
                raise RecompError('invalid or ambiguous GLES2 framebuffer extent')
            widths.add(width)
            lines.append(f'    {{{width}, {height}}},')
        lines.append('};')
    lines.append('')
    with (output / 'game_config.h').open('a') as stream:
        stream.write('\n' + '\n'.join(lines))
