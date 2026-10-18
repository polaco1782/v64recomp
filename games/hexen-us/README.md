# Hexen US

The Hexen US import is configured and generates its CPU inventory in both Debug and Release builds. A clean compile is in place, but boot/menu/gameplay validation remains unverified.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/hexen-us/profile.toml --rom /path/to/rom`.

```sh
cmake --preset hexen-us-release
cmake --build --preset hexen-us-release -j 4
./build/hexen-us-release/bin/hexen-us

cmake --preset hexen-us-debug
cmake --build --preset hexen-us-debug -j 4
./build/hexen-us-debug/bin/hexen-us --help
```

`release/` contains the playable app and Release-only sources. `debug/` contains the original trace/capture harness and inspection tools. Shared headers, adapters, `generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md), and [support status](../../docs/status.md).
