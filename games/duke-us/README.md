# Duke Nukem 64 US

Original menus and first level with RT64 graphics, ABI1 audio and live input. Full-game progression remains unverified.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/duke-us/profile.toml --rom /path/to/rom`.

```sh
cmake --workflow --preset duke-us-release
./build/duke-us-release/bin/duke-us

cmake --workflow --preset duke-us-debug
./build/duke-us-debug/bin/duke-us --help
```

`release/` contains the playable app and Release-only sources. `debug/` contains
the original trace/capture harness and inspection tools. Shared headers, adapters,
`generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
