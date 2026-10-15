# Super Mario 64 US

Working gameplay baseline, with Castle Grounds movement/jumping and the castle lobby transition previously validated. Broader courses, ending and prolonged runs still need validation.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/sm64-us/full.toml --rom /path/to/rom`.

```sh
cmake --preset sm64-us-release
cmake --build --preset sm64-us-release -j 4
./build/sm64-us-release/bin/sm64-us

cmake --preset sm64-us-debug
cmake --build --preset sm64-us-debug -j 4
./build/sm64-us-debug/bin/sm64-us --help
```

`release/` contains the playable app and required game adapters. `debug/` contains
the original trace/capture harness and inspection tools. Profiles and inventories
in this directory are shared by both builds. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
