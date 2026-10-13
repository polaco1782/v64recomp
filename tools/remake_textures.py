#!/usr/bin/env python3
"""Remake hash-named game textures through the image API or portable atlas files."""
from __future__ import annotations

import argparse
import base64
import hashlib
from io import BytesIO
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import sys
import urllib.error
import urllib.request
import unicodedata
import zipfile

from common import RecompError
from texture_alpha import recover_cutout_alpha, remove_background_key
from texture_grid import (FORMAT as GRID_FORMAT, PACK_FORMAT, GridFitError, build_grid,
                          grid_instructions, open_processed_grid, pack_instructions,
                          png_bytes, read_grid, read_grid_data, target_size_for)
from texture_tilesets import (MAX_API_EDGE, MAX_API_PIXELS, MIN_API_PIXELS,
                             clean_divider_edges, extract_tileset, plan_tileset,
                             tileset_instructions, tileset_reference)

NAME = re.compile(r'(?P<hash>[0-9a-f]{16})\.v(?P<version>\d+)\.png', re.IGNORECASE)
DEFAULT_MODEL = 'gpt-image-2.5-flare'
PROMPT_VERSION = 3


def pillow():
    try:
        from PIL import Image
    except ImportError as error:
        raise RecompError('install the optional dependency: .venv/bin/pip install -r requirements-textures.txt') from error
    return Image


def atomic_write(path: Path, data: bytes) -> None:
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_bytes(data)
    temporary.replace(path)


def read_source(path: Path) -> tuple[dict, object]:
    Image = pillow()
    match = NAME.fullmatch(path.name)
    if not match:
        raise RecompError(f'not a hash-named texture: {path.name}')
    metadata = json.loads(path.with_suffix('.json').read_text())
    with Image.open(path) as source:
        if source.format != 'PNG':
            raise RecompError(f'not a PNG: {path}')
        image = source.convert('RGBA')
    if (metadata.get('hash', '').lower() != match['hash'].lower()
            or metadata.get('hash_version') != int(match['version'])
            or (metadata.get('width'), metadata.get('height')) != image.size):
        raise RecompError(f'texture metadata does not match {path.name}')
    return metadata, image


