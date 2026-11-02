# Duke Nukem 64 US

Original menus and first level with RT64 graphics, ABI1 audio and live input. Full-game progression remains unverified.

Single-player WASD/mouse controls are enabled by default. See the
[controller guide](controller.md) for bindings, settings, ROM evidence and
validation. `RECOMP_DUKE_MOUSE=0` restores the ordinary controller path.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/duke-us/profile.toml --rom /path/to/rom`.

```sh
cmake --workflow --preset duke-us-release
./build/duke-us-release/bin/duke-us

cmake --workflow --preset duke-us-debug
./build/duke-us-debug/bin/duke-us --help
```

## Field of view

Use keypad `+` / `-` to adjust the single-player world-camera FOV live,
one horizontal degree per keydown; holding a key repeats. The minimum is the
exact original view (60 degrees vertically, about 75.2 horizontally at 4:3),
and the maximum is 140 degrees. Each launch starts at the original FOV. Lowering
to the minimum restores the original projection and visibility window. No FOV
environment variable is read. Changes print the current value to the console.

This works in Debug and Release independently of mouse capture. Multiplayer,
orthographic HUD/menu calls retain native behavior. RT64 F7 aspect expansion
is separate and can widen the displayed view further.

The verified `guPerspectiveF` wrapper updates both CPU float and GPU fixed
projection paths. World billboards receive the same focal-length ratio as the
projection, with anchors and HUD/weapon scaling preserved. The initial sector
window widens by `max(0, horizontal/2 + 1 - 35)` degrees on each side, retaining
native pitch overscan and recursive portal clipping. ROM bodies, operands and
call scope remain checked by generation; see the
[repeatable investigation guide](../../docs/field-of-view.md).

The world setup is at `0x80063bb8`; the original `guPerspectiveF` is at
`0x800c3130`. Its fixed-matrix path checks the saved parent return address so
unrelated SDK callers remain unchanged. The `scansector` wrapper at `0x8005962c`
widens only the first call from `drawrooms_`. The originals retain matrix
normalization, recursive portal clipping, visited lists and sorting.

Earlier FOV/input versions passed Debug/Release checks and the user's gameplay
test, including corrected billboard sizes at 110 degrees. The keypad/140-degree
extension has not been rebuilt or tested in game. Tilted views, portal traversal
and combined F7 expansion require gameplay acceptance.

`main.c` contains the shared entry point and lifecycle for Debug and Release.
Headers, adapters, `generate.py`, `profile.toml`, inventories and maintained
replay inputs live beside it. Generated code stays under the selected build
directory; private ROMs and saves stay in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
