# Quake 64 US

Original menu and playable first level, with movement, turning and shotgun firing previously observed. Full-game stability and saves remain unverified.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/quake-us/profile.toml --rom /path/to/rom`.

```sh
cmake --workflow --preset quake-us-release
./build/quake-us-release/bin/quake-us

cmake --workflow --preset quake-us-debug
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
native depth and camera transformations remain intact. RT64 F7 expansion is
independent of this 4:3 FOV setting.

The [controller and camera guide](controller.md) records exact ROM addresses,
ABI/field evidence, input timing and visibility behavior. ROM discovery and
translation evidence passed with all originals retained. No standalone tests
were added. The MSVC Debug build compiles and links; Release and gameplay
acceptance remain pending. The game was not launched.

`release/` contains the playable app and Release-only sources. `debug/` contains
the original trace/capture harness and inspection tools. Shared headers, adapters,
`generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
