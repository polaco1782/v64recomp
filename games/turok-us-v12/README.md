# Turok: Dinosaur Hunter US v1.2

This fork keeps the CPU port sources, but its GLES2 graphics integration still
needs a checked compiled RSP profile. The milestones below describe earlier CPU
port work and do not establish current graphics acceptance. See
[graphics status](../../docs/armv8-gles2.md).


## Build and run

From the repository root, import your own ROM once:

```sh
.venv/bin/python tools/import_rom.py --profile games/turok-us-v12/profile.toml \
  --rom '/path/to/Turok - Dinosaur Hunter (U) (V1.2).z64'
cmake --workflow --preset turok-us-v12-release
./build/turok-us-v12-release/bin/turok-us-v12
```

The normalized ROM is `local/turok.us.v12.z64`, size `0x800000`, SHA-1
`c7ed00dee20f4235823bcf50d32fa5d6862d6fce`. Other games and revisions are rejected.
Controller Pak data persists at `local/saves/turok-us-v12.mpk`.

Default [WASD/mouse controls](controller.md) provide native movement and strafing,
mouse look, left-click fire, Space/right-click jump, wheel/Q/R weapon selection
and F for the automap. Tab pauses, Escape releases the mouse, and F4 recaptures.
Controller fallback retains the previous host remap; Debug replay keeps original
N64 pad semantics. See the controller guide for sensitivity and scope.

Keypad `+` / `-` adjusts the gameplay camera's horizontal FOV from the original
approximately **60.8 degrees** to **140 degrees**, starting at the original each
launch. Projection and CPU visibility change together; native zoom/water effects
are tangent-scaled. Lowering to the minimum preserves the original path exactly.
F7 widescreen expansion is separate. The new controls/FOV have static ROM and
source-generation checks only; builds and gameplay tests are left to the user.

```sh
cmake --workflow --preset turok-us-v12-debug
./build/turok-us-v12-debug/bin/turok-us-v12 --frames 1800 \
  --input local/replays/turok-us-v12-title.txt --capture-every 150
```

Optional Debug captures are under
`build/turok-us-v12-debug/debug-output/turok-us-v12/`. Replay frames are presentation
indices, rather than guest game frames. `local/replays/turok-us-v12-play-through.txt` starts play and
exercises movement, firing, jumping, weapon selection, automap and pause/resume.
The bounded 3,000-presentation gameplay replay and 3,300-presentation Release
X11 replay completed; the final 55-second Release replay exercised the live host
remap and restored widescreen, including camera turning and the automap. These
are focused acceptance paths, not full-game coverage.
Nonzero PCM and SDL queue activity are verified, while listening quality remains
unverified. Use `--capture-every N` for optional periodic screenshots.


## Loading and checked boundaries

The IPL maps cartridge `0x1000..0x1f00` at `0x80000400`. Its custom PI word-copy
routine is bound by its entire verified body and reads the original cartridge.
The original guest RNC1 decoder expands the main program from cartridge
`0x783690` to `0x80001300`; decoded size is `0xf7e70`, SHA-256
`4edaabc08e405dac51733041ca4342eccfa0d355683207450a8e55dd5b85aec4`.
The analysis generator independently decodes that same image with packed and
unpacked CRC checks and appends it at analysis offset `0x800000`. Those analysis
offsets are never used as cartridge DMA offsets.

Live decoded bytes are fully checked before activating the main section. The
adapter suppresses scheduler-global publication during IPL decompression.
The ROM clears BSS at `0x800f9170..0x8013a280`; the synthetic IPL thread is
destroyed and joined at the verified main-thread handoff before its
RAM can be reused. The game uses 4 MiB RAM and its original 46.875 MHz counter.

The original RNC2 wrapper calls `0x80000c78`, also the terminal `jr ra` delay slot
of the preceding function. Shared discovery/metadata validation supports this
one-word ownership pattern without accepting other overlapping bodies.

Switch and function tables live in `profile.toml` with complete table hashes,
bounds, transfer words and reviewed indices. Callback roots in
`startup-entries.json` record their pointer producers and consumers and bind the
entry bytes to the exact prepared image. SDK native replacements require whole
body SHA-256 catalog matches. The original game code and RNC unpackers execute
as translated guest code.

## External references

- [Original N64 source mirror](https://github.com/Abhisheksinha1506/Turok-N64-Source-code),
  revision `f6ac62a9b9fc799bdb023dddaa440480814f9af6`. Its `src/PR/tengine/boot.c`
  describes the observed PI-copy and RNC load model. Its source/disassembly has
  a different function order and addresses from this retail revision. The
  `CGameObjectInstance__CinemaTurokArrowOrientation` callback was cross-checked
  against the retail `0x8003a950` body and actor `+0x184` producer/consumer, rather
  than importing the reference's `0x80039088` address.
- [colbyshores/turok-port](https://github.com/colbyshores/turok-port) describes a
  native PC/3DS port from original engine source, revision
  `da8f3ae26d6287ea5f69fa8f94bbbadae0ddff13`, using US v1.2 assets. Its README
  reports first-level geometry and animated-object rendering. This is a source
  port, not evidence of a matching retail decompilation; its runtime has not been
  built or independently accepted here.
- [Independent RNC implementation](https://github.com/lab313ru/rnc_propack_source),
  revision `499e8128f9a3890cb4d287a11608b3ae1e0df440`. Our prepared main image
  matched its method-1 decoder byte for byte. It is not a build dependency.


No external ROM or game assets were fetched. References remain private scratch
material; build/discovery depend only on this repository and the user's pinned ROM.

Replay commands above use private input files under `local/replays/`; create or
restore those recordings before running them. They are not shipped with the port.
