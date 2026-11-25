# Quake 64 US

Original menu and playable first level, with movement, turning and shotgun firing previously observed. Full-game stability and saves remain unverified.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/quake-us/profile.toml --rom /path/to/rom`.

```sh
python3 tools/build.py quake-us --release
./build/quake-us-release/bin/quake-us

python3 tools/build.py quake-us --debug
./build/quake-us-debug/bin/quake-us --help
```

Default [WASD/mouse controls](controller.md) provide native movement, direct
mouse look, left-click fire, Space/right-click jump and wheel weapon cycling.
Tab pauses; Escape releases the mouse and F4 recaptures. Menus, inactive
gameplay and multiplayer retain original controls. Mouse sensitivity/inversion
use the shared settings; `RECOMP_QUAKE_MOUSE=0` restores the original mapping.

Keypad `+` / `-` adjusts horizontal FOV live from the original **90 degrees** to
**140 degrees**, one degree per keydown with hold-to-repeat. Each launch starts
at the original FOV; lowering to the minimum restores it exactly. No FOV
environment variable is read. CPU clip planes expand with the GPU projection;
native depth and camera transformations remain intact.

## Ray-traced reflections

Launch with `--rt-reflections` and look down at the floor in the first level.
Floors and upward-facing slopes reflect the static level, including walls and
ceilings outside the camera view. The CPU traces one bounce per sample at
128×96 using the game's camera, textures and baked lighting. The GLES renderer
checks the floor depth before blending the reflection into the final frame.

This mode covers single-player world geometry. Monsters, pickups and moving
doors are outside the traced scene. The separate `--ssr` option uses visible
screen color and depth.

On the US ROM the world draw at `0x80025a08` reads the model through
`0x8008da00`. The adapter walks its BSP nodes at model offset `0x8c` and the
world submodel face range at `+0x58/+0x5c`. Each face points to its resident
polygon at `+0x24`, texture mapping at `+0x28` and lightmap at `+0x2c`.

## Port notes

The [controller and camera guide](controller.md) records exact ROM addresses,
ABI/field evidence, input timing and visibility behavior. ROM discovery and
translation evidence passed with all originals retained. No standalone tests
were added. The MSVC Debug build compiles and links; Release and gameplay
acceptance remain pending. The game was not launched.

`main.c` contains the shared entry point and lifecycle for Debug and Release.
Headers, adapters, `generate.py`, `profile.toml`, inventories and maintained
replay inputs live beside it. Generated code stays under the selected build
directory; private ROMs and saves stay in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
