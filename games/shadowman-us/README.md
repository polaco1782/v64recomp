# Shadow Man US

Original startup, graphics, audio and Controller Pak screen. Title/gameplay progression remains in progress; this is not a completed game port.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/shadowman-us/profile.toml --rom /path/to/rom`.

```sh
cmake --workflow --preset shadowman-us-release
./build/shadowman-us-release/bin/shadowman-us

cmake --workflow --preset shadowman-us-debug
./build/shadowman-us-debug/bin/shadowman-us --help
```

`release/` contains the playable app and Release-only sources. `debug/` contains
the original trace/capture harness and inspection tools. Shared headers, adapters,
`generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
