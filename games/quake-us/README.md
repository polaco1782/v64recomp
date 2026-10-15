# Quake 64 US

Original menu and playable first level, with movement, turning and shotgun firing previously observed. Full-game stability and saves remain unverified.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/quake-us/profile.toml --rom /path/to/rom`.

```sh
cmake --preset quake-us-release
cmake --build --preset quake-us-release -j 4
./build/quake-us-release/bin/quake-us

cmake --preset quake-us-debug
cmake --build --preset quake-us-debug -j 4
./build/quake-us-debug/bin/quake-us --help
```

`release/` contains the playable app and required game adapters. `debug/` contains
the original trace/capture harness and inspection tools. Profiles and inventories
in this directory are shared by both builds. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
