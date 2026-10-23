# Shadow Man US

Original startup, graphics, audio, Controller Pak screen and title screen are
working in the bounded desktop replay. Gameplay remains unverified; this is not
a completed game port.

Both configurations use native frame timing and VI-driven presentation. RT64's
early-presentation mode submits graphics for this game without producing display
draws, leaving the window black. The checked profile includes scene and object
callback tables; the entry inventory covers callbacks installed by guest code.

Validation: Debug and Release builds pass. A Debug desktop run completed 1,800
presentations (2,644 graphics tasks and 6,353 audio tasks) without a missing-function
failure, with visible loading and animated title screens. No interactive gameplay
acceptance is claimed by this run.

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
