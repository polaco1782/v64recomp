# Ocarina of Time US V1.2

Original title, file select, name entry and new-game opening with verified decompressed code and 469 relocatable overlays. Zelda NEAD audio and native JPEG SP decoding are supported. Inventory and full-game validation remain incomplete.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/oot-us-v12/profile.toml --rom /path/to/rom`.

```sh
cmake --workflow --preset oot-us-v12-release
./build/oot-us-v12-release/bin/oot-us-v12

cmake --workflow --preset oot-us-v12-debug
./build/oot-us-v12-debug/bin/oot-us-v12 --help
```

`main.c` contains the shared entry point and lifecycle for Debug and Release.
Headers, adapters, `generate.py`, `profile.toml`, inventories and maintained
replay inputs live beside it. Generated code stays under the selected build
directory; private ROMs and saves stay in `local/`.

JPEG tasks decode the game's coefficient buffers into RGBA5551 tiles before
posting SP completion. The native decoder uses the pinned Mupen64Plus RSP-HLE
implementation and verifies the boot, code and data images against this ROM's
SHA-256 profile. Link's house background was checked through the original opening;
other JPEG rooms and full-game progression still need validation. Debug reports
count completed tasks as `jpeg_tasks`.

## Keyboard and mouse

Default [WASD/mouse controls](controller.md) provide native camera-relative
movement, third-person mouse orbit and adventure action bindings. Right mouse
is Z-targeting, left mouse is the sword, E/Space is A and Ctrl is the shield.
Mouse orbit currently covers the standard follow camera; first-person aiming
and special camera modes retain native behavior. Set `RECOMP_OOT_MOUSE=0` to use
the original mapping.

## Field of view

Use keypad `+` / `-` to adjust FOV live, one horizontal degree per keydown;
holding a key repeats. The minimum is the exact original view (usually 60
degrees vertically, about 75.2 horizontally at 4:3), and the maximum is 140
degrees. Each launch starts at the original FOV; lowering to the minimum restores
its original path. No FOV environment variable is read. Changes print the current
angle to the console. This works independently of mouse capture.

Native zoom ratios are preserved. The override applies to normal-family 3D
gameplay cameras; fixed/prerendered backgrounds, dialog, pause and scripted
cameras keep their original framing. RT64 F7 aspect expansion is separate.

The adapter adjusts the view FOV after the original camera update, before the
shared CPU/GPU view-projection matrices are built. It leaves the native camera
FOV unchanged to avoid compounding zoom. See the
[investigation guide](../../docs/field-of-view.md#ocarina-of-time-investigation-record)
for addresses, ABI details and the pinned public reference.

The keypad/140-degree extension has not been rebuilt or tested in game. In-game
movement, collision, aiming, camera transitions, edge visibility and combined F7
behavior still require gameplay acceptance.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
