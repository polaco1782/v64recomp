# GoldenEye 007 US

Original startup, menus and playable Dam. CPU inventory is partial and remaining unsupported functions fail explicitly. Full-game progression remains unverified.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/goldeneye-us/profile.toml --rom /path/to/rom`.

```sh
cmake --preset goldeneye-us-release
cmake --build --preset goldeneye-us-release -j 4
./build/goldeneye-us-release/bin/goldeneye-us

cmake --preset goldeneye-us-debug
cmake --build --preset goldeneye-us-debug -j 4
./build/goldeneye-us-debug/bin/goldeneye-us --help
```

`release/` contains the playable app and required game adapters. `debug/` contains
the original trace/capture harness and inspection tools. Profiles and inventories
in this directory are shared by both builds. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
