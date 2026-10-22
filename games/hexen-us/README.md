# Hexen US

Hexen US revision 0 boots through the title and class menus into the first level.
Debug and Release desktop runs have exercised fighter selection, movement,
turning, combat, damage and the death screen. This establishes an initial playable
path; other classes, hub progression and in-game save/reload remain unverified.

The pinned big-endian ROM is 8 MiB, with SHA-1
`a602839132f6cca71f175fb72039897705ba4661`. Runtime uses 4 MiB of RAM.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/hexen-us/profile.toml --rom /path/to/rom`.

```sh
cmake --workflow --preset hexen-us-release
./build/hexen-us-release/bin/hexen-us

cmake --workflow --preset hexen-us-debug
./build/hexen-us-debug/bin/hexen-us --help
```

The shared keyboard bindings use `S` for Start, `A` for the N64 A button, `Z` for
the trigger and arrow keys for the stick. With the default Control Stick scheme,
`I` (C-Up) toggles the automap; `C`/`V` (L/R) zoom it. These map controls follow
the [instruction booklet](https://manuals.plus/m/7baa08667bd9411710be5c41d9b90f74bf8673035ef47cbf69759d3703df7af0.pdf).
Controller Pak storage uses
`local/saves/hexen-us.mpk`; initialization has been exercised, but persistence of
an actual game save still needs validation.

The bounded automap replay selects a fighter, waits through loading, opens the
map, zooms it, moves, and toggles between the map and 3D gameplay twice.

```sh
./build/hexen-us-debug/bin/hexen-us --frames 4500 \
  --input games/hexen-us/debug/automap.txt --capture-every 120 \
  --wav build/hexen-us-debug/automap.wav
```

The final 70-second Debug run completed with 1,509 graphics tasks, 3,978 audio
tasks and 2,911,929 nonzero audio samples. The audio backend recorded four queue
underruns and zero resets. PCM generation is verified; listening quality has not
been assessed. The inventory currently translates 1,431 functions (54 native
hooks) without reported instruction or transfer errors. It remains partial.
Both a 70-second Release keyboard replay and a separate 70-second idle
attract-mode run exited successfully after the callback fixes. The latter
replayed the script path that previously failed at `0x800dff00`.

For Release keyboard replay and window captures, use the shared desktop tool:

```sh
.venv/bin/python tools/desktop_replay.py \
  --executable build/hexen-us-release/bin/hexen-us --frames 4500 \
  --input games/hexen-us/debug/automap.txt \
  --output build/hexen-us-release/acceptance-map \
  --capture-at 56 --capture-at 58 --capture-at 62 --capture-at 65 --capture-at 73
```

This requires an X11/Xwayland desktop and ImageMagick `import` for captures.
Its keyboard input follows wall time and quantizes stick movement, so it
approximates the Debug replay. Inspect `run.json`, `run.log` and the captures;
an idle attract-mode run is useful too because it exercises different scripts.

`profile.toml` describes the initial image, DMA-loaded support code and LZH-decoded
game image. The original guest decoder runs, and the adapter verifies its output
against the prepared analysis image. Checked discovery includes all 2,846 actor
state records, the 102 script handlers and six path-traversal callbacks. Missing
traversal target `0x800f4dc4` and script target `0x800dff00` were fixed at these
registration sources. Fatal failures and optional `RECOMP_HEXEN_SNAPSHOT=1`
diagnostics use the shared crash reporter.

The sky pass first draws textured clouds, writes sky-sector depth masks, and
then clears the remaining background with a maximum-primitive-depth rectangle.
The shared RT64 build corrects primitive Z from inclusive UNORM to unsigned
0.15 fixed point, so that rectangle passes `LESS` against cleared depth outside
the sky masks. See [the shared renderer correction](../../docs/recompilation.md).
`debug/sky-depth.txt` reaches the first level, backs into the courtyard and turns
for a focused rendering replay:

```sh
./build/hexen-us-debug/bin/hexen-us --frames 3300 \
  --input games/hexen-us/debug/sky-depth.txt
```

Debug and Release passed 55-second first-level runs with movement and turning.
Captured-task isolation verifies the sky, depth-mask and background-clear passes
separately. Later levels and other games have not been checked for this change.

The automap uses L3DEX 1.21 at physical `0x66490`. RT64's database identifies
this version but marks its implementation `Unknown`; its L3DEX2 line handler
also has no implementation. `graphics.cpp` binds only Hexen's verified L3DEX
text/data, reuses ordinary F3DEX commands and renders LINE3D segments as
screen-space triangles through RT64's RDP path. The game retains its colors,
scissor, combiner and blending. Other microcodes use RT64's normal lookup.
Unverified lighting, fog, perspective lines and triangle opcodes fail explicitly.
The full text and data fingerprints match original ROM `0x1c090..0x1d090`
and `0x20530..0x20d30`; the reported failing task contained 424 lines.
Both builds passed 75-second desktop runs with visible automap lines and repeated
returns to 3D gameplay. Release captures also verified zoom in/out and movement
while the map was open. These focused runs do not establish later hub progression.

Inspect another captured RSP task without writing a snapshot decoder:

```sh
.venv/bin/python tools/inspect_rsp_task.py local/crashes/hexen-us/crash-<id>.json \
  --rom local/hexen.us.z64 --list-scale 8 --opcode 0xb5 --commands 20 \
  --extract build/hexen-us-report/rsp-task
```

Hexen stores graphics command counts in the task size field, hence the scale of
eight. The opcode histogram covers the supplied list span, without following
nested lists. Extracted microcode/display-list bytes remain private build data.

Use the maintained tools to investigate another callback without writing a ROM
scanner. These commands prepare and verify the analysis image automatically:

```sh
.venv/bin/python tools/disassemble_function.py recomp_s2_800f7a74 \
  --game hexen-us --metadata build/hexen-us-debug/generated/hexen-us/symbols.json
.venv/bin/python tools/inspect_callbacks.py --game hexen-us \
  --metadata build/hexen-us-debug/generated/hexen-us/symbols.json \
  --target 0x800f4dc4 --target 0x800f7b98
.venv/bin/python tools/inspect_callbacks.py --game hexen-us \
  --metadata build/hexen-us-debug/generated/hexen-us/symbols.json \
  --target 0x800dff00 --data .game:0x80130e70:0x80131008
```

`release/` and `debug/` have independent app entry points. Shared headers,
adapters, `generate.py`, `profile.toml` and inventories live in this parent folder.
Generated C stays in the selected build directory; private ROM/save data stays in
`local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md), and [support status](../../docs/status.md).
