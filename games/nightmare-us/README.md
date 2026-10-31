# Nightmare Creatures US

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

`release/` contains the playable app and Release-only sources. `debug/` contains
the original trace/capture harness and inspection tools. Shared headers, adapters,
`generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
