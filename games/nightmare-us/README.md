# Nightmare Creatures US

The native conversion runs the original title flow, menus, character selection
and the first level, Chelsea, with RT64 graphics, ABI1 audio and controller input.
The game, its profiles and launchers live in this directory.

From the project root:

```sh
.venv/bin/python games/nightmare-us/run_nightmare.py \
  --rom '/home/cmartin/Downloads/N64/ROMS/Nightmare Creatures (U) [!].z64' \
  --render --frames 0

# Subsequent launches use the verified copy in local/.
.venv/bin/python games/nightmare-us/run_nightmare.py --render --frames 0
```

The executable is `build/renderer/nightmare_native_rt64`. `--build-only` builds
without launching; `--jobs N` controls build parallelism. Rendering defaults to
360 presentations; `--frames 0` keeps the window open until Escape or close.
Keyboard controls: arrows for the stick, A/X for N64 A/B, S for Start, Z for Z,
C/V for L/R, I/K/J/L for C directions, T/G/F/H for the D-pad. F1 opens the RT64
inspector. No controller accessory is exposed.

`--mute` disables playback while retaining synthesis. `--wav PATH` records PCM,
`--capture-every N` saves PPM frames, and `--input-script PATH` plays a ROM-bound
controller replay. `--performance` records per-second video, VI, audio production
and playback-queue measurements in `boot-report.json.timing.jsonl`. The checked
replays navigate the menus and enter Chelsea:

```sh
.venv/bin/python games/nightmare-us/run_nightmare.py --render --frames 2160 \
  --input-script games/nightmare-us/gameplay-input.json --capture-every 120
```

The replay supplies normal input without modifying game state. It selects
Ignatius, moves forward, turns and presses B. Frame numbers count host
presentations, so slower machines may need different input timing.

Without `--render`, the default checker executes the original entry, verifies
BSS clearing and its guard, and stops before the first application thread.
`--startup` continues through the first RSP submission without executing a
backend or fabricating completion. `--report-only` reports translation coverage.
Failures save recent guest calls, thread registers, native stack frames, the
last task and word-swapped RDRAM beside `boot-report.json`:

```sh
.venv/bin/python tools/inspect_crash.py \
  build/nightmare-us/rom/startup/boot-report.json.crash.json
```

The ROM is the big-endian US revision, 16 MiB, SHA-1
`bea6c21b3ef5950bdf8cdb255955da6c4746eea8`. The resident load maps ROM offset
`0x1000` to `0x80000400`, with BSS beginning at `0x800A4AF0`. The partial checked
entry inventory pins function prefixes; switch tables pin their complete bytes.
It is not a complete classification of CPU code and data.

There are 1,210 translated functions, 47 verified native service bindings and
10 explicit unused privileged SDK boundaries. Plain SDK variants were added
to the shared catalog. The sole game override retires the synthetic IPL thread
after the original `osStartThread` transfers control to the application.

The original priority-15 controller thread reads continuously. This profile
uses a 1 ms SI transfer delay so it blocks between reads, allowing priority-10
game logic to execute. Other profiles retain their existing immediate completion
policy. The graphics microcode is F3DEX fifo 2.06: its `0x1390`-byte linked code
image includes overlays and exceeds the physical IMEM bank. Native task
validation accepts the same bounded 8 KiB graphics source range as RT64.
Audio uses the verified standard ABI1 microcode and a 160 ms playback prebuffer.

First-level startup, rendered movement and input have been observed. Full-game
coverage, prolonged play, saves, audio continuity and regressions in the other
games remain to be checked.

The game loop services VI, AI, timers and RSP completions between its 60 Hz
presentations. In a desktop comparison with the same controller replay, the old
presentation-only polling recorded 24 audio underruns in 20 seconds; deadline
service completed 3,600 presentations in 60.5 seconds with zero underruns or
queue resets. Steady audio production matched the 44,095 Hz device rate and
steady graphics submission reached about 60 tasks per second. A separate run
with periodic screenshot readbacks had one underrun in 40 seconds; capture
work can still interrupt timely audio service.
