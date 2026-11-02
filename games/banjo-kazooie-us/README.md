# Banjo-Kazooie US

Partial port pinned to the ROM identity and mappings in `profile.toml`.
Build success does not establish playable support; gameplay, audio and save
acceptance remain to be verified.

```sh
.venv/bin/python tools/import_rom.py --profile games/banjo-kazooie-us/profile.toml --rom /path/to/rom
cmake --preset banjo-kazooie-us-debug
cmake --build --preset banjo-kazooie-us-debug
./build/banjo-kazooie-us-debug/bin/banjo-kazooie-us --help
```

Use the matching `banjo-kazooie-us-release` presets for Release. Both compile
`main.c`, which owns boot, runtime services and the frame loop. `generate.py`
emits `game_config.h` under the selected build directory. Audio profiles,
checked inventories and the maintained reference importer live beside the code.
See [the common layout](../../docs/adding-a-rom.md#7-add-the-native-application-and-cmake-integration)
and [Debug options](../../docs/debugging.md).
