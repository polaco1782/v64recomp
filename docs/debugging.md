# Native debugging

## Shared performance timing

Set `RECOMP_TIMING` to a new CSV filename before launching any game in Debug or
Release. Its parent directory must exist; existing reports are never replaced.
Recording is off by default. Rebuild after adding the timing implementation.
For the current Quake II slowdown investigation:

```powershell
$env:RECOMP_TIMING = "build/quake2-debug-$(Get-Date -Format yyyyMMdd-HHmmss).csv"
.\build\msvc\quake2-us\debug\bin\Debug\quake2-us.exe
Remove-Item Env:RECOMP_TIMING
```

Spend several seconds in a smooth area and then the slow area before closing
normally. The CSV is flushed about once per second and receives a final partial
window on shutdown. Repeat in Release with a different filename for comparison.
No ROM data, RAM contents or input events are recorded.

Each row contains `elapsed_s`, `window_ms`, `logical_cpus`, `cpu_cores`, and a
`*_count` / `*_ms` pair for each category below. Values are window deltas, not
lifetime totals. Divide a count by `window_ms / 1000` to obtain its rate.
`cpu_cores` is total process CPU time divided by wall time, including backend
workers: 1.0 means one logical CPU's worth of work, not 1% machine usage. On a
16-thread system, 1.0 is roughly 6.25% total CPU. A value of -1 means unavailable.

| Category | What it measures |
| --- | --- |
| `dispatch` | Guest scheduler resumptions; count only. |
| `checkpoint` | Due checkpoint actions, not skipped loop checks; count only. |
| `context_sync` | Wall time serializing guest thread contexts and queues. |
| `guest_wait` | Owner thread waiting for the guest to yield or request a callback, including OS handoff latency. This is **not** guest CPU time. |
| `host_callback` | Native services invoked through the guest-to-owner callback path. |
| `graphics` | Display-list processing and submission, including internal RT64/GPU waits and texture collection. This is **not** GPU execution time. |
| `graphics_cpu` | Calling-thread CPU time during graphics submission; excludes blocked time, render workers and GPU execution. Windows CPU accounting is coarse, so compare multi-second totals. A zero count means no valid CPU sample. |
| `present` | Presentation preparation in the renderer. |
| `audio` | Native DSP synthesis. |
| `sleep` | Sleeps requested by the shared frame-pacing helper. |
| `display` | Completed swap-chain draws, including interpolation; count only. |

Categories overlap: graphics/audio can be inside host callbacks, and guest-side
context synchronization can be inside `guest_wait`. Do not sum them as exclusive
frame costs. Long spans are charged when they finish, so a window can exceed
1000 ms in a category. Display counts are sampled on the owner during presentation;
the last asynchronous draws at shutdown may not be included. Games with custom
pacing outside the shared helper do not contribute to `sleep`.

High handoff counts and context time suggest scheduler overhead; high graphics
or presentation time suggests renderer processing/backpressure; high process
CPU with long guest waits suggests CPU work but does not identify a particular
guest function. Low CPU usage alone does not distinguish a game wait from a GPU
or synchronization wait. Use this report to choose a targeted profiler or
experiment, not as proof of a bottleneck. Enabling timing adds clock reads at
service boundaries and periodic CSV writes; it does not time every instruction
or alter checkpoint intervals, game speed, simulation, or graphics settings.

All MSVC game builds default to `RECOMP_MSVC_DEBUG_ITERATORS=OFF`. This sets
`_ITERATOR_DEBUG_LEVEL=0` throughout local C++ code and the shared RT64 dependency
build, avoiding STL iterator checking and tracking overhead in Debug rendering.
Debug retains its CRT, symbols, `RECOMP_DEBUG` and guest CPU checks. Explicitly
configure `-DRECOMP_MSVC_DEBUG_ITERATORS=ON` and rebuild when STL diagnostics are
needed. Existing explicit cache overrides remain in effect.
The setting participates in RT64's shared cache identity, so incompatible STL
layouts cannot reuse the same renderer library. Changing it rebuilds the C++
adapter and selects/builds the matching shared renderer. Do not set this macro
on an individual translation unit: C++ objects cross the adapter/library boundary.

## Debugger and diagnostics

For a new game, start with [adding a ROM and debugging a new port](adding-a-rom.md).
It covers mapping discovery, missing host/guest symbols, GDB, memory corruption
and the shared diagnostic scripts under `tools/`.

Configure and build with `cmake --workflow --preset <game>-debug`. The binary is
`build/<game>-debug/bin/<game>`; use `--help` for options. Debug app sources live
under `games/<game>/debug/`, and generated sources under the build's `generated/`.
The original positional diagnostic interfaces are also retained for tools.

RT64 and its bundled dependencies share a build under `build/shared/rt64/` across
all games and both modes, with separate caches where the Windows CRT differs.
The default `RelWithDebInfo` configuration uses release optimization and `NDEBUG`
(`/O2 /Zi /DNDEBUG` on MSVC, `-O2 -g -DNDEBUG` on Clang/GCC). This retains renderer
symbols while disabling RT64 assertions, internal file logging and Vulkan
validation layers. Game code, the shared runtime and native adapters retain
their Debug settings and HUD. RT64 developer tools are disabled by default.
Set `RECOMP_RT64_BUILD_TYPE=Debug` explicitly when debugging RT64 itself; this
selects a separate shared build with its assertions and diagnostics enabled,
and enables RT64 developer tools in the game's Debug adapter. On Windows, RT64
uses the same CRT as its callers because C++ objects cross that boundary.

