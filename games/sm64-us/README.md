# Super Mario 64 US

Working gameplay baseline, with Castle Grounds movement/jumping and the castle lobby transition previously validated. Broader courses, ending and prolonged runs still need validation.

An experimental GLES2 backend parses the game's original Fast3D display lists,
including scene, HUD and frame setup. Desktop Castle Grounds movement passes;
Pi 3 hardware is not tested. See [Fast3D GLES2](../../docs/armv8-gles2.md) for
build flags and validation limits. RT64 remains the default.

With GLES2 and `RECOMP_GLES2_FPS=60`, `graphics.c` records stable identities for
the title head's packed vertices after running the original Goddard converters.
The US-ROM wrappers at `0x801930d8` (size `0x34c`, positions/normals) and
`0x80193424` (size `0x118`, positions) are SHA-256 checked in `profile.toml`.
Both take a group in `a0`; the adapter follows group `+0x1c` membership links
and vertex `+0x44` packed-vertex links, verified against their full local MIPS
bodies. The reference names are `convert_gd_verts_to_Vn` and
`convert_gd_verts_to_Vtx` in
[n64decomp's skin.c](https://github.com/n64decomp/sm64/blob/9921382a68bb0c865e5e45eb594d9c64db59b1af/src/goddard/skin.c).
The build uses the ROM and checked profile, not that source tree. This adds
renderer metadata only: display-list generation, physics and audio keep their
original timing. At native FPS or with RT64, the wrappers only call the originals.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/sm64-us/profile.toml --rom /path/to/rom`.

```sh
cmake --workflow --preset sm64-us-release
./build/sm64-us-release/bin/sm64-us

cmake --workflow --preset sm64-us-debug
./build/sm64-us-debug/bin/sm64-us --help
```

Debug renderer reports include `display_frames` and
`display_elapsed_ns` to distinguish displayed frames from host presentations.

Configure either mode with `-DRECOMP_EMBED_ROM=ON` to include ROM content in the
executable and launch without the external ROM. SM64's complete checked inventory
lets packaging erase ordinary CPU text, native OS ranges and the unused IPL3
boot block while retaining data and RSP programs. See
[ROM packaging](../../docs/rom-packaging.md) for exact scope, validation, reports
and comparison with an original ROM through `--rom`.

`release/` contains the playable app and Release-only sources. `debug/` contains
the original trace/capture harness and inspection tools. Shared headers, adapters,
`generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).

The generated physics regression can run without launching the game. From a
Developer PowerShell after generating/building SM64:

```powershell
.\.venv\Scripts\python.exe games/sm64-us/debug/check_physics.py --build build/msvc/sm64-us/release --cc cl
```

It checks arithmetic shifts and the generated `SET_OBJ_PHYSICS` routine with
intrinsics enabled and disabled, at both compiler optimization levels. This
covers an MSVC optimized sign-extension failure that turned Goomba drag from
`10` into `-645.36`, causing runaway speed and an animation conversion trap in
Bowser in the Fire Sea. On Linux, select an existing SM64 build and `--cc clang`.
