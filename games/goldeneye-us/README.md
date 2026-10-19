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

`release/` contains the playable app and Release-only sources. `debug/` contains
the original trace/capture harness and inspection tools. Shared headers, adapters,
`generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

Guest pthreads use the profile's entry-based names: `n64-main`, `n64-idle`,
`n64-scheduler`, `n64-sound` and `n64-controller`. `n64-sound` runs translated
sound code; `n64-audio` is the separate native DSP worker. See
[CPU profiling](../../docs/profiling.md) for thread counters and function samples.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
