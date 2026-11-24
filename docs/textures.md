# Texture dumps and replacements

Pass an original Rice/GLideN64 ZIP directly to RT64 in either Debug or Release:

```sh
./build/sm64-us-debug/bin/sm64-us --texture-pack /path/to/pack.zip
```

RT64 indexes the ZIP's filenames once, matches textures as they are drawn, and
streams the matching PNGs directly from that same archive. Loading a pack leaves
the ZIP unchanged and requires no extraction or conversion.
RT64 saves hash-to-entry paths in `/path/to/pack.zip.rt64.json`, atomically on its
background worker and again at shutdown. The ZIP directory must be writable for
that cache to persist; a write failure is logged while in-memory matching continues.
The sidecar is reused on the next launch and invalidated when the ZIP's size,
modification time, ROM title, or RT64 hash version changes.

Matching supports exact Rice IDs, CI texture/palette `$` wildcards, ordinary
images before `Copy of`/`Flash` alternates, and `_rgb`/`_a` pairs. Conflicting
same-priority images are rejected. RGB/alpha composition happens in memory on
RT64 workers. Filenames must use the ROM's internal title and the supported
`_all`, `_ciByRGBA`, `_allciByRGBA`, `_rgb`, or `_a` PNG suffixes. Unmatched textures
use the original. Textures are discovered as they are drawn, so new scenes add
mappings during play.

The graphics thread only copies bounded texture/palette memory for previously
unseen hashes (at most 8 captures per frame, with a 256 KiB admission budget).
A bounded queue retries deferred captures on later draws. Rice hashing, ZIP image
reads, ambiguity checks, JSON writes, and decoding run on RT64 workers. New matches
enter the existing asynchronous streaming queue without rebuilding the pack or
reloading the replacement cache. The original texture stays visible until its
replacement is ready. **F5** reloads the ZIP/index and toggles originals/replacements.
Debug builds print each loaded ZIP entry, its dimensions, and any separate alpha
mask as `RT64 ZIP loaded: ...`.
`RECOMP_TEXTURE_PACK=/path/to/pack.zip` is equivalent to the command-line option;
it takes precedence over `RECOMP_TEXTURE_REPLACEMENTS`.

## Edit a texture pack in-game

Launch with `--texture-pack ZIP` and press **F8**. The editor releases the mouse,
freezes the displayed frame, and pauses the guest CPU and audio. You can start
with a ZIP of ordinary PNGs and assign them to textures in the editor. Hover
over a textured surface to outline its geometry. Click it to see the texture and use
**Replace...** to choose a PNG from the ZIP. **Import PNG...** adds a local PNG
to the ZIP and selects it in the browser.

**Use selected texture** writes a separate PNG entry for the target texture's
hash, preserving the source image in the ZIP. The editor uses the target's Rice
ID when available and the versioned RT64 hash otherwise. The replacement applies
everywhere the game uses that texture. Press **F8** or **Esc** to resume and see
the replacement in play.

Editing requires a writable standalone ZIP supplied through `--texture-pack`.
The ZIP directory needs room for a temporary copy while an edit is being saved.

<a id="gles2-texture-packs-and-editor"></a>

## GLES3 texture packs and editor

Desktop GLES3 loads Rice/GLideN64 PNG ZIPs directly. On the first draw of a
texture, it checks the original texture and palette loads against the Rice
filenames in the ZIP. It remembers matches in memory and also accepts existing
RT64 `.rt64.json` mappings. Exact IDs, CI palette or texture `$` wildcards,
and split `_rgb` / `_a` images are supported. Palette matching checks both
the GLideN64 texture-filter palette and the palette bytes transferred by the RDP.
Versioned RT64 v5 entries and local editor overrides take precedence.
GLES decodes matched PNGs on a background worker. It draws the original until
the replacement is ready, then uploads it on the render thread. A limit on uploads
per display list spreads mipmap generation over several draws as a scene loads.
Uploaded replacements use a separate cache that grows on demand up to 256 MiB,
including mipmaps. F5 clears the cache and discards outstanding requests.
Tiles with incomplete source loads or incompatible images keep their original
appearance. RTZ/DDS and executable embedding require RT64.

```sh
./build/banjo-kazooie-us-gles-release/bin/banjo-kazooie-us
# Or supply a compatible pack at launch:
./build/banjo-kazooie-us-gles-release/bin/banjo-kazooie-us --texture-pack /path/to/pack.zip
```

