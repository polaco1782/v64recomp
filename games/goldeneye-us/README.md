# GoldenEye 007 US

Original startup, menus and playable Dam. CPU inventory is partial and remaining unsupported functions fail explicitly. Full-game progression remains unverified.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/goldeneye-us/profile.toml --rom /path/to/rom`.

```sh
cmake --workflow --preset goldeneye-us-release
./build/goldeneye-us-release/bin/goldeneye-us

cmake --workflow --preset goldeneye-us-debug
./build/goldeneye-us-debug/bin/goldeneye-us --help
```

`release/` contains the playable app and Release-only sources. `debug/` contains
the original trace/capture harness and inspection tools. Shared headers, adapters,
`generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

`profile.toml` contains the checked `discovery.jump_tables`. `generate.py` resolves
them from the fingerprinted, decompressed startup data before discovery checks
their transfer instructions and code targets.

Guest pthreads use the profile's entry-based names: `n64-main`, `n64-idle`,
`n64-scheduler`, `n64-sound` and `n64-controller`. `n64-sound` runs translated
sound code; `n64-audio` is the separate native DSP worker. See
[CPU profiling](../../docs/profiling.md) for thread counters and function samples.

After the first graphics task, both apps service VI/AI deadlines and worker
completions between presentations through `recomp_game_pace`. Generation checks
the empty idle loop and its `osCreateThread` argument setup before registering
the idle object with the scheduler. Synthetic-clock bootstrap keeps its bounded
drain.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
