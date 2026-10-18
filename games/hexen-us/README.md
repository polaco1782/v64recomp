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
cmake --preset hexen-us-release
cmake --build --preset hexen-us-release -j 4
./build/hexen-us-release/bin/hexen-us

cmake --preset hexen-us-debug
cmake --build --preset hexen-us-debug -j 4
./build/hexen-us-debug/bin/hexen-us --help
```

The shared keyboard bindings use `S` for Start, `A` for the N64 A button, `Z` for
the trigger and arrow keys for the stick. Controller Pak storage uses
`local/saves/hexen-us.mpk`; initialization has been exercised, but persistence of
an actual game save still needs validation.

The bounded Debug replay selects a fighter, waits through loading, then moves,
turns and fires. An enemy can kill the player during this route.

```sh
./build/hexen-us-debug/bin/hexen-us --frames 4200 \
  --input games/hexen-us/debug/play-through.txt --capture-every 300 \
  --wav build/hexen-us-debug/play-through.wav
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
  --executable build/hexen-us-release/bin/hexen-us --frames 4200 \
  --input games/hexen-us/debug/play-through.txt \
  --output build/hexen-us-release/acceptance-play \
  --capture-at 29 --capture-at 49 --capture-at 65
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
