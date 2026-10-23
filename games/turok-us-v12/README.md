# Turok: Dinosaur Hunter US v1.2

Experimental native recompilation of the US header-revision-2 cartridge. CPU
coverage is partial; undiscovered executable targets fail explicitly. Debug and
Release build. Original startup, title/menu selection and first-person gameplay
run through RT64 with controller input and ABI1 audio synthesis. Movement,
firing, weapon selection, death/respawn and pause/resume have been exercised.
Full-game progression, save/reload and audible quality remain unverified.

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

Live input maps arrows/left stick to forward/backward movement and analog
left/right turning. I/K/right-stick vertical look up/down; J/L/right-stick
horizontal strafe. Only the vertical stick and C-button directions are swapped
in the game adapter. Forward/backward movement uses a 20-unit stick threshold;
vertical looking uses an 80-unit digital stick range. Horizontal turning keeps
the host stick's analog value. Other buttons retain their shared bindings.
Debug replay files retain original N64 button/stick semantics and bypass this
host remap.

```sh
cmake --workflow --preset turok-us-v12-debug
./build/turok-us-v12-debug/bin/turok-us-v12 --frames 1800 \
  --input games/turok-us-v12/debug/title.txt --capture-every 150
```

Debug reports/captures are under
`build/turok-us-v12-debug/debug-output/turok-us-v12/`. Replay frames are presentation
indices, rather than guest game frames. `debug/play-through.txt` starts play and
exercises movement, firing, jumping, weapon selection, automap and pause/resume.
The bounded 3,000-presentation gameplay replay and 3,300-presentation Release
X11 replay completed; the final 55-second Release replay exercised the live host
remap and restored widescreen, including camera turning and the automap. These
are focused acceptance paths, not full-game coverage.
Nonzero PCM and SDL queue activity are verified, while listening quality remains
unverified. Set `RECOMP_TUROK_CAPTURE_FROM` to a presentation index to bound capture
output during a focused late-game diagnostic.

Turok uses the shared expanded widescreen viewport and 60 FPS interpolation. Disabling interpolation or widescreen did not fix
the starting-camera fog flashes. The verified cause was RT64's combined
view/projection decomposition dividing by a zero or nearly zero X component.
The shared RT64 integration now pivots on the largest view-axis component.
The user confirmed the fog correction in Turok and reported improved GoldenEye
fog as well. Quantized camera matrices and the captured Turok starting pose were
checked against the actual shared build at O0 and O2.

The automap switches to L3DEX 1.00 at `0x800b4ca0` with data `0x800f86b0`.
RT64 recognizes this version but does not implement its command decoder. Treating
it as a usable decoder skips `G_ENDDL` and runs beyond the submitted list, eventually
causing `invalid RT64 DP receipt`. The game-local `graphics.cpp` adapter checks the
complete task text/data fingerprints, decodes the ordinary F3DEX commands and
renders the map's line commands through RT64's RDP triangle path. Its orthographic
projection uses constant homogeneous W=32; screen coordinates already divide by W.
Perspective, lighting and fog line modes still fail explicitly. Core task token
and duplicate completion checks remain enabled.

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
adapter suppresses scheduler-global publication during IPL decompression. Debug
poisons and checks the original BSS clear at `0x800f9170..0x8013a280`; the synthetic
IPL thread is destroyed and joined at the verified main-thread handoff before its
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

- [PaxDino RT64 projection analysis](https://github.com/wootbeer/PaxDino/blob/1ccc166419539ac069b2cac32ae912798b39466e/lib-patches/rt64/0001-expand-exact-view-projection.patch)
  identified the unstable decomposition calculation. Our independently implemented
  correction chooses the largest axis as its division pivot. The local regression
  failed against the original pinned RT64 code and passed at O0/O2 after correction.
- [GLideN64 L3DEX decoder](https://github.com/gonetz/GLideN64/blob/master/src/uCodes/L3DEX.cpp)
  documents the line index/width encoding. The existing Hexen adapter supplied the
  repository implementation pattern; both text/data spans were verified against
  the original decoded Turok ROM and the user's crash RAM before reuse.

No external ROM or game assets were fetched. References remain private scratch
material; build/discovery depend only on this repository and the user's pinned ROM.
