# Quake 64 US

This fork uses the C11 GLES2 renderer and statically recompiled RSP graphics
programs directly. Existing presets/workflows need no renderer flags. See the
[graphics notes](../../docs/armv8-gles2.md) for this ROM's acceptance boundary.

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


The [controller and camera guide](controller.md) records exact ROM addresses,
ABI/field evidence, input timing and visibility behavior. ROM discovery and
translation evidence passed with all originals retained. Linux Debug and Release
builds pass bounded first-level replays with the compiled RSP programs. Full-game
geometry and Windows Release acceptance remain pending.

`main.c` contains the shared entry point and lifecycle for Debug and Release.
Headers, adapters, `generate.py`, `profile.toml`, inventories and maintained
replay inputs live beside it. Generated code stays under the selected build
directory; private ROMs and saves stay in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).

Graphics tasks switch from F3DEX.NoN 1.21 to F3DLP.Rej 1.21 when starting play.
Both complete task identities and program/overlay spans are pinned in the profile.
The second identity was measured from the original ROM and the reported gameplay
transition crash at `0x80069140`; unknown microcode still fails explicitly.
The GLES2 renderer preserves the physical Z image across color-buffer and task
changes and uses 24-bit depth when supported. The bounded corridor replay renders
the world, shotgun and HUD; broader checks for missing or flickering geometry
remain necessary.
