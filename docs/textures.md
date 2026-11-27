# Desktop texture packs

<a id="gles3-texture-packs-and-editor"></a>
<a id="gles2-texture-packs-and-editor"></a>


HQ2x doubles the width and height of decoded N64 tiles before GLES uploads them.
It runs once per texture cache miss and blends RGBA edges, including across
repeating texture seams. Larger-than-TMEM tiles (over 8192 texels) use native
resolution. Texture packs and editor overrides use the resolution of the supplied
image. Set `RECOMP_GLES2_HQ2X=0` before launch to disable enhancement.

The native tile cache keeps up to 1,024 uploaded textures within a 64 MiB
budget, including mipmaps. Drawing a resident tile reuses its filtered image.
The lookup checks the tile contents, format, dimensions, palette state and
HQ2x edge wrapping. F5 clears the cache when switching texture modes.

For indexed tiles, the editor and replacement loader identify the tile by its
texel indices and the palette colors those indices use. A palette animation
gets a new ID when a color used by the tile changes.

The loader recognizes older AI and dropped-PNG filenames when it encounters
their original tile and palette state. Opening the editor, reloading the pack or exiting the game saves
new mappings and imports existing loose `.tile-id` files into the active ZIP
under `v64recomp/ids/`. If writing the ZIP fails, it saves the mappings in the
local overrides directory. To transfer AI edits to a Steam Deck, copy the ZIP
into the game's `texture-overrides/` directory and use a build with embedded
mapping support. Dropped PNGs are still loose files.

If several old AI edits belong to one tile, the last
one in ZIP order wins. A new AI edit saved under the stable ID takes priority.
Removing the active edit reveals the next available replacement.

Desktop GLES3 loads Rice/GLideN64 PNG ZIPs directly. On the first draw of a
texture, it checks the original texture and palette loads against the Rice
filenames in the ZIP. It remembers matches in memory. Exact IDs, CI palette or texture `$` wildcards,
and split `_rgb` / `_a` images are supported. Palette matching checks both
the GLideN64 texture-filter palette and the palette bytes transferred by the RDP.
An AI edit in the ZIP takes priority over a dropped PNG, followed by the original pack entry.
GLES decodes and uploads a matched PNG before drawing the tile. A first use
or a reload after cache eviction can take extra frame time.
Tiles matched to the same pack image share one upload. Uploaded replacements
use a separate cache that grows on demand up to 256 MiB,
including mipmaps. F5 clears the cache.
Tiles with incomplete source loads or incompatible images keep their original
appearance.

```sh
./build/banjo-kazooie-us-gles-release/bin/banjo-kazooie-us
# Or supply a compatible pack at launch:
./build/banjo-kazooie-us-gles-release/bin/banjo-kazooie-us --texture-pack /path/to/pack.zip
```

Press **F8** to pause and open the texture gallery. The most recently drawn
tiles appear first. The gallery keeps every tile used in the last two textured
frames, including cached replacements, plus older tiles up to a history of 128
when space remains. Thumbnails show the stored image orientation. A game can
flip or rotate that image when placing it on a wall.

In the paused game window, cyan lines mark the visible triangles. Move the
pointer over a triangle to turn its outline gold. Click anywhere inside it to
select its texture and jump to the matching gallery page. For a triangle that
uses two distinct textures, **Shift-click** adds the second to your selection.
On a single-texture triangle, it adds that texture and shows a message.
Untextured triangles can be highlighted. CPU-drawn images have no triangles to
pick. Press **Esc** or **F8** in either window to resume.

The editor is available by default on desktop in Debug and Release, without a ZIP.
Set `RECOMP_GLES2_TEXTURE_EDITOR=0` to skip observing thumbnails. Passing
`--texture-editor` or `--texture-pack ZIP` explicitly overrides this setting.
Click a thumbnail to select it by itself. **Shift-click** another thumbnail to
add it, or Shift-click a selected one to deselect it. Use the mouse wheel, **Page Up**, or **Page Down** to
browse pages while keeping the selection. Click **Improve with AI** to send
each selected native tile to your signed-in Codex CLI. The prompt asks Codex
to preserve its pattern, motif placement, repeat spacing, palette, lettering,
tiling edges and transparency.
It asks for clean contours and gradients with pixelation artifacts removed.
The editor accepts a PNG exactly **8× the native width and height**. A 32×16 tile
becomes 256×128, including when you improve it again. Codex must be on `PATH`
with image generation available. On Windows the editor launches `codex.exe`.

The editor processes the tiles selected when you clicked the button, one at a
time. You can browse and change the selection while Codex works. Each result
is saved under its own tile. If a tile fails, the batch stops. Closing the editor
with **Esc** or **F8** cancels the remaining work. Completed edits stay in the ZIP and appear when play resumes.
With `--texture-pack`, the edit goes into that ZIP. Without a supplied pack,
the editor creates `<data>/texture-overrides/textures.zip`. Each tile has its
own `v64recomp/ai/<hash>.gles.png` entry. **Remove AI version** deletes that
entry and restores the underlying dropped PNG, pack texture, or native tile.

You can also drop a PNG onto a selected thumbnail. The editor saves it in
`<data>/texture-overrides/<hash>.gles.png`. Use **Remove replacement** to
delete a dropped PNG. If a tile has an AI version, remove it before dropping a new PNG onto that tile.
All replacements must match the original aspect ratio,
fit the GPU limit (up to 2048 px per side), and be at most 16 MiB on disk.
**F5** refreshes the matches and switches between original and replacement textures.

Texture support is enabled by default on desktop. Set
`-DRECOMP_GLES2_TEXTURE_PACKS=OFF` to omit the loader and editor.
WebAssembly builds omit both.
