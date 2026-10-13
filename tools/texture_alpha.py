"""Recover usable cutout alpha without applying the old low-resolution mask."""
from __future__ import annotations

from statistics import median


def solid_mask(alpha):
    peak = alpha.getextrema()[1]
    threshold = (peak + 1) // 2
    return alpha.point([255 if peak and value >= threshold else 0 for value in range(256)])


def mask_agreement(source, mask) -> dict:
    from PIL import Image

    original = source.getchannel('A').tobytes()
    probe = mask.resize(source.size, Image.Resampling.NEAREST).tobytes()
    foreground = original.count(255)
    background = original.count(0)
    return {'foreground': sum(p == 255 for p, a in zip(probe, original) if a == 255) / max(1, foreground),
            'background': sum(p == 0 for p, a in zip(probe, original) if a == 0) / max(1, background),
            'foreground_samples': foreground, 'background_samples': background,
            'source_clear': background / len(original),
            'clear': mask.histogram()[0] / (mask.width * mask.height)}


def remove_background_key(image, key: str, tolerance: int, *, source=None):
    """Clear border-connected matte; optionally recognize matte in source holes."""
    from PIL import Image, ImageChops, ImageDraw

    red, green, blue = (int(key[offset:offset+2], 16) for offset in (0, 2, 4))
    rgb = image.convert('RGB').tobytes()
    candidates = bytes(255 if max(abs(r-red), abs(g-green), abs(b-blue)) <= tolerance else 0
                       for r, g, b in zip(rgb[::3], rgb[1::3], rgb[2::3]))
    mask = Image.frombytes('L', image.size, candidates)
    padded = Image.new('L', (image.width+2, image.height+2), 255)
    padded.paste(mask, (1, 1))
    ImageDraw.floodfill(padded, (0, 0), 128)
    if source is not None:
        # A checkerboard can also be inside an enclosed hole. Inspect remaining
        # components using source coordinates, but never clip artwork to that mask.
        original = source.getchannel('A')
        expected_clear = original.point([255 if value == 0 else 0 for value in range(256)]).resize(
            image.size, Image.Resampling.NEAREST)
        for index, alpha in enumerate(original.tobytes()):
            if alpha:
                continue
            x, y = index % source.width, index // source.width
            seed = (min(image.width-1, int((x+.5)*image.width/source.width))+1,
                    min(image.height-1, int((y+.5)*image.height/source.height))+1)
            if padded.getpixel(seed) != 255:
                continue
            ImageDraw.floodfill(padded, seed, 64)
            component = padded.crop((1, 1, image.width+1, image.height+1)).point(
                [255 if value == 64 else 0 for value in range(256)])
            count = component.histogram()[255]
            clear_count = ImageChops.multiply(component, expected_clear).histogram()[255]
            probe = component.resize(source.size, Image.Resampling.NEAREST).tobytes()
            foreground_hit = any(p and a == 255 for p, a in zip(probe, original.tobytes()))
            # Retain enclosed white highlights/foreground even when the new
            # contour extends beyond the old silhouette.
            replacement = 128 if not foreground_hit and clear_count >= count * .8 else 192
            padded = padded.point([replacement if value == 64 else value for value in range(256)])
    keep = padded.crop((1, 1, image.width+1, image.height+1)).point(
        [0 if value == 128 else 255 for value in range(256)])
    result = image.copy()
    result.putalpha(ImageChops.multiply(result.getchannel('A'), keep))
    return result


def recover_cutout_alpha(source, image) -> tuple[object, dict]:
    """Repair clear inversions or a confidently identified light matte."""
    from PIL import Image

    current_mask = solid_mask(image.getchannel('A'))
    current = mask_agreement(source, current_mask)
    enough_samples = min(current['foreground_samples'], current['background_samples']) >= 4
    if enough_samples and current['foreground'] < .2 and current['background'] < .2:
        inverted = current_mask.point([255-value for value in range(256)])
        recovered = image.copy()
        recovered.putalpha(inverted)
        return recovered, {'method': 'inverted_alpha'}
    if (current['clear'] >= current['source_clear'] / 4
            and 1-current['clear'] >= (1-current['source_clear']) / 4):
        return image, {}
    # Use clear source areas only as color probes. Their geometry is not imposed
    # on the output; the recovered high-resolution contour comes from its RGB.
    sampled = image.resize(source.size, Image.Resampling.NEAREST).convert('RGB').tobytes()
    original = source.getchannel('A').tobytes()
    samples = [(r, g, b) for r, g, b, a in zip(sampled[::3], sampled[1::3], sampled[2::3], original) if a == 0]
    neutral = [pixel for pixel in samples if min(pixel) >= 210 and max(pixel)-min(pixel) <= 32]
    if len(neutral) < 4 or len(neutral) < len(samples) * .65:
        return image, {}
    key = ''.join(f'{round(median(pixel[channel] for pixel in neutral)):02x}' for channel in range(3))
    # Retain existing clear pixels at cell borders and holes. Only a wholly
    # zero alpha channel needs its foreground opacity reconstructed from RGB.
    base = image.copy()
    if image.getchannel('A').getextrema()[1] == 0:
        base.putalpha(255)
    recovered = remove_background_key(base, key, 48, source=source)
    recovered_mask = solid_mask(recovered.getchannel('A'))
    agreement = mask_agreement(source, recovered_mask)
    if (agreement['foreground'] < .65 or agreement['background'] < .65
            or agreement['clear'] < agreement['source_clear'] / 4
            or 1-agreement['clear'] < (1-agreement['source_clear']) / 4):
        return image, {}
    recovered.putalpha(recovered_mask)
    return recovered, {'method': 'matte_key', 'key': key, 'tolerance': 48,
                       'foreground_agreement': round(agreement['foreground'], 4),
                       'background_agreement': round(agreement['background'], 4)}
