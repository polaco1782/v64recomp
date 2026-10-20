# Native debugging

For a new game, start with [adding a ROM and debugging a new port](adding-a-rom.md).
It covers mapping discovery, missing host/guest symbols, GDB, memory corruption
and the shared diagnostic scripts under `tools/`.

Configure `cmake --preset <game>-debug` and build that preset. The binary is
`build/<game>-debug/bin/<game>`; use `--help` for options. Debug app sources live
under `games/<game>/debug/`, and generated sources under the build's `generated/`.
The original positional diagnostic interfaces are also retained for tools.

RT64 and its bundled dependencies share a build under `build/shared/rt64/` across
all games and both modes. The default `RelWithDebInfo` configuration uses
`-O2 -g -DNDEBUG` with Clang/GCC. This retains renderer
symbols while disabling RT64 assertions, internal file logging and Vulkan
validation layers. Game code, the shared runtime and native adapters retain
their Debug settings; F1 inspection and F6 wireframe remain available.
Set `RECOMP_RT64_BUILD_TYPE=Debug` explicitly when debugging RT64 itself; this
selects a separate shared build with its assertions and diagnostics enabled.

Debug builds automatically print timestamped diagnostics to stderr: game and ROM
identity, boot/code loads, native OS hook entry arguments and caller addresses,
thread creation/start/stop/exit and priority changes, scheduling and queue waits,
event bindings, controller setup, ROM DMA, RSP tasks, and EEPROM/SRAM access.
Save logs include paths, offsets, sizes, results, and persistence failures.
Controller Pak initialization reports the absent accessory.
Once per second while the scheduler runs, status lines show thread states, VI
retraces/framebuffer, input reads, and EEPROM counters.

Frequent events print their first eight occurrences and then powers of two per
call site and host thread; OS entries include the cumulative call count. Set
`RECOMP_DEBUG_FULL=1` to print every sampled event. Save operations and thread
lifecycle events always print. These diagnostics are compiled out of Release.
For example: `build/quake-us-debug/bin/quake-us --frames 300 2>/tmp/quake-debug.log`.

Set `RECOMP_DEBUG_INPUT=1` to log changes in the controller state supplied by the
frontend or replay: port, connection, N64 button mask and stick axes. Correlate
these timestamps with screenshots and SI reads before attributing a missing
reaction to game logic. This verifies frontend delivery; guest consumption still
depends on the game's controller polling. Analog controllers can produce many
changes, so enable it only for a focused input investigation.

Set `RECOMP_DEBUG_QUEUE=0x800bea10` to trace every successful send, receive and
blocking wait for one guest message queue. The trace includes the message,
remaining count, caller thread and guest PC; cached/uncached aliases match.

Generated Debug code updates `ctx->pc` before every translated MIPS instruction,
including branches, jumps, nops, and executed delay slots. Annulled branch-likely
slots do not update it. Native calls restore the call-site PC after the delay
slot so blocking hooks and thread snapshots identify the caller. Addresses use
the compiled guest layout, including for relocated code.
`RECOMP_TRACK_PC` compiles to a no-op in Release, with no argument evaluation or
PC stores; the retained context field holds the scheduler's initial thread entry.
Release failure boundaries still report their known address directly.

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

To display the original MIPS for a generated function, run:

```sh
.venv/bin/python tools/disassemble_function.py recomp_s3_7f068008
```

For an unidentified SDK routine or a hardware-register failure, generate a
diagnostic SDK symbol/handler report against the matching build metadata:

```sh
.venv/bin/python tools/inspect_sdk_symbols.py --game hexen-us \
  --metadata build/hexen-us-debug/generated/hexen-us/symbols.json \
  --output build/hexen-us-debug/sdk-symbols
```

The directory must be new. Read `report.txt`, `report.json`, and `symbols.map`;
use `--scope sections` when investigating helpers pruned from discovery by a
higher-level native replacement. Unowned CRC matches remain candidates. Missing
handlers require behavior review, and calls inside replaced native bodies do
not establish an active runtime gap. See [SDK report interpretation](native-hooks.md#identifying-rom-routines-and-reviewing-handler-gaps).

The tool searches generated `symbols.json` files under `build/`, identifies the
game and verifies the ROM bytes before disassembly. It uses the game's preparation
hook for compressed code and prints only instructions owned by that function.
Use `--game goldeneye-us` to select a game, or `--metadata PATH` to select an exact
build's `symbols.json`. `--rom PATH` accepts an already prepared analysis image.

Every game uses the same core failure calls: `recomp_fail(message, address)` and
`recomp_game_require(condition, message, address)`. The shared ROM loader installs
the reporter in both modes, including positional diagnostic launches. Reports
and RAM dumps go under `local/crashes/<game>/` by default; `--crash-dir` selects
another directory through the common command-line interface.

The core writes ROM identity, failure details, loaded-section state, guest thread
registers (GPRs, FPRs, HI/LO and FPU control), task words, native stack frames and
a word-swapped RAM dump. Debug dispatch also retains the last 128 guest-call
addresses without needing a game-specific trace hook or `--trace-calls`.
Game code does not provide its own failure callback or crash-report serializer.
