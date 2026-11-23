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
python3 tools/build.py perfectdark-us-v11 --release
./build/perfectdark-us-v11-release/bin/perfectdark-us-v11
```

## Desktop controls and field of view

Set `RECOMP_PERFECTDARK_WASD=1` to use WASD as the N64 control stick and
the arrow keys as C-buttons. Enter/Q maps to A, E/R to B, and Tab to Start.
For the normal first-person camera, F4 captures the mouse and Escape
releases it. While captured, mouse movement rotates the walking camera and
left/right click map to Z/R. Select the original in-game control style 1.1:
W/S move forward/back, and A/D send its native C-left/C-right strafe input.
Other styles, vehicles, multiplayer and third-person cameras are not
covered. The original keyboard layout remains the default; release capture
or disable the opt-in to return to unmodified guest inputs.

Use keypad + / - to widen the horizontal FoV from the stock angle at 4:3
to 140 degrees. The original vertical angle is 60 degrees. The two checked
calls in the normal zoom updater adjust both the player viewport and VI
camera scale from the same native value, preserving zoom magnification
and leaving the native zoom state alone. The setting is scoped to the
normal first-person camera in single-player, and does not apply to the
teleport override, intro, or cutscene projection. Return to the minimum
to restore the original behavior. Debug input replays retain stock inputs
and field of view.

The adapter is in `controls.c`. Generation verifies the original decoded
setters, the two FoV call sites, the walking look call site, their delay
slots and the associated player state loads against US V1.1. [Perfect Dark's matching decompilation](https://github.com/n64decomp/perfect_dark)
was used to identify the native roles; the prepared cartridge determines
the actual addresses and checked instructions.

For crash reports, captures and deterministic controller input:

```sh
python3 tools/build.py perfectdark-us-v11 --debug
./build/perfectdark-us-v11-debug/bin/perfectdark-us-v11 --frames 3700 --save /tmp/perfectdark-bringup.eeprom --input games/perfectdark-us-v11/gameplay-input.txt --capture-every 120
```

The replay assumes a fresh EEPROM file. Reusing an existing agent changes the
menu sequence. Optional Debug captures are written under
`build/perfectdark-us-v11-debug/debug-output/perfectdark-us-v11/`.

ROM and EEPROM data stay in `local/`. Generated sources stay in the selected
build directory. The debug executable's `--help` lists capture and save options.

CMake compiles `main.c` for both configurations. It owns the bootstrap, native
services and frame loop. Debug replay, captures and audio recording use the
shared game helpers; there is no separate diagnostic frontend.

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
