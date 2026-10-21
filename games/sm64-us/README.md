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

Both builds target 60 FPS through RT64 frame interpolation. The original game
logic and audio timing are retained. Set `rendering.target_fps` in `profile.toml`
to `0` for the original display rate, or `60` for interpolation, then rebuild.
Debug renderer reports include `target_fps`, `display_frames` and
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
