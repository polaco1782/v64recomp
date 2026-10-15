# Ocarina of Time US V1.2

Original title, file select, name entry and new-game opening with verified decompressed code and 469 relocatable overlays. Zelda NEAD audio is supported. JPEG SP tasks retain the existing skip-and-complete behavior. Inventory and full-game validation remain incomplete.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/oot-us-v12/profile.toml --rom /path/to/rom`.

```sh
cmake --preset oot-us-v12-release
cmake --build --preset oot-us-v12-release -j 4
./build/oot-us-v12-release/bin/oot-us-v12

cmake --preset oot-us-v12-debug
cmake --build --preset oot-us-v12-debug -j 4
./build/oot-us-v12-debug/bin/oot-us-v12 --help
```

`release/` contains the playable app and required game adapters. `debug/` contains
the original trace/capture harness and inspection tools. Profiles and inventories
in this directory are shared by both builds. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
