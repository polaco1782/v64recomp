# Shadow Man US

This fork uses the C11 GLES2 renderer and statically recompiled RSP graphics
programs directly. Existing presets/workflows need no renderer flags. See the
[graphics notes](../../docs/armv8-gles2.md) for this ROM's acceptance boundary.

Original startup, audio, Controller Pak screen, main menu and rendered level
progression work in desktop runs. Movement and animation transitions have been
exercised, but later levels and a complete playthrough remain unverified. This
is still an incomplete port.


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

`main.c` contains the shared entry point and lifecycle for Debug and Release.
Headers, adapters, `generate.py`, `profile.toml`, inventories and maintained
replay inputs live beside it. Generated code stays under the selected build
directory; private ROMs and saves stay in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).

The maintained title/intro replay uses the shared Debug input format:

```sh
./build/shadowman-us-debug/bin/shadowman-us --frames 3600 --mute \
  --input games/shadowman-us/title-intro.txt --save /tmp/shadowman.eeprom \
  --crash-dir /tmp/shadowman-crashes --capture-every 600
```
