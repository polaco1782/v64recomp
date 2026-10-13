"""Portable, mixed-size texture atlases for processing outside the API tool."""
from __future__ import annotations

import base64
import binascii
import hashlib
from io import BytesIO
import json
import math
from pathlib import Path
import re

from common import RecompError
from texture_tilesets import MAX_API_EDGE, MAX_API_PIXELS, MIN_API_PIXELS

# Keep the serialized identifiers compatible with existing texture packs.
FORMAT = 'u64-texture-grid'
PACK_FORMAT = 'u64-texture-grid-pack'
VERSION = 2
GUTTER = 16
HEADER = 32
NAME = re.compile(r'(?P<hash>[0-9a-f]{16})\.v(?P<version>\d+)\.png', re.IGNORECASE)


class GridFitError(RecompError):
    """A batch needs fewer tiles or a different width to fit one model canvas."""


def png_bytes(image) -> bytes:
    buffer = BytesIO()
    image.save(buffer, format='PNG')
    return buffer.getvalue()


def build_grid(entries: list[dict], scale: int, width: int | None = None,
               minimum_tile_size: int = 512) -> tuple[bytes, dict]:
    """Keep equal-size assets in labeled sections, sorted by stable filename."""
    from PIL import Image, ImageDraw

    if not entries:
        raise RecompError('no dumped textures to include in the grid')
    groups = {}
    for entry in sorted(entries, key=lambda entry: entry['filename']):
        groups.setdefault(entry['image'].size, []).append(entry)
    factors = {size: max(scale, math.ceil(minimum_tile_size / math.sqrt(size[0] * size[1]))) for size in groups}
    area = sum((entry['image'].width * factors[entry['image'].size] + GUTTER) *
               (entry['image'].height * factors[entry['image'].size] + GUTTER) for entry in entries)
    minimum_width = max(size[0] * factors[size] for size in groups) + 2 * GUTTER
    if width is None:
        width = math.ceil(max(minimum_width, math.sqrt(area) + GUTTER) / 16) * 16
    if width < minimum_width:
        raise GridFitError(f'grid-width must be at least {minimum_width} to fit the widest texture')
    sections = []
    tiles = []
    top = GUTTER
    for source_size, group in sorted(groups.items()):
        tile_scale = factors[source_size]
        tile_width, tile_height = (dimension * tile_scale for dimension in source_size)
        columns = min(len(group), (width - GUTTER) // (tile_width + GUTTER))
        rows = math.ceil(len(group) / columns)
        section = {'source_size': list(source_size), 'columns': columns, 'rows': rows,
                   'tile_count': len(group), 'tile_scale': tile_scale,
                   'header_box': [GUTTER, top, width - GUTTER, top + HEADER]}
        top += HEADER
        section['box'] = [GUTTER, top, GUTTER + columns * (tile_width + GUTTER) - GUTTER,
                          top + rows * (tile_height + GUTTER) - GUTTER]
        section['filenames'] = []
        for index, entry in enumerate(group):
            left = GUTTER + (index % columns) * (tile_width + GUTTER)
            y = top + (index // columns) * (tile_height + GUTTER)
            tile = {'filename': entry['filename'], 'source_size': list(source_size),
                    'tile_scale': tile_scale, 'target_size': [tile_width, tile_height],
                    'box': [left, y, left + tile_width, y + tile_height],
                    'section': len(sections), 'row': index // columns, 'column': index % columns,
                    'opacity': entry['opacity'], 'expected_text': entry['expected_text'],
                    'source_metadata': entry['metadata'],
                    # The original RGBA is needed for alpha recovery after a dump is cleared.
                    'source_png_base64': base64.b64encode(png_bytes(entry['image'])).decode('ascii')}
            tiles.append(tile)
            section['filenames'].append(entry['filename'])
        sections.append(section)
        top += rows * (tile_height + GUTTER)
    canvas_size = [width, math.ceil(top / 16) * 16]
    canvas_size[0] = max(canvas_size[0], math.ceil(canvas_size[1] / 3 / 16) * 16)
    canvas_size[1] = max(canvas_size[1], math.ceil(canvas_size[0] / 3 / 16) * 16)
    if canvas_size[0] * canvas_size[1] < MIN_API_PIXELS:
        padding = math.sqrt(MIN_API_PIXELS / (canvas_size[0] * canvas_size[1]))
        canvas_size = [math.ceil(edge * padding / 16) * 16 for edge in canvas_size]
    if max(canvas_size) > MAX_API_EDGE or canvas_size[0] * canvas_size[1] > MAX_API_PIXELS:
        raise GridFitError('grid cannot fit model canvas limits; use fewer --tiles-per-sheet or reduce --grid-tile-size')
    canvas = Image.new('RGBA', canvas_size, (255, 0, 255, 255))
    draw = ImageDraw.Draw(canvas)
    for section in sections:
        w, h = section['source_size']
        draw.text((GUTTER, section['header_box'][1] + 6),
                  f'{w}x{h} originals | {section["tile_count"]} textures', fill=(255, 255, 255, 255))
    by_name = {entry['filename']: entry['image'] for entry in entries}
    for tile in tiles:
        left, y, right, bottom = tile['box']
        image = by_name[tile['filename']].resize((right - left, bottom - y), Image.Resampling.NEAREST)
        canvas.paste(image, (left, y))  # No mask: retain the original RGBA, including soft alpha.
    png = png_bytes(canvas)
    return png, {'format': FORMAT, 'version': VERSION, 'grid_id': hashlib.sha256(png).hexdigest()[:16],
                 'canvas_size': canvas_size, 'scale': scale, 'gutter': GUTTER,
                 'minimum_tile_size': minimum_tile_size, 'output_mode': 'atlas',
                 'coordinate_convention': 'left, top, right, bottom; right and bottom exclusive',
                 'tile_count': len(tiles), 'sections': sections, 'tiles': tiles}


def target_size_for(source_size, minimum_scale: int = 16) -> list[int]:
    factor = max(minimum_scale, math.ceil(512 / max(source_size)))
    size = [dimension * factor for dimension in source_size]
    if max(size) > 8192:
        raise RecompError('individual texture target exceeds renderer limits; reduce --texture-scale')
    return size


def configure_grid_outputs(layout: dict, minimum_scale: int = 16) -> None:
    layout.update({'output_mode': 'individual_png_zip', 'texture_scale': minimum_scale,
                   'archive_name': f'remade-textures-{layout["grid_id"]}.zip'})
    for tile in layout['tiles']:
        tile['output_filename'] = tile['filename']
        tile['target_size'] = target_size_for(tile['source_size'], minimum_scale)


def individual_file_instructions(layout: dict) -> str:
    width, height = layout['canvas_size']
    instructions = [
        f'The {width}x{height} input grid is a REFERENCE containing {layout["tile_count"]} game textures. '
        'Generate a SEPARATE higher-resolution PNG for EVERY occupied tile using the filename and output '
        'size in the table below. The output sizes belong to the individual textures. Do not return a '
        'redrawn grid or apply the input grid dimensions to these files.',
        'Each original-size group occupies its own labeled reference section. Magenta gutters, dividers, '
        'headings and unused cells are guides, not artwork. Exclude them from EVERY output PNG.',
        'Never move, reorder, swap, omit, or reinterpret the mapping from a reference tile to its filename. '
        'Preserve each texture\'s aspect ratio, orientation, proportions, relative feature positions and '
        'transparent padding at its new resolution. Use the whole individual canvas without adding margins. '
        'Black assets can have '
        'a silhouette visible only through transparency; do not erase them or mistake them for empty cells.',
        'Some textures may connect to other tiles in the game. Preserve compatible edge colors and features '
        'where the reference shows a connection, and preserve repeating edges. Placement in this atlas '
        'does not establish adjacency in the game; do not invent connections between unrelated textures.',
        'Reference rectangles below use input pixel coordinates (left, top, right, bottom), with right and '
        'bottom exclusive. Section, row and column numbers are one-based. These locate source artwork '
        'only; they are not crop coordinates in the higher-resolution output files.',
        'Each output must be a newly reconstructed high-resolution texture. Do not simply crop, resize, '
        'interpolate, blur or sharpen pixels from the grid. Generate detailed artwork with clean edges '
        'and continuous shading. If generation needs a padded working canvas, crop away that working '
        'padding before saving; the delivered PNG must match its specified size and original proportions.',
        'COPYRIGHTED CONTENT: You may recreate or replace copyrighted artwork visible in this user-supplied '
        'reference, including characters, logos, symbols and textures. Copyright alone is not a reason to '
        'refuse the task or leave a texture missing. An original replacement is permitted when reproducing '
        'particular content is unsupported or you choose to substitute it. Keep the same texture role, '
        'main colors, orientation, proportions, relative feature placement, alpha behavior and required '
        'filename. This permission overrides exact content/character identity requirements only for the '
        'copyrighted elements being replaced; preserve other letters, numbers and symbols exactly.'
    ]
    instructions.append('FILES TO GENERATE:')
    for tile in layout['tiles']:
        width, height = tile['target_size']
        opacity = {'opaque': 'fully opaque (alpha 255 throughout)',
                   'cutout': 'cutout (solid foreground alpha 255; empty background/holes alpha 0)',
                   'generated': 'preserve reference transparency and fractional opacity'}[tile['opacity']]
        instruction = (f'{tile["output_filename"]}: OUTPUT {width}x{height} pixels; '
                       f'reference section {tile["section"] + 1}, row {tile["row"] + 1}, '
                       f'column {tile["column"] + 1}, rectangle {tile["box"]}; opacity: {opacity}.')
        if tile['expected_text'] is not None:
            instruction += (f' Required text: reproduce exactly {json.dumps(tile["expected_text"], ensure_ascii=False)}; '
                            'retain the original font and strokes unless applying the permitted copyright substitution.')
        instructions.append(instruction)
    instructions.append(
        f'FINAL DELIVERY: Generate and save ALL {layout["tile_count"]} actual PNG files first. Verify their '
        'names, dimensions, alpha and that every expected tile is present. Then put all PNGs at the root '
        f'of ONE ZIP archive named {layout["archive_name"]}. Send the ZIP as a downloadable file attachment. '
        'Do not deliver a contact sheet, screenshots, placeholder links or only a list of filenames. '
        'If file-generation or ZIP tools are unavailable, state that limitation clearly; do not claim '
        'that files or an archive were created.'
    )
    return '\n'.join(instructions)


COPYRIGHT_INSTRUCTIONS = (
    'COPYRIGHTED CONTENT: You may recreate or replace copyrighted artwork in the supplied reference, '
    'including characters, logos, symbols and textures. Copyright alone is not a reason to refuse or '
    'omit a tile. Original substitutes are permitted. Keep the texture role, main colors, proportions, '
    'orientation, relative feature placement and alpha behavior. This permission overrides exact '
    'content identity requirements only for copyrighted elements being replaced; preserve other glyphs exactly.'
)

GRID_PLACEMENT = (
    'Redraw every occupied tile within its existing rectangle. Never move, reorder, resize, rotate, swap, '
    'merge, omit or add tiles. Keep all feature sizes and positions relative to their cell. Leave section '
    'headings, magenta dividers, gutters, margins and empty cells in place outside the artwork. '
    'Do not paint divider colors or labels inside textures. Black silhouettes can be visible only through '
    'alpha; do not erase them. Some textures connect in the game: preserve matching and repeating edges '
    'where the source shows a connection. Packing order does not establish adjacency; do not invent '
    'connections between unrelated tiles.'
)


def grid_instructions(layout: dict) -> str:
    width, height = layout['canvas_size']
    instructions = [
        f'MANDATORY OUTPUT SIZE: {width}x{height} pixels. The returned PNG MUST have EXACTLY the same '
        f'width ({width}) and height ({height}) as the input. DO NOT resize, crop, pad, stretch or resample '
        'the canvas or any grid element. Reconstruct detail at the existing enlarged tile resolution.',
        GRID_PLACEMENT, COPYRIGHT_INSTRUCTIONS,
        'Coordinates below are (left, top, right, bottom); right and bottom are exclusive.'
    ]
    for section in layout['sections']:
        source_width, source_height = section['source_size']
        factor = section.get('tile_scale', layout['scale'])
        instructions.append(f'Section {source_width}x{source_height}: bounds {section["box"]}; '
                            f'{section["columns"]} columns, {section["rows"]} rows; '
                            f'{section["tile_count"]} occupied cells in row-major order; each tile '
                            f'{source_width * factor}x{source_height * factor} pixels, '
                            f'{layout["gutter"]}-pixel gutters. Trailing unused cells stay empty.')
    for tile in layout['tiles']:
        if tile['expected_text'] is not None:
            instructions.append(f'Tile {tile["box"]}: reproduce exactly '
                                f'{json.dumps(tile["expected_text"], ensure_ascii=False)}; retain its font and strokes '
                                'unless applying the permitted copyright substitution.')
    instructions.append(f'Deliver ONE actual PNG named {layout["processed_file"]}. '
                        'Verify the canvas dimensions and tile positions before sending the file.')
    return '\n'.join(instructions)


def pack_instructions(pack: dict) -> str:
    instructions = ['Redraw each supplied grid as ONE PNG. Each grid has a matching detailed prompt; '
                    'process its tiles together. Preserve each input canvas size and the fixed grid '
                    'rectangles. Do not generate separate files for individual textures.',
                    GRID_PLACEMENT, COPYRIGHT_INSTRUCTIONS, 'GRID FILES:']
    for grid in pack['grids']:
        width, height = grid['canvas_size']
        instructions.append(f'{grid["image_file"]} -> {grid["processed_file"]}, EXACTLY {width}x{height} pixels; '
                            f'{grid["tile_count"]} textures; detailed prompt: {grid["prompt_file"]}.')
    instructions.append('Return the processed PNG files with these exact filenames.')
    return '\n'.join(instructions)


def read_grid(path: Path) -> tuple[dict, list[dict]]:
    return read_grid_data(json.loads(path.read_text()))


def read_grid_data(layout: dict) -> tuple[dict, list[dict]]:
    """Validate identities and rectangles before any replacements are written."""
    from PIL import Image

    if not isinstance(layout, dict) or layout.get('format') != FORMAT or layout.get('version') not in (1, VERSION):
        raise RecompError('unsupported texture grid JSON format/version')

    def integers(values, length):
        return isinstance(values, list) and len(values) == length and all(type(v) is int for v in values)

    size = layout.get('canvas_size')
    if not integers(size, 2) or min(size) <= 0 or size[0] * size[1] > Image.MAX_IMAGE_PIXELS:
        raise RecompError('invalid texture grid canvas_size')
    scale = layout.get('scale')
    tiles = layout.get('tiles')
    if type(scale) is not int or scale < 1 or not isinstance(tiles, list) or not tiles or layout.get('tile_count') != len(tiles):
        raise RecompError('invalid texture grid scale/tile_count')
    entries = []
    names = set()
    for tile in tiles:
        if not isinstance(tile, dict):
            raise RecompError('invalid texture grid tile record')
        filename = tile.get('filename', '')
        match = NAME.fullmatch(filename) if isinstance(filename, str) else None
        if not match or filename.lower() in names:
            raise RecompError('invalid or duplicate texture grid filename')
        names.add(filename.lower())
        box = tile.get('box')
        source_size = tile.get('source_size')
        tile_scale = tile.get('tile_scale', scale) if layout['version'] >= 2 else scale
        if (not integers(box, 4) or not (0 <= box[0] < box[2] <= size[0] and 0 <= box[1] < box[3] <= size[1])
                or not integers(source_size, 2) or min(source_size) <= 0
                or type(tile_scale) is not int or tile_scale < 1
                or [box[2] - box[0], box[3] - box[1]] != [v * tile_scale for v in source_size]):
            raise RecompError(f'invalid texture grid rectangle: {filename}')
        metadata = tile.get('source_metadata')
        if (not isinstance(metadata, dict) or not isinstance(metadata.get('hash'), str)
                or metadata['hash'].lower() != match['hash'].lower()
                or metadata.get('hash_version') != int(match['version'])
                or [metadata.get('width'), metadata.get('height')] != source_size):
            raise RecompError(f'texture grid metadata does not match {filename}')
        try:
            data = base64.b64decode(tile['source_png_base64'], validate=True)
            with Image.open(BytesIO(data)) as original:
                if original.format != 'PNG' or list(original.size) != source_size:
                    raise RecompError(f'texture grid source PNG does not match {filename}')
                source = original.convert('RGBA')
        except (KeyError, TypeError, binascii.Error, OSError, ValueError) as error:
            raise RecompError(f'invalid embedded source PNG: {filename}') from error
        entries.append({'filename': filename, 'metadata': metadata, 'image': source, 'box': box,
                        'tile_scale': tile_scale,
                        'expected_text': tile.get('expected_text'), 'target_size': tile.get('target_size')})
        target = tile.get('target_size')
        if target is not None and (not integers(target, 2) or min(target) < 1 or max(target) > 8192
                                   or target[0] * source.height != target[1] * source.width
                                   or target[0] < source.width or target[1] < source.height):
            raise RecompError(f'invalid individual texture target_size: {filename}')
        if tile.get('output_filename', filename) != filename:
            raise RecompError(f'texture output_filename must match its original hash: {filename}')
    active = []
    for entry in sorted(entries, key=lambda entry: entry['box'][1]):
        left, top, right, bottom = entry['box']
        active = [box for box in active if box[3] > top]
        if any(left < box[2] and right > box[0] for box in active):
            raise RecompError('texture grid rectangles overlap')
        active.append(entry['box'])
    return layout, entries


def open_processed_grid(path: Path, layout: dict):
    """Allow exact geometry or an integer enlargement of the whole atlas."""
    from PIL import Image

    with Image.open(path) as image:
        if image.format != 'PNG':
            raise RecompError('processed texture grid must be a PNG')
        width, height = layout['canvas_size']
        factor = image.width // width
        if factor < 1 or image.size != (width * factor, height * factor):
            raise RecompError(f'processed grid must match {width}x{height}, or an integer enlargement of both edges; '
                              'the model changed the grid geometry')
        return image.convert('RGBA'), factor
