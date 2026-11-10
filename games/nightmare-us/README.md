# Nightmare Creatures US

This fork uses the C11 GLES2 renderer and statically recompiled RSP graphics
programs directly. Existing presets/workflows need no renderer flags. See the
[graphics notes](../../docs/armv8-gles2.md) for this ROM's acceptance boundary.

Original title, menus, character selection and playable Chelsea with graphics, ABI1 audio and live input. Full-game progression remains unverified.

Mouse steering and WASD are enabled by default. W/S move forward/back; A/D use
the native side dodges. Left/right mouse strike/kick, Space jumps, Ctrl defends,
E uses an item, and the wheel selects inventory items. Horizontal mouse motion
turns the character with the original following camera; vertical free-look is
not added. Tab pauses; Esc/F4 release/recapture the mouse.

See [controller findings](controller.md) for controls, settings, ROM hooks and
state checks. The adapter has static ROM verification and Debug/Release build
coverage; its gameplay acceptance is left to user testing.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/nightmare-us/profile.toml --rom /path/to/rom`.

```sh
cmake --workflow --preset nightmare-us-release
./build/nightmare-us-release/bin/nightmare-us

cmake --workflow --preset nightmare-us-debug
./build/nightmare-us-debug/bin/nightmare-us --help
```

`main.c` contains the shared entry point and lifecycle for Debug and Release.
Headers, adapters, `generate.py`, `profile.toml`, inventories and maintained
replay inputs live beside it. Generated code stays under the selected build
directory; private ROMs and saves stay in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
