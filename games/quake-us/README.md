# Quake 64 US native recompilation

Quake has its own ROM profile, application, checked callback entries and audio
recognition under `games/quake-us/`. Generated code and captures stay under
`build/quake-us/`; the imported ROM stays in `local/quake.us.z64`.
Run commands from the checkout root.

## Import, build and run

```sh
.venv/bin/python games/quake-us/run_quake.py \
  --rom '/home/cmartin/Downloads/N64/ROMS/Quake 64 (U) [!].z64'
.venv/bin/python games/quake-us/run_quake.py --startup
.venv/bin/python games/quake-us/run_quake.py --render --build-only
.venv/bin/python games/quake-us/run_quake.py --render --frames 0
```

`--render` uses RT64 graphics, SDL audio and live controller input. Its default
limit is 360 presentations; `--frames 0` runs until Escape or window close.
Keyboard controls are arrows for the stick, A/X for N64 A/B, S for Start,
Z for Z, C/V for L/R, I/K/J/L for C directions, and T/G/F/H for the D-pad.
Use `--jobs N` to limit build parallelism.
The native controller has no accessory. On the Rumble Pak prompt, move the
stick down to **Do not use Pak** and press A to continue.

`--mute` keeps synthesis active without opening an audio device; `--wav PATH`
captures PCM. `--capture-every N` saves periodic PPM images beside the report.
`--input-script PATH` accepts a ROM-bound controller replay: JSON with
`format_version: 1`, `rom_sha1`, and `events` containing increasing `frame`
numbers, a `buttons` N64 mask and optional signed `stick_x`/`stick_y` values.

## Validation and current scope

The default bounded checker poisons BSS, executes the original ROM entry code,
checks the clear and its end guard, then validates the first thread's entry,
ID, priority and stack before it executes. `--startup` runs application
initialization and stops all guest threads at the first real RSP submission,
before backend execution. It does not fabricate SP/DP completion.

```sh
ASAN_OPTIONS=detect_leaks=0 .venv/bin/python games/quake-us/run_quake.py \
  --startup --sanitize-address --output build/quake-us/rom-asan
.venv/bin/python games/quake-us/run_quake.py --report-only
.venv/bin/python games/quake-us/run_quake.py --render --frames 2400 \
  --input-script games/quake-us/gameplay-input.json --capture-every 300
```

The bootstrap and first-task checks pass with ASan/UBSan. The partial conversion
contains 1,404 translated functions and 55 shared catalog bindings. A desktop
check passes through the menus, starts the first level and verifies forward
movement, turning and shotgun firing. Player displacement was 695.99 units;
the HUD's shell count changed from 25 to 23, with health remaining at 100.
The 2,400-presentation run completed 2,147 graphics tasks, including 1,081
lists without DP full sync. Their real SP receipts retire without fabricating
DP events; RT64 keeps their draws for the subsequent full-sync list.

The audio backend uses the pinned ABI1 program and a 160 ms playback prebuffer.
The gameplay run produced nonzero PCM and recorded one audio underrun, with
zero queue resets. Audio continuity, prolonged play, remaining code inventory
classification and Controller Pak save persistence remain incomplete.
The shared changes also pass a bounded SM64 Castle Grounds graphics/PCM check.

Reports are `build/quake-us/rom/boot/verification.json` and
`build/quake-us/rom/startup/verification.json`. Failures save guest registers,
recent calls, task words, native frames and word-swapped RAM:

```sh
.venv/bin/python tools/inspect_crash.py \
  build/quake-us/rom/startup/boot-report.json.crash.json
```

Gameplay reports record entry into the original game loop and initial/final
player coordinates. The entity array indexer, player-spawn stores and loop call
in the pinned ROM supply these diagnostic addresses. Replays use normal
controller input and do not write gameplay state.

## ROM and native services

The pinned ROM is big-endian, 12 MiB, US region E, revision 0, header CRC
`9F5BF79C D2FE08A0`, SHA-1 `25a7fe0342490d3c23328040bb34f8748995537a`.
The profile uses 4 MiB RAM and rejects different ROM bytes.

The initialized image maps ROM `0x1000..0x78080` to guest
`0x80000400..0x80077480`. Original startup clears
`0x80077480..0x800debc0`, enters `0x80048f20`, and creates thread 1 at
`0x80048f7c` with priority 10 and stack top `0x800cca00`.
CPU code is directly stored in the ROM. Original translated code performs
subsequent asset loading through native PI services.

Graphics tasks use `0x30` in their flags. These are the application's
`OS_TASK_USR0`/`OS_TASK_USR1` bits in the
[SDK task header](https://git.noest.dev/gijs/ogrebattle64/src/commit/f38c7a0f3e0d386efbf6d6bb3b344baebd98c195/include/ultra/PR/sptask.h).
The shared task transport preserves these metadata bits.

All OS calls use the [shared catalog](../../docs/native-hooks.md); the game
profile has no OS hook overrides. Newly verified SDK variants were added to
`tools/native_hooks.toml`, and missing ordinary services were implemented in
the shared runtime. This same rule applies to future game additions.
Callback roots, local switches, the 76-callback entity factory and the
16-entry controller action table are fingerprinted against the ROM. The
build uses no matching ELF, reference game source or CPU interpreter.