Press **F8** to pause and open a gallery of recently seen, editable textures.
It is available by default on desktop in Debug and Release, without a ZIP.
Set `RECOMP_GLES2_TEXTURE_EDITOR=0` to skip observing thumbnails. Passing
`--texture-editor` or `--texture-pack ZIP` explicitly overrides this setting.
Select a thumbnail, drop a PNG onto the window, then press **Esc** or **F8**
to resume. The replacement needs the same aspect ratio as the original, and
must fit the GPU limit (at most 2048 px per side and 16 MiB on disk).
GLES3 applies it to every use of that texture. It saves a local override in
`<data>/texture-overrides/<hash>.v5.png` without touching the source ZIP;
delete that override and reload to revert. **F5** refreshes the matches and
switches between original and replacement textures. This editor selects
textures from thumbnails, not by clicking game geometry.

Builds without the pinned texture dependencies, builds configured with
`-DRECOMP_GLES2_TEXTURE_PACKS=OFF`, and WebAssembly retain the plain renderer
without the editor. The GLES backend operates independently of the RT64 renderer.

## Embed a ZIP in the executable

Set the optional CMake path when configuring either Debug or Release:

```sh
cmake --preset sm64-us -B build/sm64-us-release -DCMAKE_BUILD_TYPE=Release -DRECOMP_EMBED_TEXTURE_PACK=/path/to/pack.zip
cmake --build build/sm64-us-release
```

The build appends the ZIP to the final executable and adjusts its archive offsets.
The game automatically streams textures from its own executable, without extracting
the pack or needing the source ZIP at launch. Changing the ZIP rebuilds the executable.
This works with or without `RECOMP_EMBED_ROM=ON`; the `pack` target also preserves
the embedded ZIP when producing a UPX executable.

`--texture-pack ZIP` or `RECOMP_TEXTURE_PACK=/path/to/other.zip` overrides the
embedded pack. Set `RECOMP_TEXTURE_PACK=` to disable it for a launch.
F5 works as usual. The matching cache is saved beside the executable as
`<executable>.rt64.json`; read-only installations still match in memory.
Configure with `-DRECOMP_EMBED_TEXTURE_PACK=` to return to an executable without
an embedded pack.

## Dump original textures

Every game can export original textures from RT64's decoded texture cache in
both Debug and Release. Enable dumping at launch:

```sh
./build/sm64-us-release/bin/sm64-us --texture-dump local/textures/sm64-us/dump
```

The selected directory is created automatically and receives RGBA PNGs and JSON
metadata, once per texture. Use a separate directory for each game.
Dumping is disabled unless `--texture-dump DIR` is supplied.
Textures with fewer than 64 pixels (`width * height < 64`) and textures whose
decoded RGBA pixels are all identical are skipped. Exactly 64 pixels is allowed;
varying alpha counts as a color difference. These filters affect only new dumps,
not rendering or replacement loading, and do not delete existing files.
A filename such as
`0123456789abcdef.v5.png` uses RT64's 64-bit content hash and hash algorithm version.
The hash covers the texture bytes, referenced palette entries, dimensions and
decoding parameters. It stays the same when the same asset moves in RAM. Changes
to an animated texture or its palette can produce a new file. This is a texture
content ID, not a ROM code checksum or a RAM address. Game directories keep packs
separate. Existing complete dumps are reused on subsequent runs.

Dumping is opt-in and happens locally. It exports RGBA16/32, CI4/8 with RGBA16 or
IA16 palettes, IA and intensity formats, including TMEM row swaps and alpha.
Framebuffer images, raw TMEM sampling and YUV textures are not exported. Textures
are discovered by drawing, so visit the scenes you want to capture. A bounded
scripted run opens its renderer after startup; use interactive play to capture
the splash, title and menu textures too.

## Use replacements

The runner automatically loads `replacements/textures.rtz` when present. The
archive includes its own RT64 index and loads without scanning enabled.
Existing loose PNG folders are also supported. A matching PNG
replaces the original through RT64's texture cache and UV scaling; missing files
use the original ROM texture. No `rt64.json` index is required. Put a custom
PNG with the same hash filename here and preserve the original aspect ratio.
Standard directories containing `rt64.json` and existing `.rtz` archives can
also be loaded through `RECOMP_TEXTURE_REPLACEMENTS`.
Each **F5** press during play reloads added, changed or removed files and toggles
between original and replacement textures. Replacements start enabled: the first
press shows originals, the next shows refreshed replacements. Replacements also
load on the next launch.

```sh
./build/sm64-us-release/bin/sm64-us
# Custom dump and replacement directories (Debug or Release).
RECOMP_TEXTURE_REPLACEMENTS=/path/to/replacements \
  ./build/sm64-us-release/bin/sm64-us --texture-dump /path/to/originals
# Compare with the originals.
RECOMP_TEXTURE_REPLACEMENTS= ./build/sm64-us-release/bin/sm64-us
```

Dump and replacement directories must differ. Reloading can briefly stall while
PNG uploads complete. Loose replacement directories are checked at startup or F5;
ZIP matching runs in RT64 as textures are encountered. Bounded run results include counts
of PNGs written, replacement files indexed and replacement textures encountered.
