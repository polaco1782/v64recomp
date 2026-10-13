# Texture dumps and AI remakes

The native RT64 runner can export the original textures as they enter the decoded
texture cache. Enable it while playing:

```sh
.venv/bin/python games/sm64-us/run_sm64.py --render --dump-textures
```

The default game directory is `local/textures/sm64-us/`. `dump/` receives
RGBA PNGs and JSON metadata, once per texture. A filename such as
`0123456789abcdef.v5.png` uses RT64's 64-bit content hash and hash algorithm version.
The hash covers the texture bytes, referenced palette entries, dimensions and
decoding parameters. It stays the same when the same asset moves in RAM. Changes
to an animated texture or its palette can produce a new file. This is a texture
content ID, not a ROM code checksum or a RAM address. Game directories keep packs
separate. Existing complete dumps are reused on subsequent runs.

Dumping is opt-in and happens locally. It exports RGBA16/32, CI4/8 with RGBA16 or
IA16 palettes, IA and intensity formats, including TMEM row swaps and alpha.
Framebuffer images, raw TMEM sampling and YUV textures are not exported. Textures
are discovered by drawing, so visit the scenes you want to remake. A bounded
scripted run opens its renderer after startup; use interactive play to capture
the splash, title and menu textures too.

## Remake textures

Install the optional image-processing dependency:

```sh
.venv/bin/pip install -r requirements-textures.txt
```

Preview one request locally, without an API key or upload:

```sh
.venv/bin/python games/sm64-us/remake_textures.py --dry-run
```

Set `OPENAI_API_KEY` in your environment, then run:

```sh
# Default: redraw one texture, preserving resolution and matching source opacity.
.venv/bin/python games/sm64-us/remake_textures.py
# Select a texture and append consistent art/material instructions.
.venv/bin/python games/sm64-us/remake_textures.py \
  --texture 0123456789abcdef --prompt-file /path/to/art-style.txt
# Process at most ten new textures, skipping existing replacements.
.venv/bin/python games/sm64-us/remake_textures.py --limit 10
# Give the exact text for a selected glyph (this SM64 texture is capital A).
.venv/bin/python games/sm64-us/remake_textures.py \
  --texture 029cca6f508abde8 --text A
```