Debug builds automatically print timestamped diagnostics to stderr: game and ROM
identity, boot/code loads, native OS hook entry arguments and caller addresses,
thread creation/start/stop/exit and priority changes, scheduling and queue waits,
event bindings, controller setup, ROM DMA, RSP tasks, and EEPROM/SRAM access.
Save logs include paths, offsets, sizes, results, and persistence failures.
Controller Pak initialization reports the absent accessory.
Once per second while the scheduler runs, status lines show thread states, VI
retraces/framebuffer, input reads, and EEPROM counters.

Rendered Debug builds also show a compact, 12-pixel green overlay at the top
right, without capturing input. It displays presentation FPS, translated MIPS
operations per second and total operations, interrupt events processed by the
scheduler, VI retraces, live guest threads (including runnable and waiting
counts), and submitted RSP tasks. It also shows triangles and recorded draw
calls in the latest completed RT64 workload, graphics display-list tasks/second
(`DL/s`), audio-list tasks/second (`AL/s`), pending RSP tasks, average graphics
job wall time per DL and synthesis time per AL, audio queue depth
in milliseconds, and playback underruns. Rates and timings use half-second
windows. Graphics time includes RT64 backpressure and any GPU waits inside the
job; it is not a GPU timer or exclusive CPU time. Synthesis time covers DSP
execution. HLE runs on the owner while the guest is parked; neither timer
includes guest execution or the scheduler handoff into the owner callback.
`frame` means a game workload completed by DP full sync, including preceding
split display lists, rather than an interpolated display frame. Triangle counts
exclude rectangle expansion and geometry rejected before RT64 records it; they
are not a count of triangles visible on screen. Draw counts describe RT64's
recorded game calls, rather than GPU API draws. DL/AL counts describe RSP tasks,
rather than nested display-list calls or individual audio commands.
Audio sampling uses completed synthesis counters without waiting for pending
work. The queue reads `off` when playback is muted/disabled, `starting` before a
device/rate is available, and `n/a` when no audio backend is attached.
Totals accumulate from startup. Operations
include executed nops and delay slots, exclude annulled slots, and exclude work
inside native hooks. IRQ events count drained libultra events, even when unbound;
they are separate from VI retraces, which may coalesce before event delivery.
The overlay is independent of the F1 inspector and absent from Release builds.

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
`RECOMP_TRACE` compiles to a no-op in Release, with no argument evaluation or
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
| `--cheat-menu` | SM64 only; session-only original level select. Use F12 for the shared GameShark menu. |
| `--frames N` | Presentation limit except SM64's scripted modes, which count playable frames. 0 runs until close. |

The replay JSON files under each game's `debug/` are ROM-bound reference inputs.
Convert one to the native harness's text input with:

```sh
.venv/bin/python tools/controller_replay.py games/quake-us/debug/gameplay-input.json /tmp/quake-input.txt --game quake-us
./build/quake-us-debug/bin/quake-us --input /tmp/quake-input.txt --frames 900
```

With `RECOMP_RT64_BUILD_TYPE=Debug`, F1 toggles RT64's inspector and F6 toggles
wireframe. Texture dumps are enabled only
in debug via `RECOMP_TEXTURE_DUMP`. `RECOMP_DUKE_PERFORMANCE=1` and
`RECOMP_NIGHTMARE_PERFORMANCE=1` retain those games' timing logs.

SM64's inspection scripts inspect task traces, ROM assets, display lists and
interior captures; OOT's callback analyzer proposes roots for checked inventories.
The shared crash inspector reads both release and debug reports. Use the
matching executable and generated sources to resolve addresses accurately.

To display the original MIPS and available recompiled x86-64 instructions for a
generated function in one run, use:

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
Both architectures are displayed by default. Select `--arch mips` for only MIPS,
`--arch x86` for only x86-64, or `--arch both` for both. The x86-only mode does not
load or prepare the ROM; the MIPS-only mode does not invoke `objdump`.
When equivalent metadata copies exist, lookup prefers a build with an executable,
then the newest metadata. The tool uses GNU `objdump` to append Intel-syntax host
disassembly from that build's `bin/<game>`. Use `--binary PATH` to select an
executable explicitly, paired with its matching `--metadata PATH`. The output
identifies both paths and notes if the binary predates the metadata. Without a
binary, `objdump`, or a retained function symbol, MIPS output remains available
and the tool explains why host disassembly is unavailable.

Every game uses the same core failure calls: `recomp_fail(message, address)` and
`recomp_game_require(condition, message, address)`. The shared ROM loader installs
the reporter in both modes, including positional diagnostic launches. Reports
and RAM dumps go under `local/crashes/<game>/` by default; `--crash-dir` selects
another directory through the common command-line interface.

Recoverable HLE operations use `recomp_warn(message, address)`. It captures the
same native stack, guest state and RAM snapshot once per message/address site
(up to 32 sites per thread), writing `warning-*.json` and a matching RAM dump.
The report marks
`failure.severity` as `warning`; the process keeps running. Audio and JPEG HLE
warnings skip the remainder of their current task and publish its completion.
An unrecognized graphics microcode skips its display list and publishes SP/DP
completion. Unsupported game-specific display-list commands skip that command.
Visuals or audio from a skipped operation may be missing until the next task.
Malformed task receipts and memory bounds remain fatal.

The core writes ROM identity, failure details, loaded-section state, guest thread
registers (GPRs, FPRs, HI/LO and FPU control), task words, native stack frames and
a word-swapped RAM dump. Debug dispatch also retains the last 128 guest-call
addresses without needing a game-specific trace hook or `--trace-calls`.
Game code does not provide its own failure callback or crash-report serializer.
