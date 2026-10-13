"""Fixed texture grids for image-edit requests and deterministic extraction."""
from __future__ import annotations

from io import BytesIO
import math

from common import RecompError

# https://developers.openai.com/api/docs/guides/image-prompting
MIN_API_PIXELS = 655_360
MAX_API_PIXELS = 8_294_400
MAX_API_EDGE = 3840


def clean_divider_edges(source, image) -> tuple[object, dict]:
    """Replicate adjacent artwork over a narrow gutter spill at cell edges."""
    from PIL import Image

    def magenta(r, g, b):
        return r > 160 and b > 150 and g < 120 and min(r, b) > g * 1.7

    source_rgb = source.convert('RGB').tobytes()
    # Magenta can be genuine artwork. Without a distinct guide color we cannot
    # safely infer which pixels came from the divider, so leave those assets alone.
    if any(magenta(r, g, b) for r, g, b in zip(source_rgb[::3], source_rgb[1::3], source_rgb[2::3])):
        return image, {}
    width, height = image.size
    rgb = image.convert('RGB').tobytes()
    mask = bytes(magenta(r, g, b) for r, g, b in zip(rgb[::3], rgb[1::3], rgb[2::3]))
    limit = max(2, min(width, height) // 24)

    def band(lines, span):
        counts = [sum(line) for line in lines]
        if not counts or counts[0] < span * .9:
            return 0
        count = 0
        for hits in counts:
            if not hits:
                break
            count += 1
        if count > limit:
            raise RecompError('a tileset divider covers too much of the tile; output was not saved')
        return count

    top = band([mask[y*width:(y+1)*width] for y in range(min(limit+1, height))], width)
    bottom = band([mask[y*width:(y+1)*width] for y in range(height-1, max(-1, height-limit-2), -1)], width)
    left = band([mask[x::width] for x in range(min(limit+1, width))], height)
    right = band([mask[x::width] for x in range(width-1, max(-1, width-limit-2), -1)], height)
    amounts = {side: size for side, size in zip(('left', 'top', 'right', 'bottom'),
                                              (left, top, right, bottom)) if size}
    if not amounts:
        return image, {}
    if left + right >= width or top + bottom >= height:
        raise RecompError('tileset dividers left no usable artwork; output was not saved')
    result = image.copy()
    if top:
        result.paste(image.crop((0, top, width, top+1)).resize((width, top), Image.Resampling.NEAREST), (0, 0))
    if bottom:
        result.paste(image.crop((0, height-bottom-1, width, height-bottom)).resize(
            (width, bottom), Image.Resampling.NEAREST), (0, height-bottom))
    if left:
        result.paste(result.crop((left, 0, left+1, height)).resize((left, height), Image.Resampling.NEAREST), (0, 0))
    if right:
        result.paste(result.crop((width-right-1, 0, width-right, height)).resize(
            (right, height), Image.Resampling.NEAREST), (width-right, 0))
    return result, amounts


def plan_tileset(count: int, source_size: tuple[int, int], minimum_scale: int,
                 model: str) -> dict:
    width, height = source_size
    # Leave a visible gutter outside every asset, including the outer border.
    gutter = max(1, min(width, height) // 8)
    legacy = model in ('gpt-image-1', 'gpt-image-1.5', 'gpt-image-1-mini')
    candidates = []
    for columns in range(1, count + 1):
        rows = math.ceil(count / columns)
        logical_w = columns * width + (columns + 1) * gutter
        logical_h = rows * height + (rows + 1) * gutter
        if legacy:
            canvases = [(w, h, min(w // logical_w, h // logical_h))
                        for w, h in ((1024, 1024), (1536, 1024), (1024, 1536))]
        else:
            canvases = []
            for factor in range(minimum_scale, MAX_API_EDGE // max(logical_w, logical_h) + 1):
                w = math.ceil(logical_w * factor / 16) * 16
                h = math.ceil(logical_h * factor / 16) * 16
                # A thin strip still fits: pad its short canvas edge while the
                # cell retains its original ratio and exact crop coordinates.
                w = max(w, math.ceil(h / 3 / 16) * 16)
                h = max(h, math.ceil(w / 3 / 16) * 16)
                if (1 / 3 <= w / h <= 3 and MIN_API_PIXELS <= w * h <= MAX_API_PIXELS
                        and max(w, h) <= MAX_API_EDGE):
                    canvases.append((w, h, factor))
                    break
        for canvas_w, canvas_h, factor in canvases:
            if factor < minimum_scale:
                continue
            needs_padding = not 1/3 <= logical_w/logical_h <= 3
            # Prefer balanced grids when possible. Padding supports genuine
            # thin strips without turning ordinary groups into long rows.
            score = (needs_padding, columns * rows - count, canvas_w * canvas_h,
                     canvas_w * canvas_h / (count * width * height * factor * factor),
                     abs(math.log(canvas_w / canvas_h)))
            candidates.append((score, columns, rows, factor, canvas_w, canvas_h))
    if not candidates:
        raise RecompError('tileset cannot fit the image API limits at this scale; reduce --scale or --tiles-per-sheet')
    _, columns, rows, factor, canvas_w, canvas_h = min(candidates)
    margin_x = (canvas_w - (columns * width + (columns + 1) * gutter) * factor) // 2
    margin_y = (canvas_h - (rows * height + (rows + 1) * gutter) * factor) // 2
    boxes = []
    for index in range(count):
        row, column = divmod(index, columns)
        left = margin_x + (gutter + column * (width + gutter)) * factor
        top = margin_y + (gutter + row * (height + gutter)) * factor
        boxes.append([left, top, left + width * factor, top + height * factor])
    return {'source_size': list(source_size), 'canvas_size': [canvas_w, canvas_h],
            'columns': columns, 'rows': rows, 'tile_scale': factor,
            'gutter': gutter * factor, 'boxes': boxes}


def tileset_reference(images: list, layout: dict) -> tuple[bytes, list[int], list[list[int]]]:
    from PIL import Image

    width, height = layout['source_size']
    # Keep the references readable, without making the target-size pixel grid
    # appear to be finished artwork. All positions scale with the whole sheet.
    readable_factor = min(layout['tile_scale'], max(1, 128 // max(width, height)))
    ratio = readable_factor / layout['tile_scale']
    reference_size = [math.ceil(edge * ratio) for edge in layout['canvas_size']]
    reference = Image.new('RGBA', reference_size, (255, 0, 255, 255))
    boxes = []
    for image, box in zip(images, layout['boxes'], strict=True):
        left, top = round(box[0] * ratio), round(box[1] * ratio)
        tile_size = (width * readable_factor, height * readable_factor)
        reference.paste(image.resize(tile_size, Image.Resampling.NEAREST), (left, top))
        boxes.append([left, top, left + tile_size[0], top + tile_size[1]])
    result = BytesIO()
    reference.save(result, format='PNG')
    return result.getvalue(), reference_size, boxes


def tileset_instructions(layout: dict, tiles: list[dict]) -> str:
    width, height = layout['canvas_size']
    return (f'TEXTURE TILESET: output exactly {width} by {height} pixels, with '
            f'{layout["columns"]} columns and {layout["rows"]} rows. The magenta divider '
            'lines and gutters are layout guides, never part of any game texture. '
            'Keep the grid geometry and divider positions at the specified output coordinates. '
            'Do not draw guide lines or their colored fringes inside tile artwork, including '
            'along the bottom and right tile edges; guides belong only in the gutters. '
            'Redraw each asset strictly inside its own tile rectangle. Do not move, swap, '
            'reorder, rotate, flip, merge or resize tiles, or move features into another cell. '
            'Keep empty cells and all gutters unchanged. No labels, captions or added symbols. '
            'Some tiles can be connected parts of a larger surface or object, even when they '
            'are not adjacent in this sheet. Preserve the original edge feature positions, '
            'orientation and UV alignment. Where the references visibly connect, recreate '
            'compatible colors, lighting, pattern scale and continuous edge details across '
            'those tiles. Packing order does not establish adjacency; do not invent connections '
            'between unrelated assets. Never assemble the tiles into one big scene. '
            'Read the low-resolution references and replace every occupied cell with a true '
            'high-resolution recreation at the same position. Coordinates below use the '
            'top-left origin, and right/bottom are exclusive. Cell assignments:\n'
            + '\n'.join(
                f'Row {index // layout["columns"] + 1}, column {index % layout["columns"] + 1}: '
                f'{tile["filename"]}; output rectangle {tile["box"]}; '
                f'reference rectangle {tile["reference_box"]}; '
                f'opacity {tile["opacity"]}'
                + (f'; REQUIRED TEXT: reproduce exactly {tile["expected_text"]!r}'
                   if tile.get('expected_text') is not None else '')
                for index, tile in enumerate(tiles)))


def extract_tileset(generated: bytes, layout: dict) -> list[bytes]:
    from PIL import Image

    with Image.open(BytesIO(generated)) as image:
        if image.format != 'PNG':
            raise RecompError('the image service did not return a PNG tileset')
        if list(image.size) != layout['canvas_size']:
            raise RecompError('generated tileset dimensions differ from its layout; no replacements were saved')
        image = image.convert('RGBA')
    result = []
    for box in layout['boxes']:
        buffer = BytesIO()
        image.crop(box).save(buffer, format='PNG')
        result.append(buffer.getvalue())
    return result
