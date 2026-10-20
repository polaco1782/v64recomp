# Perfect Dark US V1.1

This port targets the big-endian US revision 1.1 ROM with SHA-1
`af8788ac4d1a57260eae9c53ffe851fcf2a3319b` and uses 8 MiB of guest RAM.
Bring-up is in progress; the checked startup inventory is partial and unknown
functions fail with a guest address and crash report.

Release and debug builds have been checked through the original startup,
agent creation, EEPROM saving, menus and first-person Carrington Institute
rendering. Live input works; the user also confirmed the mission transition.
Full-game progression and broad regressions remain untested.

From the project root:

```sh
.venv/bin/python tools/import_rom.py --profile games/perfectdark-us-v11/profile.toml --rom '/home/cmartin/Downloads/N64/ROMS/Perfect Dark (U) (V1.1) [!].z64'
cmake --preset perfectdark-us-v11-release
cmake --build --preset perfectdark-us-v11-release -j 4
./build/perfectdark-us-v11-release/bin/perfectdark-us-v11
```

For crash reports, captures and deterministic controller input:

```sh
cmake --preset perfectdark-us-v11-debug
cmake --build --preset perfectdark-us-v11-debug -j 4
./build/perfectdark-us-v11-debug/bin/perfectdark-us-v11 --frames 3700 --save /tmp/perfectdark-bringup.eeprom --input games/perfectdark-us-v11/debug/gameplay-input.txt --capture-every 120
```

The replay assumes a fresh EEPROM file. Reusing an existing agent changes the
menu sequence. Debug reports and captures are written under
`build/perfectdark-us-v11-debug/debug-output/perfectdark-us-v11/`.

ROM and EEPROM data stay in `local/`. Generated sources stay in the selected
build directory. The debug executable's `--help` lists report and save options.

CMake selects independent `release/app.c` and `debug/app.c` frontends. Replay,
captures and reports live in `debug/app.c`; both frontends link the shared
bootstrap, native services and frame loop in `perfectdark.c`.

`generate.py` expands the RareZip library, data and game pages only for
static translation. The executable retains the original cartridge image and
runs the translated original bootstrap, inflater and 8 MiB loader. Complete
decoded-image hashes are checked before their translated functions become
available. Layout references come from the matching `ntsc-final` version of
[the Perfect Dark decompilation](https://github.com/n64decomp/perfect_dark).

Common SDK calls are recognized by complete function-body hashes in the shared
`tools/symbols.json` catalog. `profile.toml` contains Perfect Dark's
boot mapping, inflater handoff, obfuscated cartridge read and idle checkpoint,
alongside checked original switch and AI handler tables. The model
assembly's shared epilogue and nonlocal helper returns have explicit, checked
translation metadata. Native audio uses the pinned NAudio/MP3 RSP-HLE decoder
and verifies the task's microcode images.

One longer debug run stopped with `ADPCM predictor outside codebook`. A repeat
of the same menu action completed normally; the intermittent audio failure
still needs investigation. Capture-heavy debug runs also recorded audio queue
underruns, so they are not an audio-pacing acceptance result.
