# Native texture scanning

Pass a Rice/GLideN64 PNG pack ZIP or an extracted directory to the matching game:

```sh
./build/sm64-us-release/bin/sm64-us --texture-scan /home/cmartin/Downloads/NINTEMOD_Mario_64_Complete.zip
./build/duke-us-release/bin/duke-us --texture-scan /path/to/duke-pack.zip
./build/goldeneye-us-release/bin/goldeneye-us --texture-scan /path/to/extracted-ge-pack
```

Both release and debug runners support scanning. Rice hashing is linked into
the game and runs directly on the native scanner thread. No helper executable,
Python texture importer, or separate conversion/packing command is needed.
The project is licensed under GPL version 2; the original Rice/GLideN64 and RT64
notices are retained in [the third-party notices](../THIRD_PARTY_NOTICES.md).
The RT64 checkout includes a capture callback used by the background writer.
The capture writer, importer, ZIP reader, and pack writer live in this folder.

With no directory or ZIP argument, `--texture-scan` reads
`local/textures/<game>/source/`. ZIP entries are read directly without extracting
the source pack. Original ZIPs and extracted source images remain untouched.
The running game's ROM header selects the filename prefix to match, so Mario
textures cannot be imported into GoldenEye by accident.

## What happens while playing

RT64 captures raw texture memory and load information as textures are drawn.
A capture worker writes immutable raw snapshots. A separate native scanner
thread calculates Rice hashes, matches source images, and updates the pack
immediately and then every 60 seconds.
Capture writes, hashing, source image decoding, and packing run outside the renderer.
The graphics worker only copies bounded amounts of guest memory into a bounded,
nonblocking queue; a full queue defers discovery until a later draw. Atomic
completion markers let the scanner read captures without locking graphics.
Matching runs at lower CPU priority, retains one decoded image at a time, and
skips decoding unchanged inputs and outputs on subsequent scans.

Matched PNGs are stored inside a single RT64 pack:

```text
local/textures/<game>/replacements/textures.rtz
local/textures/<game>/replacements/rt64.json
```

The archive contains hash-named textures such as `0123456789abcdef.v5.png` and
its own `rt64.json` index, so the `.rtz` is self-contained. The external JSON is
a readable copy of that index. PNG data is already compressed and is stored
without additional compression to keep background packing work small. PNG packs
use RT64's stall loading operation; a DDS low-mip streaming cache is not needed.

When matches change, the scanner writes a new archive and atomically publishes
it before requesting an RT64 reload. The renderer can continue reading its
previously mapped archive during packing. Unchanged scans do not rebuild it.
Existing converted hash-named PNGs are migrated into the archive and removed
only after publication and mapping persistence succeed. Source images are kept.

The runner loads `textures.rtz` automatically from the replacement folder, even
without `--texture-scan`. A self-contained pack can also be loaded directly:

```sh
RECOMP_TEXTURE_REPLACEMENTS=/path/to/textures.rtz ./build/sm64-us-release/bin/sm64-us
```

## Image matching

The matcher accepts `_all.png`, `_ciByRGBA.png`, and `_allciByRGBA.png` images,
as well as split `_rgb.png` and `_a.png` images. A complete RGB/mask pair is
combined into one RGBA PNG using the mask's red channel as alpha. Dimensions
must match. An unpaired RGB image becomes opaque. Complete pairs take
precedence over combined variants, and combined variants over unpaired RGB.
This follows the split-mask convention in
[GLideN64's loader](https://github.com/gonetz/GLideN64/blob/master/src/GLideNHQ/TxHiResLoader.cpp).

The matcher supports palette and texture `$` wildcards. Its saved mappings let
images from earlier move-based imports match later RT64 hashes and palette
variants, including after migration to an archive. Ordinary filenames beat
corresponding `Copy of ` and `flash ` variants; pixel-identical duplicates are
accepted. Conflicting ordinary images produce an error for manual review.

Automatic reload preserves the original/replacement mode selected with F5.
F5 still reloads and toggles manually. GPU uploads can briefly stall during
reload. Ordinary launches do not capture or scan textures.

The following files persist under each game's texture directory:

- `scan/`: RT64 raw dumps, complete `ready/` snapshots, and their hash database.
  Hashing errors go to the game log. These are capture data, not extracted source images.
- `replacements/`: `textures.rtz`, `rt64.json`, and `.texture-scan.json`, which
  preserves reusable source mappings. Keep this mapping when continuing a scan.
- `source/`: original images, if using the default source directory.

Existing captures and mappings are reused in later runs. Only drawn textures
can be discovered, and Rice matching can differ between plugins; unmatched
textures retain the originals. RT64 can lack Rice source-load information for
interior mip levels loaded in one block; these are skipped without hashing an
unset source address. Source, dump, and replacement paths must be
separate. The scan destination must be a directory; unrelated indexed RT64
packs are protected from overwriting. `RECOMP_TEXTURE_REPLACEMENTS` overrides
that directory, and `RECOMP_TEXTURE_SCAN_RAW` overrides the capture directory.
