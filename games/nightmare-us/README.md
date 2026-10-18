# Nightmare Creatures US

Original title, menus, character selection and playable Chelsea with graphics, ABI1 audio and live input. Full-game progression remains unverified.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/nightmare-us/profile.toml --rom /path/to/rom`.

```sh
cmake --preset nightmare-us-release
cmake --build --preset nightmare-us-release -j 4
./build/nightmare-us-release/bin/nightmare-us

cmake --preset nightmare-us-debug
cmake --build --preset nightmare-us-debug -j 4
./build/nightmare-us-debug/bin/nightmare-us --help
```

`release/` contains the playable app and Release-only sources. `debug/` contains
the original trace/capture harness and inspection tools. Shared headers, adapters,
`generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
