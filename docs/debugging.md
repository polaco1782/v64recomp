# Native debugging

Configure `cmake --preset <game>-debug` and build that preset. The binary is
`build/<game>-debug/bin/<game>`; use `--help` for options. Debug app sources live
under `games/<game>/debug/`, and generated sources under the build's `generated/`.
The original positional diagnostic interfaces are also retained for tools.

Debug always keeps the game's existing startup checks and final report/capture.
Artifacts live in `build/<game>-debug/debug-output/<game>/`. Call logging is
opt-in with `--trace-calls` (or `RECOMP_TRACE_CALLS=1`); it can be very verbose.

| Option | Availability |
| --- | --- |
| `--trace-calls` | Guest-call tracing for games other than SM64, plus recent-call crash history. SM64 traces task/game state. |
| `--input FILE` | All except SM64; whitespace rows: frame, N64 buttons, stick X, stick Y. GoldenEye frames start at 1; others at 0. |
| `--capture-every N` | Shadow Man, Quake, Duke, Nightmare, OOT; interval in presentations. GoldenEye bounded runs capture every 60 presentations. |
| `--wav FILE` | All except SM64, whose debug capture writes `audio.wav` automatically. |
| `--audio-trace FILE` | OOT; host audio queue timing as CSV. |
| `--mode live/idle/move/jump/interior` | SM64; scripted modes preserve its gameplay diagnostic harness. Bounded checks use 60..600 playable frames. |
| `--cheat-menu` | SM64 and GoldenEye; session-only original level select/unlocks. |
| `--frames N` | Presentation limit except SM64's scripted modes, which count playable frames. 0 runs until close. |

The replay JSON files under each game's `debug/` are ROM-bound reference inputs.
Convert one to the native harness's text input with:

```sh
.venv/bin/python tools/controller_replay.py games/quake-us/debug/gameplay-input.json /tmp/quake-input.txt --game quake-us
./build/quake-us-debug/bin/quake-us --input /tmp/quake-input.txt --frames 900
```

F1 toggles RT64's inspector; F6 toggles wireframe. Texture dumps are enabled only
in debug via `RECOMP_TEXTURE_DUMP`. `RECOMP_DUKE_PERFORMANCE=1` and
`RECOMP_NIGHTMARE_PERFORMANCE=1` retain those games' timing logs.

SM64's inspection scripts inspect task traces, ROM assets, display lists and
interior captures; OOT's callback analyzer proposes roots for checked inventories.
The shared crash inspector reads both release and debug reports. Use the
matching executable and generated sources to resolve addresses accurately.
