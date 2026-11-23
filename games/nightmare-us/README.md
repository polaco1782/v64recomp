# Nightmare Creatures US

Original title, menus, character selection and playable Chelsea with graphics, ABI1 audio and live input. Full-game progression remains unverified.

The original effect code can submit vertices from the previous frame. In a
captured GLES2 task, this caused a 64-byte RSP DMA read at `0x00e96850`,
beyond installed RDRAM. The shared RSP runtime supplies zeroes for that read.
The unchanged display list reached RDP full sync after 586,109 RSP instructions
in both Debug and Release. A 5,800-presentation Debug replay also reached
Chelsea and exited normally.

Mouse steering and WASD are enabled by default. W/S move forward/back; A/D use
the native side dodges. Left/right mouse strike/kick, Space jumps, Ctrl defends,
E uses an item, and the wheel selects inventory items. Horizontal mouse motion
turns the character with the original following camera; vertical free-look is
not added. Tab pauses; Esc/F4 release/recapture the mouse.

See [controller findings](controller.md) for controls, settings, ROM hooks and
state checks. The adapter has static ROM verification and Debug/Release build
coverage; its gameplay acceptance is left to user testing.

## Adjustable field of view

Keypad **+ / −** widens the horizontal field of view from the stock 4:3 view
(about 66°) to 140° during active gameplay. It works independently of the mouse
setting. Each launch starts at the original angle; lowering to the minimum
uses the unchanged cartridge path. Menus and inactive cameras are excluded.

The game-local adapter changes the verified 52° vertical argument at the
original fixed-perspective call made by display-list setup (`0x80074d24`). CPU
matrix readers and renderer commands still consume the same game-built matrix
bank. Roll, depth range, camera movement and original controller input are
unchanged. ROM identity, caller and consumer are checked at generation time.

Debug GLES2 completed a bounded offscreen title-screen smoke and RT64 Release
built. The wider view still needs gameplay and edge-visibility acceptance on a
desktop; title-screen output alone does not establish this.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/nightmare-us/profile.toml --rom /path/to/rom`.

```sh
python3 tools/build.py nightmare-us --release
./build/nightmare-us-release/bin/nightmare-us

python3 tools/build.py nightmare-us --debug
./build/nightmare-us-debug/bin/nightmare-us --help
```

`main.c` contains the shared entry point and lifecycle for Debug and Release.
Headers, adapters, `generate.py`, `profile.toml`, inventories and maintained
replay inputs live beside it. Generated code stays under the selected build
directory; private ROMs and saves stay in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
