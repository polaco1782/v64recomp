# Duke Nukem 64 US

Original menus and first level with RT64 graphics, ABI1 audio and live input. Full-game progression remains unverified.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/duke-us/profile.toml --rom /path/to/rom`.

```sh
cmake --preset duke-us-release
cmake --build --preset duke-us-release -j 4
./build/duke-us-release/bin/duke-us

cmake --preset duke-us-debug
cmake --build --preset duke-us-debug -j 4
./build/duke-us-debug/bin/duke-us --help
```

`release/` contains the playable app and required game adapters. `debug/` contains
the original trace/capture harness and inspection tools. Profiles and inventories
in this directory are shared by both builds. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
