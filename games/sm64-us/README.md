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

Shared renderer statistics distinguish displayed frames from host presentations;
Debug uses the common tracing, replay and capture options.

Configure either mode with `-DRECOMP_EMBED_ROM=ON` to include ROM content in the
executable and launch without the external ROM. SM64's complete checked inventory
lets packaging erase ordinary CPU text, native OS ranges and the unused IPL3
boot block while retaining data and RSP programs. See
[ROM packaging](../../docs/rom-packaging.md) for exact scope, validation, reports
and comparison with an original ROM through `--rom`.

`main.c` contains the shared entry point and lifecycle for Debug and Release.
Headers, adapters, `generate.py`, `profile.toml`, inventories and maintained
replay inputs live beside it. Generated code stays under the selected build
directory; private ROMs and saves stay in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).

Word shifts retain the fix for MSVC optimized sign extension that affected
packed physics parameters. The standalone physics regression harness and
scripted validation frontend have been removed; Debug now uses the common
frame loop, replay and capture options.

## Browser preview

See [WebAssembly build and validation](../../docs/webassembly.md) for the experimental Emscripten/GLES2 target.