def generation_size(width: int, height: int) -> str:
    # Request the smallest supported canvas with the exact texture proportions.
    ratio = width / height
    if not 1/3 <= ratio <= 3:
        raise RecompError('texture aspect ratio exceeds the image API range (1:3..3:1); select another texture')
    common = math.gcd(width, height)
    unit_w, unit_h = width // common, height // common
    multiplier = math.lcm(16 // math.gcd(unit_w, 16), 16 // math.gcd(unit_h, 16))
    factor = math.ceil(max(common, math.sqrt(MIN_API_PIXELS/(unit_w*unit_h)))/multiplier)*multiplier
    w, h = unit_w*factor, unit_h*factor
    if max(w,h) > MAX_API_EDGE or w*h > MAX_API_PIXELS:
        raise RecompError('texture aspect ratio cannot fit the API resolution limits exactly')
    return f'{w}x{h}'


def generation_canvas(width: int, height: int, model: str = DEFAULT_MODEL) -> tuple[str, tuple[int,int,int,int] | None]:
    if model in ('gpt-image-1','gpt-image-1.5','gpt-image-1-mini'):
        common=math.gcd(width,height);unit_w,unit_h=width//common,height//common
        candidates=[]
        for canvas_w,canvas_h in ((1024,1024),(1536,1024),(1024,1536)):
            factor=min(canvas_w//unit_w,canvas_h//unit_h)
            content_w,content_h=unit_w*factor,unit_h*factor
            if content_w<width or content_h<height:continue
            left,top=(canvas_w-content_w)//2,(canvas_h-content_h)//2
            crop=None if (content_w,content_h)==(canvas_w,canvas_h) else (left,top,left+content_w,top+content_h)
            candidates.append((canvas_w*canvas_h/(content_w*content_h),canvas_w*canvas_h,
                               f'{canvas_w}x{canvas_h}',crop))
        if not candidates:raise RecompError('texture cannot fit this model at the requested scale; reduce --scale')
        selected=min(candidates,key=lambda item:item[:2]);return selected[2],selected[3]
    if 1/3 <= width/height <= 3:
        return generation_size(width,height), None
    # Fit a narrow strip in the smallest square canvas. Its dimensions are
    # integer multiples of the original ratio, keeping both edges compact and
    # allowing a crop that preserves UV proportions exactly.
    common=math.gcd(width,height);unit_w,unit_h=width//common,height//common
    long_unit=max(unit_w,unit_h)
    step=16//math.gcd(long_unit,16)
    factor=math.ceil(max(common,math.sqrt(MIN_API_PIXELS)/long_unit)/step)*step
    edge=long_unit*factor
    if edge>MAX_API_EDGE or edge*edge>MAX_API_PIXELS:
        raise RecompError('texture cannot fit the API canvas at the requested scale; reduce --scale')
    canvas_w=canvas_h=edge
    content_w,content_h=unit_w*factor,unit_h*factor
    left,top=(canvas_w-content_w)//2,(canvas_h-content_h)//2
    return f'{canvas_w}x{canvas_h}',(left,top,left+content_w,top+content_h)


def reference_png(source, size: tuple[int,int], crop_box) -> bytes:
    if crop_box is None:
        # Make tiny glyphs readable without presenting the target-size pixel
        # grid as finished artwork. Keep larger original references intact.
        Image=pillow();output=BytesIO()
        readable_factor=max(1,128//max(source.size))
        requested_factor=max(1,min(size[0]//source.width,size[1]//source.height))
        factor=min(readable_factor,requested_factor)
        source.resize((source.width*factor,source.height*factor),Image.Resampling.NEAREST).save(output,format='PNG')
        return output.getvalue()
    Image=pillow();left,top,right,bottom=crop_box
    background=(128,128,128,255) if source.getchannel('A').getextrema()[0]==255 else (0,0,0,0)
    canvas=Image.new('RGBA',size,background)
    canvas.paste(source.resize((right-left,bottom-top),Image.Resampling.NEAREST),(left,top))
    output=BytesIO();canvas.save(output,format='PNG');return output.getvalue()


def make_prompt(image, target: tuple[int, int], extra: str, crop_box=None,
                expected_text: str | None = None, *, layout_instructions: str | None = None,
                transparency_instructions: str | None = None,
                task_description: str | None = None) -> str:
    alpha = image.getchannel('A').getextrema()
    foreground_opacity = ('Keep the source\'s fractional opacity where present; do not make translucent effects solid.'
                          if any(image.getchannel('A').histogram()[1:255])
                          else 'Keep the solid foreground fully opaque.')
    transparency = ('Reconstruct a clean silhouette and the original transparent holes with antialiased edges. '
                    'Empty background and holes must be real PNG transparency with alpha 0. '
                    f'{foreground_opacity} Never paint a checkerboard, white/gray matte or backdrop to represent '
                    'transparency. Keep approximately the original amount of empty background.'
                    if alpha[0] < 255 else 'The source is fully opaque; keep every pixel opaque.')
    asset_size=target if crop_box is None else (crop_box[2]-crop_box[0],crop_box[3]-crop_box[1])
    placement='Use the entire canvas; no borders, padding, perspective, mockup, new objects, or added text. '
    if crop_box is not None:
        left,top,right,bottom=crop_box
        placement=(f'The reference places a narrow texture inside a {target[0]} by {target[1]} padded canvas. '
                   f'Redraw only the rectangle from pixel ({left}, {top}) to ({right}, {bottom}), with the '
                   'right and bottom coordinates exclusive. Keep this rectangle in exactly the same location '
                   'and proportions. Preserve the padding outside it; the padding will be cropped away. '
                   'Do not stretch the artwork to fill the canvas. No perspective, mockup, new objects, or added text. ')
    if layout_instructions is not None:
        placement = layout_instructions + '\n'
    if transparency_instructions is not None:
        transparency = transparency_instructions
    literal=''
    if expected_text is not None:
        characters=', '.join(f'U+{ord(char):04X} {unicodedata.name(char,"UNNAMED CHARACTER")}' for char in expected_text)
        literal=(f'REQUIRED TEXT: reproduce exactly {json.dumps(expected_text,ensure_ascii=False)}. '
                 f'Characters in order: {characters or "none"}. Preserve their original font, case, colors, '
                 'outline and placement. Do not add, remove, replace or reinterpret any character. ')
    task = task_description or f'Redraw this game asset as new, fully reconstructed artwork at {asset_size[0]} by {asset_size[1]}.'
    return (f'{task} '
            'The input is a low-resolution reference, enlarged only to make it readable. Its square pixel '
            'blocks, stair-step edges and patchwork color cells are sampling artifacts that MUST be removed. '
            'Rebuild the underlying forms directly at output resolution: clean continuous contours, straight '
            'diagonals or smooth curves, crisp antialiased edges, continuous shading and newly drawn fine '
            'detail within the existing forms. Integrate blocky highlights into smooth lighting on those forms. '
            'The result must visibly contain higher-resolution artwork; copying or enlarging the reference '
            'pixel grid, tracing every pixel square, blurring it, or merely adding a glow is unacceptable. '
            'PRESERVE CONTENT: the same recognizable asset, game art style, main colors, orientation, '
            'overall proportions, feature locations, stroke structure, holes and spacing. These constraints '
            'apply to the intended design, not to individual source pixels or their jagged boundaries. '
            'Keep texture edges and repeated patterns aligned for existing UV coordinates. Add detail only '
            'within identified surfaces; do not invent objects, change the material or redesign the asset '
            'as a scene or a photorealistic object. Blank areas must remain blank. Resolve ambiguous detail '
            'conservatively while still redrawing its contours at high resolution. '
            'GLYPHS AND SYMBOLS: redraw every visible letter, number, punctuation mark and icon as clean '
            'continuous high-resolution strokes, preserving its exact identity. '
            'Keep the same character identity, case, count and order. Preserve strokes, crossbars, counters '
            'and negative space. Never replace a letter with punctuation, a warning sign or a pictogram. '
            'If the source contains A, keep its two diagonal legs, crossbar and interior hole; never turn '
            'it into an exclamation mark or a warning triangle. '
            f'{literal}{placement}Preserve continuity at opposite edges wherever the source repeats. '
            f'{transparency} Do not add halos, outer glows, background gradients or extra shadows. '
            f'Additional instructions must retain the same content and require a true high-resolution redraw. {extra}').strip()


def edit_image(source: bytes, prompt: str, *, key: str, model: str, size: str,
               quality: str, transparent: bool, timeout: float) -> tuple[bytes, dict]:
    # The edits endpoint accepts JSON image references, including data URLs.
    # https://developers.openai.com/api/reference/resources/images/methods/edit
    payload = {'model': model, 'images': [{'image_url': 'data:image/png;base64,' + base64.b64encode(source).decode('ascii')}],
               'prompt': prompt, 'n': 1, 'size': size, 'quality': quality,
               'output_format': 'png', 'background': 'transparent' if transparent else 'opaque'}
    if model in ('gpt-image-1', 'gpt-image-1.5'):
        payload['input_fidelity'] = 'high'
    request = urllib.request.Request('https://api.openai.com/v1/images/edits',
        data=json.dumps(payload).encode(), headers={'Authorization': f'Bearer {key}', 'Content-Type': 'application/json'}, method='POST')
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            result = json.load(response)
    except urllib.error.HTTPError as error:
        detail = error.read().decode('utf-8', errors='replace')[:2000]
        raise RecompError(f'OpenAI image edit failed ({error.code}): {detail}') from error
    except (urllib.error.URLError, TimeoutError) as error:
        # Do not automatically retry a potentially billed request.
        raise RecompError(f'OpenAI request failed; inspect the result before retrying: {error}') from error
    try:
        data = base64.b64decode(result['data'][0]['b64_json'], validate=True)
    except (KeyError, IndexError, TypeError, ValueError) as error:
        raise RecompError('OpenAI response did not contain a base64 image') from error
    return data, result.get('usage', {})


def resolve_alpha_mode(source, alpha_mode: str) -> str:
    if alpha_mode != 'auto':
        return alpha_mode
    histogram = source.getchannel('A').histogram()
    if source.getchannel('A').getextrema()[0] == 255:
        return 'opaque'
    if histogram[0] and histogram[255] and not any(histogram[1:255]):
        return 'cutout'
    return 'generated'


def finish_image(source, generated: bytes, target: tuple[int, int], alpha_mode: str,
                 resize_to_scale: bool = False, crop_box=None, canvas_size=None, *,
                 background_key: str | None = None, background_tolerance: int = 32,
                 tileset: bool = False, processing: dict | None = None) -> bytes:
    Image = pillow()
    automatic_alpha = alpha_mode == 'auto'
    alpha_mode = resolve_alpha_mode(source, alpha_mode)
    with Image.open(BytesIO(generated)) as raw:
        if raw.format != 'PNG':
            raise RecompError('the image service did not return a PNG')
        result = raw.convert('RGBA')
    if crop_box is not None:
        if canvas_size is not None and result.size != canvas_size:
            raise RecompError('generated canvas dimensions differ from the padded reference; output was not saved')
        left,top,right,bottom=crop_box
        if not (0<=left<right<=result.width and 0<=top<bottom<=result.height):
            raise RecompError('invalid texture crop rectangle; output was not saved')
        result=result.crop(crop_box)
    if result.width * target[1] != result.height * target[0]:
        raise RecompError(f'generated aspect ratio {result.size} differs from source {source.size}; output was not saved')
    if result.width < target[0] or result.height < target[1]:
        raise RecompError('generated image is smaller than the requested replacement; output was not saved')
    if max(result.size) > 8192:
        raise RecompError('generated image exceeds renderer limits; output was not saved')
    divider_cleanup = {}
    if tileset:
        result, divider_cleanup = clean_divider_edges(source, result)
    if background_key is not None:
        result = remove_background_key(result, background_key, background_tolerance)
    alpha_recovery = {}
    if automatic_alpha and alpha_mode == 'cutout' and background_key is None:
        result, alpha_recovery = recover_cutout_alpha(source, result)
    if processing is not None:
        processing.update({'background_key': background_key,
                           'background_tolerance': background_tolerance if background_key else None,
                           'divider_cleanup': divider_cleanup, 'alpha_recovery': alpha_recovery})
    if (alpha_mode in ('generated', 'cutout') and source.getchannel('A').getextrema()[0] < 255
            and result.getchannel('A').getextrema()[0] == 255):
        raise RecompError('the model lost source transparency; use --alpha source or retry')
    if (alpha_mode == 'generated' and not resize_to_scale and crop_box is None
            and background_key is None and not divider_cleanup):
        # Preserve the image service result exactly, including detailed alpha.
        return generated
    if alpha_mode == 'source':
        # Resample RGB independently when discarding the generated alpha.
        result = result.convert('RGB')
    if resize_to_scale and result.size != target:
        result = result.resize(target, Image.Resampling.LANCZOS)
    result = result.convert('RGBA')
    if alpha_mode == 'source':
        result.putalpha(source.getchannel('A').resize(result.size, Image.Resampling.NEAREST))
    elif alpha_mode == 'opaque':
        result.putalpha(255)
    elif alpha_mode == 'cutout':
        # Copy-cycle HUD draws can test against alpha 1.0. AI outputs often
        # peak at 254 (or lower), making every pixel fail that test. Match the
        # source's one-bit opacity using the AI silhouette, never its old mask.
        alpha = result.getchannel('A')
        peak = alpha.getextrema()[1]
        if peak == 0:
            raise RecompError('the model returned an empty cutout; output was not saved')
        threshold = (peak + 1) // 2
        result.putalpha(alpha.point([0 if value < threshold else 255 for value in range(256)]))
        source_alpha = source.getchannel('A').histogram()
        output_alpha = result.getchannel('A').histogram()
        if output_alpha[0] / (result.width * result.height) < source_alpha[0] / (source.width * source.height) / 4:
            raise RecompError('the model painted over most transparent background; output was not saved. '
                              'Use --reuse-generated with --background-key for a known matte, or --alpha source')
    output = BytesIO(); result.save(output, format='PNG')
    return output.getvalue()


def expected_text_for(args, metadata: dict) -> str | None:
    expected = args.text if args.text is not None else metadata.get('expected_text')
    if expected is not None and (not isinstance(expected, str) or len(expected) > 256):
        raise RecompError('expected_text must be a string of at most 256 characters')
    return expected


def skipped_path(output: Path, texture: Path) -> Path:
    return output / 'skipped' / texture.with_suffix('.json').name


def skip_texture(args, path: Path, reason, previous: dict) -> None:
    record = {'texture': path.name, 'reason': str(reason),
              **{key: previous[key] for key in ('model', 'prompt_version', 'tileset_id', 'grid_id') if key in previous}}
    if not args.dry_run:
        destination = skipped_path(args.output, path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(destination, (json.dumps(record, indent=2) + '\n').encode())
    print(f'Skipped {path.name}: {reason}', file=sys.stderr, flush=True)


def save_replacement(args, path: Path, metadata: dict, source, output: bytes, previous: dict,
                     processing: dict | None = None) -> None:
    Image = pillow()
    with Image.open(BytesIO(output)) as image:
        width, height = image.size
        transparent = image.convert('RGBA').getchannel('A').getextrema()[0] < 255
    history_keys = ('model', 'quality', 'prompt', 'usage', 'api_size', 'crop_box', 'prompt_version',
                    'expected_text', 'reference_size', 'tileset_id', 'tileset_box',
                    'tileset_tile_count', 'usage_scope', 'grid_id', 'grid_box', 'grid_json',
                    'generation_source', 'processed_grid', 'zip_archive', 'zip_member',
                    'grid_reference_box', 'target_size')
    manifest = {**metadata, 'source_width': source.width, 'source_height': source.height,
                **{key: previous[key] for key in history_keys if key in previous},
                'width': width, 'height': height, 'transparent': transparent,
                'alpha_mode': args.alpha, 'alpha_processing': resolve_alpha_mode(source, args.alpha),
                **(processing or {}),
                'requested_scale': args.scale, 'resize_to_scale': args.resize_to_scale}
    atomic_write((args.output / path.name).with_suffix('.json'), (json.dumps(manifest, indent=2) + '\n').encode())
    atomic_write(args.output / path.name, output)
    skipped_path(args.output, path).unlink(missing_ok=True)
    if processing and processing.get('alpha_recovery'):
        print(f'Recovered alpha for {path.name}: {processing["alpha_recovery"]["method"]}', flush=True)
    print(f'Saved {args.output/path.name} ({width}x{height}, alpha: {manifest["alpha_processing"]})', flush=True)


def remake_tilesets(args, sources: list[Path], extra: str, key: str) -> int:
    saved_count = skipped_count = 0
    groups = {}
    for path in sources[:args.limit]:
        metadata, image = read_source(path)
        expected = expected_text_for(args, metadata)
        groups.setdefault(image.size, []).append((path, metadata, image, expected))
    for source_size, entries in groups.items():
        remaining = entries
        while remaining:
            count = min(args.tiles_per_sheet, len(remaining))
            while True:
                try:
                    layout = plan_tileset(count, source_size, args.scale, args.model)
                    break
                except RecompError:
                    if count == 1:
                        raise
                    count = max(1, count // 2)
            batch, remaining = remaining[:count], remaining[count:]
            reference, reference_size, reference_boxes = tileset_reference([entry[2] for entry in batch], layout)
            tiles = [{'filename': entry[0].name, 'box': box, 'reference_box': reference_box,
                      'opacity': resolve_alpha_mode(entry[2], 'auto'), 'expected_text': entry[3]}
                     for entry, box, reference_box in zip(batch, layout['boxes'], reference_boxes, strict=True)]
            size = 'x'.join(map(str, layout['canvas_size']))
            prompt = make_prompt(batch[0][2], tuple(layout['canvas_size']), extra,
                                 layout_instructions=tileset_instructions(layout, tiles),
                                 transparency_instructions=(
                                     'Treat opacity separately for each cell: opaque cells must remain fully opaque; '
                                     'cutout cells need a transparent background, their original holes and a solid '
                                     'opaque interior with clean edges. Empty cutout backgrounds and holes must '
                                     'have actual PNG alpha 0; never paint a checkerboard, white/gray matte or '
                                     'backdrop to simulate transparency. Preserve approximately the original '
                                     'amount of empty background. Generated cells retain the source\'s soft '
                                     'transparency. Never fill transparent holes with the divider color. '
                                     'Keep the magenta gutters intact and outside the artwork.'))
            if len(prompt) > 32_000:
                raise RecompError('tileset prompt exceeds the API limit; reduce --tiles-per-sheet or the extra instructions')
            fingerprint = hashlib.sha256(reference + json.dumps(
                {'layout': layout, 'model': args.model, 'quality': args.quality, 'prompt': prompt},
                sort_keys=True).encode()).hexdigest()[:16]
            identifier = f'{source_size[0]}x{source_size[1]}-{fingerprint}'
            cache = args.output / 'tilesets'
            cache.mkdir(parents=True, exist_ok=True)
            reference_path = cache / f'{identifier}.reference.png'
            generated_path = cache / f'{identifier}.generated.png'
            manifest_path = cache / f'{identifier}.json'
            history = {'model': args.model, 'quality': args.quality, 'prompt': prompt,
                       'api_size': size, 'crop_box': None, 'prompt_version': PROMPT_VERSION,
                       'reference_size': reference_size, 'tileset_id': identifier,
                       'tileset_tile_count': count, 'usage_scope': 'tileset',
                       'layout': layout, 'tiles': tiles}
            atomic_write(reference_path, reference)
            if args.dry_run:
                # A dry run can update a preview, but must retain cached usage
                # and provenance if this exact sheet was already generated.
                if not generated_path.exists():
                    atomic_write(manifest_path, (json.dumps(history, indent=2) + '\n').encode())
                print(json.dumps({'tileset': identifier, 'reference': str(reference_path),
                                  'api_size': size, 'tiles': tiles, 'prompt': prompt}, indent=2))
                continue
            if generated_path.is_file():
                print(f'Reusing cached tileset {generated_path}', flush=True)
                history = json.loads(manifest_path.read_text())
                generated = generated_path.read_bytes()
            else:
                print(f'Redrawing tileset {identifier}: {count} textures, {size} ({args.model})', flush=True)
                # Persist the layout before upload, and the raw response before
                # validation, so failed extraction never requires another call.
                atomic_write(manifest_path, (json.dumps(history, indent=2) + '\n').encode())
                if not key:
                    raise RecompError('set OPENAI_API_KEY in your environment, or use --dry-run')
                generated, usage = edit_image(reference, prompt, key=key, model=args.model, size=size,
                                              quality=args.quality, transparent=any(
                                                  entry[2].getchannel('A').getextrema()[0] < 255
                                                  for entry in batch), timeout=args.timeout)
                atomic_write(generated_path, generated)
                history['usage'] = usage
                atomic_write(manifest_path, (json.dumps(history, indent=2) + '\n').encode())
            try:
                extracted = extract_tileset(generated, layout)
            except (RecompError, OSError, ValueError) as error:
                for entry in batch:
                    skip_texture(args, entry[0], error, history)
                    skipped_count += 1
                continue
            raw_directory = args.output / 'generated'
            raw_directory.mkdir(parents=True, exist_ok=True)
            per_tile_history = []
            for entry, raw, tile in zip(batch, extracted, tiles, strict=True):
                previous = {**{key: value for key, value in history.items() if key not in ('tiles', 'layout')},
                            'tileset_box': tile['box'], 'expected_text': entry[3]}
                raw_path = raw_directory / entry[0].name
                atomic_write(raw_path, raw)
                atomic_write(raw_path.with_suffix('.json'), (json.dumps(previous, indent=2) + '\n').encode())
                per_tile_history.append(previous)
            # A bad model result affects only that cell; publish valid crops
            # and continue processing later cells, sheets and size groups.
            for entry, raw, previous in zip(batch, extracted, per_tile_history, strict=True):
                processing = {}
                try:
                    output = finish_image(entry[2], raw, (entry[2].width * args.scale, entry[2].height * args.scale),
                                          args.alpha, args.resize_to_scale, background_key=args.background_key,
                                          background_tolerance=args.background_tolerance,
                                          tileset=True, processing=processing)
                except (RecompError, OSError, ValueError) as error:
                    skip_texture(args, entry[0], error, previous)
                    skipped_count += 1
                    continue
                save_replacement(args, entry[0], entry[1], entry[2], output, previous, processing)
                saved_count += 1
    if not args.dry_run:
        print(f'Finished: {saved_count} textures saved, {skipped_count} invalid AI results skipped.', flush=True)
    return 0


def make_grid_prompt(layout: dict, source, extra: str = '') -> str:
    transparency = (
        'Handle opacity separately in EVERY tile: opaque originals must remain fully opaque; '
        'cutout sprites need fully opaque foreground and real alpha 0 in empty background and holes; '
        'translucent effects must retain fractional opacity. Use the original RGBA reference for each tile. '
        'Do not paint checkerboards, white/gray backgrounds or matte colors to simulate transparency. '
        'Preserve approximately the original amount of empty space in each sprite.'
    )
    if layout['format'] == PACK_FORMAT:
        instructions = pack_instructions(layout)
        size = layout['grids'][0]['canvas_size']
        task = f'Redraw the supplied texture grids as {len(layout["grids"])} PNGs, one complete image per grid.'
    else:
        instructions = grid_instructions(layout)
        size = layout['canvas_size']
        task = (f'Redraw this entire texture grid as ONE PNG named {layout["processed_file"]}, '
                f'EXACTLY {size[0]}x{size[1]} pixels. Reconstruct all {layout["tile_count"]} textures '
                'together inside their fixed enlarged rectangles.')
    return instructions + '\n\n' + make_prompt(
        source, tuple(size), extra, layout_instructions=instructions,
        transparency_instructions=transparency, task_description=task)


def dump_grid(args, sources: list[Path], extra: str) -> int:
    destination = (args.input.parent/'grids/all-textures.png' if args.dump_grid is True else args.dump_grid)
    if destination.suffix.lower() != '.png':
        raise RecompError('--dump-grid destination must have a .png extension')
    if any(destination.resolve() == source.resolve() for source in sources):
        raise RecompError('grid destination must not overwrite an original texture')
    entries = []
    for path in sources:
        metadata, image = read_source(path)
        entries.append({'filename': path.name, 'metadata': metadata, 'image': image,
                        'opacity': resolve_alpha_mode(image, 'auto'),
                        'expected_text': expected_text_for(args, metadata)})
    if not entries:
        raise RecompError('no dumped textures to include in the grids')
    entries.sort(key=lambda entry: (*entry['image'].size, entry['filename']))
    batches = []
    offset = 0
    while offset < len(entries):
        count = min(args.tiles_per_sheet, len(entries) - offset)
        while True:
            try:
                data, layout = build_grid(entries[offset:offset + count], args.scale, args.grid_width,
                                          args.grid_tile_size)
                break
            except GridFitError:
                if count == 1:
                    raise
                count -= 1
        batches.append((data, layout))
        offset += count
    multiple = len(batches) > 1
    for index, (data, layout) in enumerate(batches, 1):
        path = destination.with_name(f'{destination.stem}.{index:03d}.png') if multiple else destination
        layout.update({'image_file': path.name, 'prompt_file': path.with_suffix('.prompt.txt').name,
                       'processed_file': f'{path.stem}.processed.png', 'prompt_version': PROMPT_VERSION,
                       'grid_task_version': 3, 'extra_instructions': extra})
        layout['prompt'] = make_grid_prompt(layout, entries[0]['image'], extra)
    if multiple:
        pack = {'format': PACK_FORMAT, 'version': 1, 'tile_count': len(entries),
                'minimum_tile_size': args.grid_tile_size, 'grids': [layout for _, layout in batches]}
        pack['prompt'] = make_grid_prompt(pack, entries[0]['image'], extra)
    else:
        pack = batches[0][1]
    if args.dry_run:
        print(json.dumps({'manifest': str(destination.with_suffix('.json')), 'tile_count': len(entries),
                          'grids': [{'image': layout['image_file'], 'canvas_size': layout['canvas_size'],
                                     'tile_count': layout['tile_count']} for _, layout in batches],
                          'prompt': pack['prompt']}, indent=2))
        return 0
    destination.parent.mkdir(parents=True, exist_ok=True)
    previous_path = destination.with_suffix('.json')
    if multiple and previous_path.is_file() and destination.is_file():
        previous = json.loads(previous_path.read_text())
        if previous.get('format') == GRID_FORMAT and previous.get('grid_id') == hashlib.sha256(destination.read_bytes()).hexdigest()[:16]:
            archive = destination.parent/'previous'/previous['grid_id']
            archive.mkdir(parents=True, exist_ok=True)
            for old in (destination, previous_path, destination.with_suffix('.prompt.txt')):
                if old.is_file():
                    old.replace(archive/old.name)
    for data, layout in batches:
        path = destination.parent/layout['image_file']
        atomic_write(path.with_suffix('.json'), (json.dumps(layout, indent=2) + '\n').encode())
        atomic_write(path, data)
        atomic_write(path.with_suffix('.prompt.txt'), (layout['prompt'] + '\n').encode())
        print(f'Exported {path}: {layout["tile_count"]} textures, '
              f'{layout["canvas_size"][0]}x{layout["canvas_size"][1]}', flush=True)
    if multiple:
        atomic_write(destination.with_suffix('.json'), (json.dumps(pack, indent=2) + '\n').encode())
        atomic_write(destination.with_suffix('.prompt.txt'), (pack['prompt'] + '\n').encode())
    print(f'Exported ALL {len(entries)} dumped textures in {len(batches)} grids. '
          f'Manifest: {destination.with_suffix(".json")}', flush=True)
    return 0


def import_grid(args) -> int:
    pillow()
    document = json.loads(args.grid_json.read_text())
    if isinstance(document, dict) and document.get('format') == PACK_FORMAT:
        return import_grid_pack(args, document)
    layout, entries = read_grid_data(document)
    return import_grid_sheet(args, args.import_grid, layout, entries)


def import_grid_sheet(args, path: Path, layout: dict, entries: list[dict]) -> int:
    processed, factor = open_processed_grid(path, layout)
    if args.dry_run:
        print(json.dumps({'processed_grid': str(path), 'grid_json': str(args.grid_json),
                          'tile_count': len(entries), 'canvas_size': processed.size,
                          'enlargement': factor, 'output': str(args.output)}, indent=2))
        return 0
    args.output.mkdir(parents=True, exist_ok=True)
    cache = args.output/'generated'
    cache.mkdir(parents=True, exist_ok=True)
    saved_count = skipped_count = 0
    for entry in entries:
        texture = Path(entry['filename'])
        box = [value * factor for value in entry['box']]
        args.scale = entry['tile_scale']
        raw = png_bytes(processed.crop(box))
        previous = {'generation_source': 'external_grid', 'grid_id': layout.get('grid_id'),
                    'grid_box': box, 'grid_json': str(args.grid_json.resolve()),
                    'processed_grid': str(path.resolve()),
                    'prompt': layout.get('prompt'), 'prompt_version': layout.get('prompt_version'),
                    'expected_text': entry['expected_text']}
        atomic_write(cache/texture.name, raw)
        atomic_write((cache/texture.name).with_suffix('.json'), (json.dumps(previous, indent=2) + '\n').encode())
        source = entry['image']
        processing = {}
        try:
            output = finish_image(source, raw, (source.width * args.scale, source.height * args.scale),
                                  args.alpha, args.resize_to_scale, tileset=True, processing=processing)
        except (RecompError, OSError, ValueError) as error:
            skip_texture(args, texture, error, previous)
            skipped_count += 1
            continue
        save_replacement(args, texture, entry['metadata'], source, output, previous, processing)
        saved_count += 1
    print(f'Finished: {saved_count} textures saved, {skipped_count} invalid results skipped (local grid import).', flush=True)
    return 0


def import_grid_pack(args, pack: dict) -> int:
    if pack.get('version') != 1 or not isinstance(pack.get('grids'), list) or not pack['grids']:
        raise RecompError('invalid grid pack manifest')
    parsed = [read_grid_data(grid) for grid in pack['grids']]
    filenames = [entry['filename'].lower() for _, entries in parsed for entry in entries]
    if len(filenames) != len(set(filenames)) or pack.get('tile_count') != len(filenames):
        raise RecompError('grid pack has duplicate textures or an invalid tile_count')
    for layout, entries in parsed:
        name = layout.get('processed_file')
        if not isinstance(name, str) or Path(name).name != name or not name.endswith('.png'):
            raise RecompError('invalid processed filename in grid pack')
    if args.import_grid.is_dir():
        jobs = [(args.import_grid/layout['processed_file'], layout, entries) for layout, entries in parsed]
    else:
        jobs = [(args.import_grid, layout, entries) for layout, entries in parsed
                if args.import_grid.name == layout['processed_file']]
        if not jobs:
            raise RecompError('processed PNG name does not match the master manifest; use its per-grid JSON or required filename')
    completed = missing = rejected = 0
    for path, layout, entries in jobs:
        if not path.is_file():
            print(f'Missing processed grid {path.name}; replacements preserved.', file=sys.stderr, flush=True)
            missing += 1
            continue
        try:
            # Reject a changed canvas before writing any members of this sheet.
            checked, _ = open_processed_grid(path, layout)
            checked.close()
        except (RecompError, OSError, ValueError) as error:
            if isinstance(error, OSError) and error.errno is not None:
                raise
            for entry in entries:
                skip_texture(args, Path(entry['filename']), error, {'grid_id': layout['grid_id']})
            rejected += 1
            continue
        import_grid_sheet(args, path, layout, entries)
        completed += 1
    print(f'Grid pack: {completed} grids processed, {rejected} invalid grids skipped, {missing} missing.', flush=True)
    return 0


def import_zip(args) -> int:
    pillow()
    layout, entries = read_grid(args.grid_json)
    expected = {entry['filename'].lower(): entry for entry in entries}
    saved_count = skipped_count = missing_count = 0
    try:
        archive = zipfile.ZipFile(args.import_zip)
    except zipfile.BadZipFile as error:
        raise RecompError(f'invalid texture ZIP: {error}') from error
    with archive:
        members = {}
        ignored = 0
        for member in archive.infolist():
            path = PurePosixPath(member.filename)
            if path.is_absolute() or '..' in path.parts or '\\' in member.filename:
                raise RecompError(f'unsafe ZIP member path: {member.filename}')
            if member.is_dir() or path.suffix.lower() != '.png':
                continue
            name = path.name.lower()
            if name not in expected:
                ignored += 1
                continue
            if name in members:
                raise RecompError(f'duplicate texture in ZIP: {path.name}')
            members[name] = member
        if not members:
            raise RecompError('ZIP contains no PNGs matching the grid JSON; use the filenames in the model prompt')
        if ignored:
            print(f'Ignored {ignored} PNGs not listed in the grid JSON.', flush=True)
        for entry in entries:
            path = Path(entry['filename'])
            member = members.get(path.name.lower())
            if member is None:
                print(f'Missing {path.name} in ZIP; existing replacement preserved.', file=sys.stderr, flush=True)
                missing_count += 1
                continue
            source = entry['image']
            target = entry['target_size'] or target_size_for(source.size, layout.get('texture_scale', 16))
            args.scale = target[0] / source.width
            previous = {'generation_source': 'external_zip', 'grid_id': layout.get('grid_id'),
                        'grid_reference_box': entry['box'], 'grid_json': str(args.grid_json.resolve()),
                        'zip_archive': str(args.import_zip.resolve()), 'zip_member': member.filename,
                        'target_size': target, 'prompt': layout.get('prompt'),
                        'prompt_version': layout.get('prompt_version'), 'expected_text': entry['expected_text']}
            try:
                if member.file_size > 8192 * 8192 * 4 + 1_048_576:
                    raise RecompError('texture PNG exceeds renderer file size limits')
                raw = archive.read(member)
            except (zipfile.BadZipFile, RuntimeError, RecompError) as error:
                skip_texture(args, path, error, previous)
                skipped_count += 1
                continue
            if not args.dry_run:
                cache = args.output/'generated'
                cache.mkdir(parents=True, exist_ok=True)
                atomic_write(cache/path.name, raw)
                atomic_write((cache/path.name).with_suffix('.json'), (json.dumps(previous, indent=2) + '\n').encode())
            processing = {}
            try:
                output = finish_image(source, raw, tuple(target), args.alpha, args.resize_to_scale,
                                      tileset=True, processing=processing)
            except (RecompError, OSError, ValueError) as error:
                skip_texture(args, path, error, previous)
                skipped_count += 1
                continue
            if args.dry_run:
                print(f'Validated {path.name}: minimum {target[0]}x{target[1]}.', flush=True)
            else:
                save_replacement(args, path, entry['metadata'], source, output, previous, processing)
            saved_count += 1
    verb = 'validated' if args.dry_run else 'saved'
    print(f'Finished: {saved_count} textures {verb}, {skipped_count} invalid results skipped, '
          f'{missing_count} missing (local ZIP import).', flush=True)
    return 0


def main(argv: list[str] | None = None, *, default_root: Path | None = None) -> int:
    project = Path(__file__).resolve().parents[1]
    root = default_root or project/'local/textures'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, default=root/'dump')
    parser.add_argument('--output', type=Path, default=root/'replacements')
    grid_mode = parser.add_mutually_exclusive_group()
    grid_mode.add_argument('--dump-grid', type=Path, nargs='?', const=True, metavar='PNG',
                           help='export ALL originals into grids of up to 16 enlarged tiles, with JSONs and prompts; optional base PNG path')
    grid_mode.add_argument('--import-grid', type=Path, metavar='PATH',
                           help='import one processed PNG or a directory of processed grids using a master manifest')
    grid_mode.add_argument('--import-zip', type=Path, metavar='ZIP',
                           help='locally import individually generated, hash-named texture PNGs from a ZIP')
    parser.add_argument('--grid-json', type=Path, help='manifest produced by --dump-grid; required for grid or ZIP import')
    parser.add_argument('--grid-width', type=int, help='export atlas width in pixels (default: chosen from all tile sizes)')
    parser.add_argument('--grid-tile-size', type=int, default=512, help='minimum tile area equivalent to SIZE x SIZE pixels, preserving aspect ratio (default: 512)')
    parser.add_argument('--model', default=DEFAULT_MODEL)
    parser.add_argument('--quality', choices=('low','medium','high','xhigh','max'), default='high')
    parser.add_argument('--scale', type=int, default=4, help='minimum API replacement size or export tile scale (default: 4); grid imports use the saved layout scale')
    parser.add_argument('--resize-to-scale', action='store_true', help='explicitly reduce the AI result to the source dimensions times --scale')
    parser.add_argument('--limit', type=int, help='maximum textures to remake (default: 1, or 16 with --tileset)')
    parser.add_argument('--tileset', action='store_true', help='redraw equal-size textures together in divided grids')
    parser.add_argument('--tiles-per-sheet', type=int, default=16, help='maximum textures per exported grid or tileset request (default: 16)')
    parser.add_argument('--tile-size', help='only process textures with these original dimensions, e.g. 32x32')
    parser.add_argument('--texture', help='select one full hash filename or its 16-digit hash')
    parser.add_argument('--prompt-file', type=Path, help='append material/style instructions from a text file')
    parser.add_argument('--text', help='literal characters expected in the selected texture; requires --texture')
    parser.add_argument('--alpha', choices=('auto','source','generated'), default='auto',
                        help='match source opacity with the AI silhouette (default), preserve raw AI alpha, or use the source mask')
    parser.add_argument('--background-key', metavar='RRGGBB',
                        help='clear a known painted background connected to tile edges; requires --texture')
    parser.add_argument('--background-tolerance', type=int, default=32,
                        help='per-channel color tolerance for --background-key (default: 32)')
    parser.add_argument('--reuse-generated', action='store_true', help='reprocess cached generated PNGs locally and overwrite replacements without API requests')
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--dry-run', action='store_true', help='print prompts without network requests; tileset mode also saves preview sheets')
    args = parser.parse_args(argv)
    grid_import = args.import_grid is not None or args.import_zip is not None
    if args.dump_grid is not None and (args.texture or args.tile_size or args.limit is not None):
        raise RecompError('--dump-grid includes ALL originals; omit --texture, --tile-size and --limit')
    if grid_import and (args.texture or args.tile_size or args.limit is not None):
        raise RecompError('grid/ZIP import processes the manifest; omit --texture, --tile-size and --limit')
    if args.grid_width is not None and (args.grid_width < 1 or args.dump_grid is None):
        raise RecompError('--grid-width must be positive and requires --dump-grid')
    if not 16 <= args.grid_tile_size <= 2048:
        raise RecompError('grid-tile-size must be 16..2048')
    if args.grid_json is not None and not grid_import:
        raise RecompError('--grid-json requires --import-grid or --import-zip')
    if grid_import and args.grid_json is None:
        raise RecompError('grid/ZIP import requires --grid-json from the original export')
    if (args.dump_grid is not None or grid_import) and args.reuse_generated:
        raise RecompError('grid export/import is local; omit --reuse-generated')
    if args.limit is None:
        args.limit = 16 if args.tileset else 1
    if args.text is not None and not args.texture:
        raise RecompError('--text requires --texture')
    if args.background_key is not None:
        if not args.texture:
            raise RecompError('--background-key requires --texture')
        if not re.fullmatch(r'[0-9a-fA-F]{6}', args.background_key):
            raise RecompError('background-key must be an RGB hex color, e.g. ffffff')
        if args.alpha == 'source':
            raise RecompError('--background-key requires --alpha auto or generated')
        args.background_key = args.background_key.lower()
    if not 0 <= args.background_tolerance <= 255:
        raise RecompError('background-tolerance must be 0..255')
    if not 2 <= args.scale <= 32 or args.limit < 1 or args.timeout <= 0:
        raise RecompError('scale must be 2..32, limit must be positive, and timeout must be positive')
    if not 1 <= args.tiles_per_sheet <= 64:
        raise RecompError('tiles-per-sheet must be 1..64')
    if args.input.resolve() == args.output.resolve():
        raise RecompError('input and output directories must differ')
    if args.import_grid is not None:
        return import_grid(args)
    if args.import_zip is not None:
        return import_zip(args)
    if not args.input.is_dir():
        raise RecompError(f'texture dump directory does not exist: {args.input}')
    extra = args.prompt_file.read_text() if args.prompt_file else ''
    sources = [path for path in sorted(args.input.glob('*.png')) if NAME.fullmatch(path.name)]
    if args.dump_grid is not None:
        return dump_grid(args, sources, extra)
    sources = [path for path in sources if path.with_suffix('.json').is_file()]
    if args.texture:
        sources = [path for path in sources if args.texture.lower() in (path.name.lower(), NAME.fullmatch(path.name)['hash'].lower())]
        if not sources:
            raise RecompError(f'texture was not found in the dump directory: {args.texture}')
    if args.tile_size:
        if not re.fullmatch(r'[1-9]\d*x[1-9]\d*', args.tile_size):
            raise RecompError('tile-size must be WIDTHxHEIGHT, e.g. 32x32')
        dimensions = tuple(map(int, args.tile_size.split('x')))
        sources = [path for path in sources if read_source(path)[1].size == dimensions]
        if args.texture and not sources:
            raise RecompError('the selected texture does not match --tile-size')
    if args.reuse_generated:
        if args.texture and not (args.output/'generated'/sources[0].name).is_file():
            raise RecompError(f'cached generated PNG was not found: {args.output/"generated"/sources[0].name}')
        sources = [path for path in sources if (args.output/'generated'/path.name).is_file()]
    else:
        sources = [path for path in sources if not (args.output/path.name).exists()]
        if not args.texture:
            rejected = [path for path in sources if skipped_path(args.output, path).exists()]
            sources = [path for path in sources if not skipped_path(args.output, path).exists()]
            if rejected:
                print(f'Skipping {len(rejected)} previously rejected textures; use --texture or --reuse-generated to revisit them.', flush=True)
    if not sources:
        print('No cached generated textures.' if args.reuse_generated else 'No pending textures. Existing replacements are preserved.'); return 0
    key = os.environ.get('OPENAI_API_KEY', '')
    if not args.dry_run and not args.reuse_generated and not args.tileset and not key:
        raise RecompError('set OPENAI_API_KEY in your environment, or use --dry-run')
    if args.tileset and not args.reuse_generated:
        return remake_tilesets(args, sources, extra, key)
    saved_count = skipped_count = 0
    for path in sources[:args.limit]:
        metadata, source = read_source(path)
        expected_text = expected_text_for(args, metadata)
        target = (source.width*args.scale, source.height*args.scale)
        if max(target) > 8192:
            raise RecompError(f'replacement exceeds renderer limits: {path.name}')
        raw_path = args.output/'generated'/path.name
        raw_metadata_path=raw_path.with_suffix('.json')
        manifest_path = (args.output/path.name).with_suffix('.json')
        previous={};crop_box=None
        if args.reuse_generated:
            Image = pillow()
            history=raw_metadata_path if raw_metadata_path.is_file() else manifest_path
            if history.is_file():
                previous=json.loads(history.read_text())
                crop_box=previous.get('crop_box')
            cached_bytes = raw_path.read_bytes()
            try:
                with Image.open(BytesIO(cached_bytes)) as cached:
                    size = f'{cached.width}x{cached.height}'
            except (OSError, ValueError) as error:
                skip_texture(args, path, error, previous)
                skipped_count += 1
                continue
        else:
            size,crop_box = generation_canvas(*target,args.model)
        requested_size = tuple(map(int, size.split('x')))
        prompt = make_prompt(source, requested_size, extra,crop_box,expected_text)
        if args.dry_run:
            print(json.dumps({'texture':path.name, 'source':source.size, 'minimum_size':target,
                              'resize_to_scale':args.resize_to_scale, 'api_size':size,
                              'crop_box':crop_box,
                              'model':args.model, 'alpha':args.alpha, 'reuse_generated':args.reuse_generated,
                              'alpha_processing':resolve_alpha_mode(source,args.alpha),
                              'background_key':args.background_key, 'background_tolerance':args.background_tolerance,
                              'expected_text':expected_text,'prompt_version':PROMPT_VERSION,
                              'prompt':prompt}, indent=2)); continue
        args.output.mkdir(parents=True, exist_ok=True)
        if args.reuse_generated:
            print(f'Reusing {raw_path}', flush=True)
            generated = cached_bytes
        else:
            print(f'Redrawing {path.name}: {source.size} -> {size} ({args.model})', flush=True)
            if crop_box is not None:
                print(f'  Padded reference; saved texture crops to {crop_box[2]-crop_box[0]}x{crop_box[3]-crop_box[1]}.',flush=True)
            reference=reference_png(source,requested_size,crop_box)
            Image=pillow()
            with Image.open(BytesIO(reference)) as reference_image:reference_size=reference_image.size
            print(f'  Reference {reference_size[0]}x{reference_size[1]}; prompt v{PROMPT_VERSION}.',flush=True)
            generated, usage = edit_image(reference, prompt, key=key, model=args.model,
                size=size, quality=args.quality, transparent=source.getchannel('A').getextrema()[0] < 255, timeout=args.timeout)
            # Keep the service result even when validation rejects it.
            raw_path.parent.mkdir(parents=True, exist_ok=True)
            atomic_write(raw_path, generated)
            previous = {'model':args.model, 'quality':args.quality, 'prompt':prompt, 'usage':usage,
                        'api_size':size,'crop_box':crop_box,'prompt_version':PROMPT_VERSION,
                        'expected_text':expected_text,'reference_size':reference_size}
            atomic_write(raw_metadata_path,(json.dumps(previous,indent=2)+'\n').encode())
        processing = {}
        try:
            output = finish_image(source, generated, target, args.alpha, args.resize_to_scale,
                                  crop_box, requested_size, background_key=args.background_key,
                                  background_tolerance=args.background_tolerance,
                                  tileset=bool(previous.get('tileset_id') or previous.get('grid_id')), processing=processing)
        except (RecompError, OSError, ValueError) as error:
            skip_texture(args, path, error, previous)
            skipped_count += 1
            continue
        save_replacement(args, path, metadata, source, output, previous, processing)
        saved_count += 1
    if not args.dry_run:
        print(f'Finished: {saved_count} textures saved, {skipped_count} invalid AI results skipped.', flush=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RecompError, OSError, ValueError) as error:
        print(f'error: {error}', file=sys.stderr); sys.exit(1)
