# Super Mario 64 US

Working gameplay baseline, with Castle Grounds movement/jumping and the castle lobby transition previously validated. Broader courses, ending and prolonged runs still need validation.

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
