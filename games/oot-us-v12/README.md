# Ocarina of Time US V1.2

Original title, file select, name entry and new-game opening with verified decompressed code and 469 relocatable overlays. Zelda NEAD audio and JPEG image tasks run recompiled RSP microcode. Inventory and full-game validation remain incomplete.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/oot-us-v12/profile.toml --rom /path/to/rom`.

```sh
python3 tools/build.py oot-us-v12 --release
./build/oot-us-v12-release/bin/oot-us-v12

python3 tools/build.py oot-us-v12 --debug
./build/oot-us-v12-debug/bin/oot-us-v12 --help
```

`main.c` contains the shared entry point and lifecycle for Debug and Release.
Headers, adapters, `generate.py`, `profile.toml`, inventories and maintained
replay inputs live beside it. Generated code stays under the selected build
directory; private ROMs and saves stay in `local/`.

The guest CPU entropy-decodes JPEG data. The recompiled RSP program takes the
resulting coefficients and quantization tables and writes RGBA5551 tiles.
On the Temple of Time exit, all 75 captured tasks matched the cxd4 RSP
interpreter across the entire 8 MiB of RDRAM after each task. Debug reports
count completed tasks as `jpeg_tasks`.

## GLES2 renderer

Build and run the desktop Debug configuration:

```sh
python3 tools/build.py oot-us-v12 --debug --gles
./build/oot-us-v12-gles-debug/bin/oot-us-v12
```

The ROM profile verifies the original RSP boot program, the F3DZEX2 task text
and data, and the F3DZEX2 and S2DEX2 instruction images used for IMEM loads.
The images come from the pinned ROM and its decompressed `.code`, not from
a separate microcode download. The original boot and the cartridge's
observed restart jump are fingerprinted separately. The JPEG task executes either bootstrap and the ROM’s recompiled JPEG microcode.
Desktop controls and native audio are unchanged.

A native GLES replay of the first save slot reproduced horizontal bands outside
the Temple of Time. S2DEX2 copies this background in six-row strips with zero
tile bounds and 15-bit wrap masks. The renderer now uses the rectangle coordinates
to size those textures. Replaying the 320×240 copy against the decoded
background in guest RAM reproduced all 76,800 RGBA5551 pixels.

## Keyboard and mouse

Default [WASD/mouse controls](controller.md) provide native camera-relative
movement, third-person mouse orbit and adventure action bindings. Right mouse
is Z-targeting, left mouse is the sword, E/Space is A and Ctrl is the shield.
Arrow keys map to the matching C-buttons, including while playing the ocarina;
WASD navigates menus and Enter supplies A when mouse capture is released.
Mouse orbit currently covers the standard follow camera; first-person aiming
and special camera modes retain native behavior. Set `RECOMP_OOT_MOUSE=0` to use
the original mapping.

Steam Deck packages use [dual-stick controls](controller.md#steam-deck-and-gamepad-camera):
left stick for movement, right stick for third-person camera positioning, and
the top-left D-pad for C-buttons and ocarina notes. This mode uses native
gamepad axes and works without mouse capture. Set `RECOMP_OOT_GAMEPAD_CAMERA=1`
to enable it on a desktop gamepad.

## Field of view

For widescreen, resize the GLES window to 16:9 and press **F7** while playing.
Press F7 again for 4:3. The normal 3D gameplay camera shows more of the world
on the left and right at the same vertical zoom. Window ratios above 4:3
through 21:9 are supported. File select, pause, dialogue and fixed-background
cameras use 4:3. The HUD shares the expanded framebuffer and can look wider.

Use keypad `+` / `-` to adjust FOV live, one horizontal degree per keydown;
holding a key repeats. The minimum is the exact original view (usually 60
degrees vertically, about 75.2 horizontally at 4:3), and the maximum is 140
degrees. Each launch starts at the original FOV; lowering to the minimum restores
its original path. No FOV environment variable is read. Changes print the current
angle to the console. This works independently of mouse capture.

Native zoom ratios are preserved. The override applies to normal-family 3D
gameplay cameras; fixed/prerendered backgrounds, dialog, pause and scripted
cameras keep their original framing.

The adapter adjusts the view FOV after the original camera update, before the
shared CPU/GPU view-projection matrices are built. It leaves the native camera
FOV unchanged to avoid compounding zoom. See the
[investigation guide](../../docs/field-of-view.md#ocarina-of-time-investigation-record)
for addresses, ABI details and the pinned public reference.

Desktop GLES Debug and Release replays toggled between 4:3 and 16:9 in the
Temple of Time. The doorway and floor aligned in the center of both frames,
with additional architecture visible on either side in widescreen. Release
also exercised keypad FOV from 75.2° to 87.2° and back while F7 was active.
The 140° limit and the full range of camera transitions still need gameplay coverage.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
