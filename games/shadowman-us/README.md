# Shadow Man US

Original startup, audio, Controller Pak screen, main menu and rendered level
progression work in desktop runs. Movement and animation transitions have been
exercised, but later levels and a complete playthrough remain unverified. This
is still an incomplete port.

Both configurations use native frame timing and VI-driven presentation. The
game's custom VI mode starts at framebuffer row zero, whereas RT64 estimates
the framebuffer base by subtracting a scanline. The shared renderer normalizes
its VI snapshot using the OS swap address for this case. This avoids displaying
an offset RAM fallback instead of the rendered GPU image; guest VI state is
preserved. Loading can still take a while, and loading/HUD images retain visible
grain that has not been established as matching original hardware.

The checked profile covers menu callbacks, sound-ID remapping, animation events
and transitions, and scene collision callbacks. Additional constant callbacks
passed through object traversals are recorded in the ROM-bound entry inventory.
Each addition was checked against its producer, consumer and ROM bytes; no
missing-function stubs are used. Keyboard traces confirm directional input
returns to `(0,0)` on release.

Validation on 2026-10-26: Debug and Release build successfully. The updated
Release completed a 6,000-presentation desktop run in 145.8 seconds with no
crash or timeout, with movement/action input and visible level captures. The
final Debug app completed 2,200 presentations through the opening cinematic,
with 4,383 graphics tasks, 4,754 audio tasks, nonzero PCM output and no audio
underruns or resets. Its replay driver flagged the uncaptured 90-second
screenshot as incomplete because the app finished its frame budget at 80 seconds.
The shared renderer also passed a 600-presentation GoldenEye startup smoke run.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/shadowman-us/profile.toml --rom /path/to/rom`.

```sh
cmake --workflow --preset shadowman-us-release
./build/shadowman-us-release/bin/shadowman-us

cmake --workflow --preset shadowman-us-debug
./build/shadowman-us-debug/bin/shadowman-us --help
```

`release/` contains the playable app and Release-only sources. `debug/` contains
the original trace/capture harness and inspection tools. Shared headers, adapters,
`generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