The separate tool uploads original PNGs, individually or as divided tilesets, to OpenAI's
[image-edit API](https://developers.openai.com/api/reference/resources/images/methods/edit).
Requests use `gpt-image-2.5-flare` by default; `--model` selects another image
model available to your API account. Every non-dry-run request uses your API
account and billing. The emulator itself never contacts the service.

The prompt requires a visible high-resolution redraw: pixel-grid blocks, stair-step
edges and patchwork shading must become continuous contours, crisp antialiased
edges, smooth lighting and finer detail within the original forms. Simply copying
the enlarged pixels, blurring them or adding a glow is explicitly rejected.
It preserves the asset's identity,
colors, proportions, silhouette, orientation, feature placement, transparency and
repeating edges. Letters, numbers and symbols must retain their original identity,
case, stroke layout, holes, colors and font. It explicitly forbids interpreting
an A as a warning triangle or exclamation mark, adding objects to blank textures,
changing materials, or turning game art into a photorealistic redesign.
Small references are enlarged with nearest-neighbor sampling to a readable size
of up to 128 pixels per edge, rather than to the full output resolution. Larger
original references retain their size. Padded strips use the full reference canvas
to keep crop coordinates aligned. The reference guides the design; the model
must reconstruct the output. Reference size and prompt version are recorded
with each new result and printed before the request.
For exact lettering, use `--texture HASH --text A`, or add `"expected_text": "A"`
to that original texture's JSON sidecar. The latter applies during normal batches.
The prompt and its version are recorded with the generated image. Prompts reduce
semantic errors but cannot guarantee the model will copy every character correctly;
review glyphs before accepting them. Existing generated images are preserved.
RGB is redrawn, rather than enlarging the original pixels. The API result is
generated on the smallest supported canvas and retains its resolution by default.
GPT Image 2.5 requires at least 655,360 pixels, 16-pixel
edge multiples, and an aspect ratio within 3:1; a small square texture therefore
requests 816×816 instead of 1024×1024. See the
[model size constraints](https://developers.openai.com/api/docs/guides/image-prompting).
`--scale` sets a minimum size
relative to the original texture; it defaults to four times each dimension.
RT64 accepts the larger image and adjusts the UV scale automatically.
Aspect-ratio changes and undersized results are rejected, and the untouched
service result is also retained in `replacements/generated/` for review.
The default `--alpha auto` matches the original opacity type using the AI artwork:
opaque sources stay opaque, binary cutouts use the generated silhouette with
alpha 0/255, and sources with soft transparency retain generated alpha. Cutout
alpha is thresholded at half its maximum; a fully transparent result is rejected.
When cutout alpha is inconsistent, `auto` first tries to recover it. A strongly
inverted mask is flipped. For a painted white/gray background or checkerboard,
the tool samples colors in the original transparent regions, infers a light matte
color, and removes its connected background from the new artwork. Enclosed matte
regions can also be cleared when the source identifies them as holes, while
foreground highlights are retained. Existing transparent edges are preserved;
an entirely zeroed alpha channel can be reconstructed from separable RGB artwork.
The original mask guides detection and validation, rather than clipping the new
silhouette to the old pixel grid. Recovery requires at least 65% agreement at both
foreground and background source sample positions and enough foreground remaining.
It preserves RGB and records the method, inferred key and validation scores under
`alpha_recovery` in the replacement JSON. If the background cannot be identified
or the model erased the foreground, the result remains skipped.
The tool also rejects cutouts that lose most of the original empty background,
even if a few pixels still have alpha 0. This catches painted checkerboards and
matte backgrounds that would otherwise pass a simple alpha-channel check. The
check requires the cleared fraction to remain at least a quarter of the original
transparent fraction; it is a conservative coverage check, not image recognition.
This matters for HUD glyphs: some draw commands test alpha against 255, so an AI
image whose maximum alpha is 254 would load successfully but render invisible.
The old low-resolution mask is not reapplied. `--alpha generated` preserves raw
AI alpha instead; it can make cutouts invisible under those draw commands.
`--resize-to-scale` explicitly reduces the generated image to the original size
times `--scale`. `--alpha source` explicitly replaces AI transparency with the
original cutout mask. These options can remove generated detail or clip an AI
silhouette that differs from the original; use them only when needed.
Narrow strips are supported using a padded reference canvas. The tool enlarges
the reference with nearest-neighbor sampling, preserves its proportions, places
it in the center of a supported canvas, and asks the model to redraw only that
rectangle. It then crops away the padding and applies the selected opacity handling.
For example, a 16×4 strip uses an 816×816 canvas and saves an 816×204 texture.
Canvas size and crop coordinates are recorded with the cached generated image
so `--reuse-generated` can repeat the same crop without another API call.
AI cannot guarantee exact UV alignment or
seam continuity; inspect the result in the game before processing a large batch.

For smaller game files, explicitly save at four times the original dimensions:

```sh
.venv/bin/python games/sm64-us/remake_textures.py --limit 10 --resize-to-scale
```

This requests the same minimum API canvas, then reduces the new AI artwork
using its generated silhouette and selected opacity handling. A 32×32 original
becomes a 128×128 replacement.

The tool saves finished PNGs in `replacements/` under the same filenames, with
generation settings and usage in JSON sidecars. It processes one new texture by
default and skips existing replacements. Invalid AI artwork is reported by
filename and skipped while the remaining textures continue. It never
automatically retries an API request. Keep the directory to resume later.

Rejected filenames and reasons are recorded in `replacements/skipped/`. Normal
batches omit these textures before selecting their inputs, so resuming does not
upload the same rejected originals again. Raw AI images remain cached, and an
existing working replacement is preserved if a local reprocessing attempt fails.
Explicit `--texture HASH` selection or `--reuse-generated` bypasses the rejection
record for review or repair; a successful save clears that record. Request,
source metadata and filesystem failures are reported as errors.

To restore a previously processed replacement from its cached AI output without
another upload or API call, run:

```sh
.venv/bin/python games/sm64-us/remake_textures.py \
  --texture 0123456789abcdef --reuse-generated
```

This overwrites the selected replacement locally, preserves the cached PNG,
and updates its JSON dimensions and processing settings. Generation model,
prompt and usage records remain those of the original request.
To attempt automatic recovery across cached textures, including previously
rejected ones, use `--reuse-generated --tileset --limit 64`. This is local
processing with no image API requests.

If the model painted a known background color into a cutout, remove that background
from the cached artwork without another request:

```sh
.venv/bin/python games/sm64-us/remake_textures.py \
  --texture 05ca166bef1000bc --reuse-generated \
  --background-key ffffff --background-tolerance 48
```

`--background-key RRGGBB` requires one selected texture. It clears matching colors
connected to the image border, including nearby shades within the per-channel
tolerance (default 32). It preserves RGB, dimensions, the AI silhouette and
isolated interior highlights; it does not restore the original pixel mask.
Choose a color that belongs to the unwanted background, since matching foreground
that touches the border can also be removed. The cached raw image is unchanged,
and the selected key and tolerance are recorded in the replacement JSON.

## Export enlarged texture grids

`--dump-grid` collects **every** dumped texture, including those with existing
replacements or rejection records, and exports batches of up to sixteen tiles.
The model redraws each entire grid as one PNG. Squares have at least 512×512
pixels; rectangles preserve their aspect ratio and receive at least the same
pixel area. Integer nearest-neighbor enlargement keeps source strokes and alpha
unchanged in the reference. For example, 32×32 and 16×16 become 512×512;
16×8 becomes 736×368, and 64×32 becomes 768×384.

```sh
# Export existing dumps without building or starting the game.
.venv/bin/python games/sm64-us/run_sm64.py --dump-grid
# Collect textures during play, then export when the game exits.
.venv/bin/python games/sm64-us/run_sm64.py --render --dump-grid
# Select a base filename, eight textures per grid, and the minimum tile area.
.venv/bin/python games/sm64-us/remake_textures.py \
  --dump-grid /path/to/game-grid.png --tiles-per-sheet 8 --grid-tile-size 512
```

The default directory is `local/textures/sm64-us/grids/`. For multiple
batches it contains `all-textures.001.png`, `.002.png`, and so on. Each PNG has a
matching `.json` and `.prompt.txt`. `all-textures.json` is the master manifest,
including every grid's layout and original RGBA sources. `all-textures.prompt.txt`
summarizes the grids and expected output filenames. If everything fits one batch,
the files use `all-textures.png`, `.json` and `.prompt.txt` without numbering.

Tiles are grouped by original size and sorted by filename within each group.
Magenta gutters and section headings separate the artwork. Every batch retains
fixed integer rectangles and real transparency. Packing automatically reduces
the batch size when needed to fit the model canvas limits. Short batches and
narrow strips receive extra unused canvas padding; texture proportions stay
unchanged. All originals are included across the batches; `--limit`, `--texture`
and `--tile-size` cannot filter this export.

`--grid-tile-size` sets the minimum square-equivalent tile area (default 512).
`--scale` sets an additional minimum multiplier relative to the originals
(default 4). `--grid-width` optionally sets the canvas width. `--tiles-per-sheet`
sets the maximum batch count (default 16). `--prompt-file` appends style guidance.
`--dry-run` in the texture tool prints planned grids and prompts without writing
files. With a custom input directory, the default `grids/` directory is alongside
that input; the launcher uses `--dump-textures DIRECTORY`, while the texture tool
uses `--input DIRECTORY`.

Give a reference PNG and its matching prompt to the model. The prompt requires
one whole-grid PNG at **exactly the same canvas dimensions**, with no resizing,
reordering or shifting of tiles, gutters or sections. It asks for newly drawn
detail inside the enlarged rectangles, preserving literal glyphs, proportions,
alpha and connected/repeating edges. Copyrighted content may be recreated or
replaced with an original design while retaining its texture role and layout.
The model is asked to name its output `all-textures.001.processed.png`, etc.
It does not need to produce individual texture files or a ZIP.

Save processed files with these names in a directory and import them together:

```sh
.venv/bin/python games/sm64-us/remake_textures.py \
  --import-grid /path/to/processed-grids \
  --grid-json local/textures/sm64-us/grids/all-textures.json
```

To import one batch, use its matching JSON; this also allows any processed PNG
filename:

```sh
.venv/bin/python games/sm64-us/remake_textures.py \
  --import-grid /path/to/processed-grid.png \
  --grid-json local/textures/sm64-us/grids/all-textures.001.json
```

The master manifest validates all texture identities before writing replacements.
Missing processed grids are reported and skipped. Invalid canvases are rejected
for that grid while other batches continue. Each valid tile is extracted under
its original hash filename, applying the usual alpha recovery and divider cleanup.
Invalid tiles preserve any existing working replacement. Raw extracted PNGs stay
in `replacements/generated/`, and processing provenance goes into JSON sidecars.
Export and import are local operations with no API calls. F5 reloads replacements.

Processed grids must match their exported dimensions or be an integer enlargement
of both edges. An enlargement adjusts crop coordinates and preserves resolution.
`--resize-to-scale` explicitly reduces a tile to its saved per-tile scale, keeping
the planned 512-square-equivalent minimum. `--output DIRECTORY` selects the
replacement destination. `--dry-run` validates manifests and canvases without
writing replacements. Embedded original RGBA sources allow imports after clearing
the dump directory.

Older single-grid JSONs remain supported. If a verified previous single reference
is replaced by a batch export, its PNG, JSON and prompt are preserved under
`grids/previous/<grid-id>/`. Previously generated individual PNG ZIPs can still
be imported using `--import-zip ZIP --grid-json` with their original manifest.

## Remake divided tilesets

Use `--tileset` to group textures by their exact original dimensions, pack each
group into a grid, redraw the whole sheet in one request, and extract the cells
back into their individual hash-named PNGs. Magenta gutters and outer divider
lines mark the cell limits and are excluded from every extracted texture.
No replacement directory or renderer changes are needed.

```sh
# Preview up to sixteen 32x32 originals locally; no upload or game files changed.
.venv/bin/python games/sm64-us/remake_textures.py \
  --tileset --tile-size 32x32 --dry-run
# Redraw up to sixty-four pending textures, grouping all original sizes.
.venv/bin/python games/sm64-us/remake_textures.py --tileset --limit 64
# Use smaller sheets and explicitly save each recreation at 4x its original size.
.venv/bin/python games/sm64-us/remake_textures.py \
  --tileset --tiles-per-sheet 8 --limit 32 --resize-to-scale
```

Tileset mode defaults to sixteen textures per invocation and at most sixteen
per sheet. `--limit` counts selected textures, including newly rejected results,
not requests; previously rejected textures are excluded before that limit is
applied. Each distinct size needs its
own sheet, and `--tiles-per-sheet` caps each request. Existing replacements are
skipped before grouping. `--tile-size WIDTHxHEIGHT` optionally selects one source
size. Oversized groups are split to fit API limits. Cells retain the exact source
aspect ratio, and `--scale` sets their minimum resolution. Very narrow or tall
strips use padding on the short canvas edge to meet the API aspect-ratio limits;
extraction excludes that padding, preserving their original proportions.
For example, one 128×16 strip fits a 1456×496 canvas and saves a 1408×176 tile
with the default minimum scale. Sixteen
32×32 textures fit a 4×4 sheet requested at 896×896, with 192×192 artwork per tile.
The input uses smaller readable references, while the prompt specifies exact
output coordinates.

The prompt requires a true recreation within each cell, forbids changing grid
positions, order, orientation or feature locations, and asks the model to preserve
compatible edge details where tiles visibly connect. Hash sorting determines
packing order; it does not establish which textures are neighbors in the game.
The prompt therefore also forbids inventing connections between unrelated cells.
Literal `expected_text` values from individual source JSON sidecars apply to their
own cells. Opacity handling runs separately on every extracted tile.

`replacements/tilesets/` stores `<size>-<id>.reference.png`, `.generated.png` and
`.json`. The JSON maps every filename to a fixed rectangle, records the reference
and output dimensions, and retains the prompt, model and usage for the request.
Dry runs write only preview references and layouts here. The returned sheet must
match the requested dimensions exactly. A changed or unreadable sheet is skipped
and its member textures receive rejection records; later sheets continue. Each
valid crop is saved independently, so one failed transparency or divider check
does not block the other cells. Raw sheets and extracted tiles are retained when
validation fails. Repeating the same eligible sheet reuses its cached result
without another API request. The final output reports saved and skipped counts.

Small magenta divider spills along a tile edge are replaced with the adjacent
artwork during processing, preserving the dimensions and the rest of the image.
This also runs when reusing cached tiles. It only handles narrow edge bands;
larger intrusions are rejected. Assets whose originals contain magenta are left
alone, because that color cannot safely identify a divider in their artwork.
The replacement JSON records the repaired edges and their widths under
`divider_cleanup`. Raw generated crops and sheets retain their original pixels.

Each extracted raw tile is also saved in `replacements/generated/`, so the usual
`--texture HASH --reuse-generated` command can reprocess it locally. Per-texture
JSONs link to the sheet and its coordinates. Their `usage_scope: "tileset"` means
the recorded usage belongs to the whole request; do not add that same usage once
per tile. Prompts cannot guarantee unchanged positions or seamless artwork;
review the returned sheet and extracted textures before processing a large pack.

## Use replacements

The runner automatically reads `replacements/` when it exists. A matching PNG
replaces the original through RT64's texture cache and UV scaling; missing files
use the original ROM texture. No `rt64.json` index is required. Put a hand-made
PNG with the same hash filename here to use a different service or an artist's
version. Preserve the original aspect ratio. Each **F5** press during play reloads
added, changed or removed files and toggles between original and replacement
textures. Replacements start enabled: the first press shows originals, the next
shows refreshed replacements. Replacements also load on the next launch.

```sh
.venv/bin/python games/sm64-us/run_sm64.py --render
# Use custom directories.
.venv/bin/python games/sm64-us/run_sm64.py --render \
  --dump-textures /path/to/originals --texture-replacements /path/to/remakes
# Compare with the originals.
.venv/bin/python games/sm64-us/run_sm64.py --render --no-texture-replacements
```

Dump and replacement directories must differ. Reloading can briefly stall while
PNG uploads complete. Normal play only checks the directory at startup or F5;
it does not scan the filesystem every frame. Bounded run results include counts
of PNGs written, replacement files indexed and replacement textures encountered.
