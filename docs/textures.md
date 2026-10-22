# Texture dumps and replacements

Pass an original Rice/GLideN64 ZIP directly to RT64 in either Debug or Release:

```sh
./build/sm64-us-debug/bin/sm64-us --texture-pack /path/to/pack.zip
```

RT64 indexes the ZIP's filenames once, matches textures as they are drawn, and
streams the matching PNGs directly from that same archive. The ZIP stays
unchanged. No extraction, capture files, generated RTZ, or packing step is needed.
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
# Custom dump and replacement directories (debug only for dumping).
RECOMP_TEXTURE_DUMP=/path/to/originals RECOMP_TEXTURE_REPLACEMENTS=/path/to/replacements \
  ./build/sm64-us-debug/bin/sm64-us
# Compare with the originals.
RECOMP_TEXTURE_REPLACEMENTS= ./build/sm64-us-release/bin/sm64-us
```

Dump and replacement directories must differ. Reloading can briefly stall while
PNG uploads complete. Loose replacement directories are checked at startup or F5;
ZIP matching runs in RT64 as textures are encountered. Bounded run results include counts
of PNGs written, replacement files indexed and replacement textures encountered.
