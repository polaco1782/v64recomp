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

`release/` contains the playable app and Release-only sources. `debug/` contains
the original trace/capture harness and inspection tools. Shared headers, adapters,
`generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

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

Set `RECOMP_FOV` to horizontal degrees at the original 4:3 reference aspect,
from `60` through `120`. Unset, empty or `0` preserves the original camera. Its
usual native vertical FOV is 60 degrees (about 75.2 degrees horizontal at 4:3).
The override preserves native zoom ratios and applies to normal-family 3D
gameplay cameras. Fixed/prerendered backgrounds, dialog, pause and scripted
cameras keep their original framing. RT64 F7 aspect expansion is separate.

```powershell
.\tools\build_msvc_project.ps1 -Game oot-us-v12 -Configuration Release
$env:RECOMP_FOV = '110'
.\build\msvc\oot-us-v12\release\bin\Release\oot-us-v12.exe
```

The adapter adjusts the view FOV after the original camera update, before the
shared CPU/GPU view-projection matrices are built. It leaves the native camera
FOV unchanged to avoid compounding zoom. See the
[investigation guide](../../docs/field-of-view.md#ocarina-of-time-investigation-record)
for addresses, ABI details and the pinned public reference.

## Focused control check

```powershell
.\tools\build_msvc_project.ps1 -Game oot-us-v12 -Configuration Debug
$env:RECOMP_OOT_CHECK_CONTROLS = '1'
.\build\msvc\oot-us-v12\debug\bin\Debug\oot-us-v12.exe --frames 1
Remove-Item Env:RECOMP_OOT_CHECK_CONTROLS
```

MSVC Debug and Release builds and the focused Debug check passed. The check
covers synthetic action mapping, camera/collision ABI, targeting,
state exclusions, FOV conversion and native zoom ratios. In-game movement,
collision, aiming, camera transitions, edge visibility and combined F7 behavior
still require gameplay testing.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
