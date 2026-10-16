# Texture dumps and replacements

For automatic Rice/GLideN64 matching while playing, use
[`--texture-scan [DIRECTORY]`](../texture_scan/README.md). The native worker moves
matching images into the game's replacement folder and requests a reload every
15 seconds when new matches are found. Both release and debug runners support
this mode; no Python texture importer or `.rtz` conversion is needed.

The native RT64 runner can export the original textures as they enter the decoded
texture cache. Enable it while playing:

```sh
RECOMP_TEXTURE_DUMP="$PWD/local/textures/sm64-us/dump" ./build/sm64-us-debug/bin/sm64-us
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
are discovered by drawing, so visit the scenes you want to capture. A bounded
scripted run opens its renderer after startup; use interactive play to capture
the splash, title and menu textures too.

## Use replacements

The runner automatically reads `replacements/` when it exists. A matching PNG
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
# Custom dump and replacement directories (debug only for dumping).
RECOMP_TEXTURE_DUMP=/path/to/originals RECOMP_TEXTURE_REPLACEMENTS=/path/to/replacements \
  ./build/sm64-us-debug/bin/sm64-us
# Compare with the originals.
RECOMP_TEXTURE_REPLACEMENTS= ./build/sm64-us-release/bin/sm64-us
```

Dump and replacement directories must differ. Reloading can briefly stall while
PNG uploads complete. Normal play only checks the directory at startup or F5;
`--texture-scan` adds the periodic background matcher. Bounded run results include counts
of PNGs written, replacement files indexed and replacement textures encountered.
