# Duke Nukem 64 US native recompilation

Duke has its own ROM profile, application, checked callback entries and audio
recognition under `games/duke-us/`. Generated code and captures stay under
`build/duke-us/`; the imported ROM stays in `local/duke.us.z64`.
Run commands from the checkout root.

```sh
.venv/bin/python games/duke-us/run_duke.py \
  --rom '/home/cmartin/Downloads/N64/ROMS/Duke Nukem 64 (U) [!].z64'
.venv/bin/python games/duke-us/run_duke.py --startup
.venv/bin/python games/duke-us/run_duke.py --render --build-only
.venv/bin/python games/duke-us/run_duke.py --render --frames 0
```

`--render` uses RT64 graphics, SDL audio and live controller input. Its default
limit is 360 presentations; `--frames 0` runs until Escape or window close.
Keyboard controls are arrows for the stick, A/X for N64 A/B, S for Start,
Z for Z, C/V for L/R, I/K/J/L for C directions, and T/G/F/H for the D-pad.
Use `--jobs N` to limit build parallelism.
The controller has no accessory; Controller Pak saving is unavailable.

`--mute` keeps synthesis active without opening an audio device; `--wav PATH`
captures PCM. `--capture-every N` saves periodic PPM images beside the report.
`--input-script PATH` accepts a ROM-bound controller replay: JSON with
`format_version: 1`, `rom_sha1`, and `events` containing increasing `frame`
numbers, a `buttons` N64 mask and optional signed `stick_x`/`stick_y` values.

The default bounded checker poisons BSS, executes the original ROM entry code,
checks the clear and its end guard, then validates the first thread's entry,
ID, priority and stack before it executes. `--startup` runs application
initialization and stops guest threads at the first real RSP submission,
before backend execution. It does not fabricate SP/DP completion.
`--report-only` reports translation coverage without building.

## CPU and frame pacing

Startup runs against exact VI/timer deadlines until Duke's original two-retrace
clock calibration returns. The clock then continues from the same guest time
using host elapsed time. This prevents host startup delays from inflating the
measured retrace period and slowing all subsequent gameplay. The translated
calibration computes 781,250 counter ticks per retrace (16.667 ms); the original
frame loop uses two retraces per new graphics frame.

The launcher recognizes the main thread's original frame-time polling loop
and returns to host pacing when that thread is waiting. The priority-zero idle
thread also returns to the host. While sleeping between presentations, the app
wakes for native VI, AI DMA and timer deadlines so audio keeps receiving timely
service. Long frames rebase overdue presentation deadlines. Game calculations,
graphics quality and audio synthesis remain the original translated/runtime paths.

Add `--performance` to a rendered run to include elapsed time, main-thread CPU
time, scheduler time, graphics submission, presentation and sleep time in the
report. Graphics submission time is included in scheduler time. It also writes
`REPORT.performance.jsonl` once per 60 presentations with cumulative graphics
tasks, VI retraces, calibrated retrace ticks and game state, so actual game
frames can be distinguished from repeated window presentations. For example:

```sh
.venv/bin/python games/duke-us/run_duke.py --render --frames 3600 --performance \
  --input-script games/duke-us/gameplay-input.json
```

Presentation runs at 60 Hz; the original game still decides when to produce a
new graphics task. Desktop gameplay after the calibration fix held about 30 new
graphics tasks per second, versus about 16 with the inflated startup measurement.
Evidence is in `build/duke-us/performance/video-fixed.json.performance.jsonl`.

## Validation and current scope

The partial conversion has 743 translated functions and 55 shared catalog
bindings, with no unresolved selected instructions or transfers. The bootstrap
and first-task checks pass with UBSan. The initial desktop run completed 2,700
presentations, 1,250 graphics tasks with matching SP/DP callbacks, and 3,901 audio
tasks. Captures verify the first level, forward movement, turning and pistol
firing: ammo changed from 48 to 34 while health remained at 100. The stereo PCM
capture contains nonzero samples; the run recorded zero audio underruns/resets.

```sh
.venv/bin/python games/duke-us/run_duke.py --render --frames 3600 \
  --input-script games/duke-us/gameplay-input.json --capture-every 300 \
  --wav build/duke-us/gameplay.wav
```

Controller Pak persistence, full-game progression, complete CPU inventory and
broader regression checks remain to be done. This first addition focuses on
getting the game running. The desktop evidence is recorded in
`build/duke-us/rom/startup/gameplay-verification.json` and
`build/duke-us/gameplay.png`.

## ROM and native services

The pinned ROM is big-endian, 8 MiB, US region E, revision 0, header CRC
`A273AB56 DA33DB9A`, SHA-1 `98d6778004becc672eba0a5e887f6e3f3d1b5c15`.
The profile uses 8 MiB RAM and rejects different ROM bytes. The original sprite
sorter can speculatively read above the base 4 MiB bank (observed at
`0x8051e65c` on the first rooftop); the upper bank supplies valid backing for
those reads without replacing the game routine.
The initialized image maps ROM `0x1000..0xc0880` to guest
`0x8004b400..0x8010ac80`. Original startup clears
`0x8010ac80..0x802ae7b0`, enters `0x8004b450`, and creates thread 1 at
`0x8004b4b4` with priority 10 and stack top `0x80112e38`.
Original translated code loads subsequent assets through native PI services.

All OS calls use the [shared catalog](../../docs/native-hooks.md); the game
profile has no OS hook overrides. SDK variants verified against their complete
ROM bodies were added to `tools/native_hooks.toml`. The audio manager's inline
AI length read uses the same native DMA state as `osAiGetLength`.
Audio uses the pinned ABI1 program and a 160 ms playback prebuffer.
Callback entries and local switch tables are fingerprinted against the ROM.
This is a partial conversion; there is no complete CPU code/data inventory.
The build uses no matching ELF, reference game source or CPU interpreter.

Reports are `build/duke-us/rom/boot/verification.json` and
`build/duke-us/rom/startup/verification.json`. Failures save guest registers,
recent calls, task words, native frames and word-swapped RAM:

```sh
.venv/bin/python tools/inspect_crash.py \
  build/duke-us/rom/startup/boot-report.json.crash.json
```
