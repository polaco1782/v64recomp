# Native texture scanning

Extract a Rice/GLideN64 PNG pack, then run the matching game's native executable:

```sh
./build/sm64-us-release/bin/sm64-us --texture-scan /path/to/extracted-mario-pack
./build/duke-us-release/bin/duke-us --texture-scan /path/to/extracted-duke-pack
./build/goldeneye-us-release/bin/goldeneye-us --texture-scan /path/to/extracted-ge-pack
```

Both release and debug runners support scanning. Build the selected runner
normally with CMake; this also builds `bin/texture_hasher` beside it. Keep that
native helper beside the game if moving the executables. No Python texture
importer, generated pack, or separate conversion command is needed.

With no directory argument, `--texture-scan` reads
`local/textures/<game>/source/`. For example:

```sh
mkdir -p local/textures/sm64-us/source
unzip -q /home/cmartin/Downloads/NINTEMOD_Mario_64_Complete.zip \
  -d local/textures/sm64-us/source
./build/sm64-us-release/bin/sm64-us --texture-scan
```

The directory must already be extracted; the runner does not unpack ZIP files.
The running game's ROM header selects the filename prefix to match, so Mario
textures cannot be imported into GoldenEye by accident.

## What happens while playing

RT64 captures raw texture memory and load information as textures are drawn.
A native worker takes complete snapshots, calls RT64's native Rice hasher,
and scans the source directory immediately and then every 15 seconds. The
helper preserves RT64's existing separation of its GPL Rice implementation.
Hashing and source image matching run outside the renderer. Snapshot copying
briefly synchronizes with the graphics worker so files are never hashed while
RT64 writes them.

Matched `_all.png`, `_ciByRGBA.png`, and `_allciByRGBA.png` images are **moved** into
`local/textures/<game>/replacements/` with RT64 names such as
`0123456789abcdef.v5.png`. PNGs are checked before import. An image is published
atomically, its reusable mapping is saved, and then the original is removed.
Keep the original ZIP if you want to restore the extracted images later.

Rice's split `_rgb.png` and `_a.png` format is also supported. A complete pair
is combined into one RGBA PNG using the mask's red channel as alpha; both
source files are consumed after the result and mapping are saved. Dimensions
must match. An unpaired `_rgb.png` becomes opaque. Complete pairs take
precedence over combined variants, and combined variants over unpaired RGB.
Persisted mappings retain that preference after originals have been moved, so
a leftover RGB file cannot erase an already imported texture's transparency.
This follows the split-mask convention in
[GLideN64's loader](https://github.com/gonetz/GLideN64/blob/master/src/GLideNHQ/TxHiResLoader.cpp).

New replacements trigger a synchronized RT64 reload on the host thread without
toggling originals/replacements. F5 still reloads and toggles manually; automatic
reload preserves the mode selected with F5. Reloading can briefly stall during
GPU uploads. No source scanning or raw capture happens in an ordinary launch.

The native matcher supports palette and texture `$` wildcards. Its mapping file
allows an already moved image to match a later RT64 hash or palette variant.
Ordinary filenames beat corresponding `Copy of ` and `flash ` variants; pixel
identical duplicates are accepted. Conflicting ordinary images remain in the
source directory and produce an error for manual review.

The following files persist under each game's texture directory:

- `scan/`: RT64 raw dumps, complete `ready/` snapshots, their `rt64.json` hash
  database, and `hasher.log`.
- `replacements/`: hash-named PNGs and `.texture-scan.json`, which preserves
  aliases to moved source images. Keep this mapping when continuing a scan.
- `source/`: unmatched images, when using the default source directory.

Existing captures and mappings are reused in later runs. Replacement PNGs also
load without `--texture-scan`. Only drawn textures can be discovered, and Rice
matching can differ between plugins; unmatched textures retain the originals.
Source, dump, and replacement directories must be separate. A scan destination
must be a hash-named replacement folder rather than an indexed RT64 pack.
`RECOMP_TEXTURE_REPLACEMENTS` overrides the destination and
`RECOMP_TEXTURE_SCAN_RAW` overrides the raw capture directory.

## Native checks

```sh
cmake --preset goldeneye-us-debug -DRECOMP_BUILD_TEXTURE_SCAN_TESTS=ON
cmake --build build/goldeneye-us-debug --target check-texture-scan -j 4
```

These checks use temporary image fixtures, do not launch a game or desktop, and cover
file moves, persistent wildcard aliases, split RGB/mask composition, format
preference after moving files, duplicate names, wrong ROM prefixes, output
isolation, reload notifications, and thread shutdown.
