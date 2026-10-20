# Ocarina of Time US V1.2

Original title, file select, name entry and new-game opening with verified decompressed code and 469 relocatable overlays. Zelda NEAD audio and native JPEG SP decoding are supported. Inventory and full-game validation remain incomplete.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/oot-us-v12/profile.toml --rom /path/to/rom`.

```sh
cmake --workflow --preset oot-us-v12-release
./build/oot-us-v12-release/bin/oot-us-v12

cmake --workflow --preset oot-us-v12-debug
./build/oot-us-v12-debug/bin/oot-us-v12 --help
```

`release/` contains the playable app and Release-only sources. `debug/` contains
the original trace/capture harness and inspection tools. Shared headers, adapters,
`generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

Both builds target **60 FPS** using RT64 frame interpolation. Gameplay and audio
retain the original timing; the extra display frames interpolate movement between
game frames. This does not increase the game's logic or input update rate.

Set the display target at configuration time (RT64 caps it to the detected display
refresh rate). Use `0` to restore the original frame rate:

```sh
cmake --preset oot-us-v12-release -DRECOMP_OOT_FPS=0
cmake --build --preset oot-us-v12-release -j 4
```

Set `-DRECOMP_OOT_FPS=60` to restore 60 FPS, or choose another target from 10 to
1000. The setting is cached separately in each build directory. Debug reports
include `target_fps`, `display_frames` (swap-chain draws, including interpolated
frames), and `display_elapsed_ns` (time between the first and last draw).
The existing `presentations` counter counts host VI updates rather than displayed
frames.

JPEG tasks decode the game's coefficient buffers into RGBA5551 tiles before
posting SP completion. The native decoder uses the pinned Mupen64Plus RSP-HLE
implementation and verifies the boot, code and data images against this ROM's
SHA-256 profile. Link's house background was checked through the original opening;
other JPEG rooms and full-game progression still need validation. Debug reports
count completed tasks as `jpeg_tasks`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
