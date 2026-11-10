# Native debugging

## Shared performance timing

Set `RECOMP_TIMING` to a new CSV filename before launching any game in Debug or
Release. Its parent directory must exist; existing reports are never replaced.
Recording is off by default. CSV creation/header errors warn and disable profiling
without preventing the game from starting. Later CSV write errors also disable
profiling; existing reports remain untouched. Remove `RECOMP_TIMING` to stop
requesting profiling in subsequent launches. Rebuild after changing the timing
implementation.
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


Categories overlap: graphics/audio can be inside host callbacks, and guest-side
context synchronization can be inside `guest_run`. Do not sum them as exclusive
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


## Debugger and diagnostics

For a new game, start with [adding a ROM and debugging a new port](adding-a-rom.md).
It covers mapping discovery, missing host/guest symbols, GDB, memory corruption
and the shared diagnostic scripts under `tools/`.

Configure and build with `cmake --workflow --preset <game>-debug`. The binary is
`build/<game>-debug/bin/<game>`; use `--help` for options. Both configurations
compile `games/<game>/main.c`; generated sources live under the build's
`generated/` directory. Both use the same named command-line options.

Linux Release builds move debug sections into a compressed `<game>.debug`
sidecar beside the executable and add a `.gnu_debuglink`. GDB and compatible
address resolvers can use that matching file for source locations. Keep the
sidecar with build artifacts; copying only the executable is sufficient to run
the game. The native symbol table, dynamic exports and unwind information remain
in the executable for crash reports, profiling and live repair. Debug retains
its embedded debug information; MSVC uses PDB files in both configurations.
Disable separation with `-DRECOMP_SEPARATE_DEBUG_INFO=OFF` when needed.
Both Linux modes explicitly link as PIE; the profiler and crash resolver account
for the module's runtime relocation base.


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
`RECOMP_TRACE` compiles to a no-op in Release, with no argument evaluation or
PC stores; the retained context field holds the scheduler's initial thread entry.
Release failure boundaries still report their known address directly.

Debug runs the same bootstrap and frame loop as Release. Routine captures are
opt-in; the old positional launch interface, per-game boot reports and SM64
scripted validation modes have been removed. The renderer-specific cheat menu is removed.

| Option | Behavior |
| --- | --- |
| `--trace-calls` | Trace guest calls in core dispatch for every title. Recent-call crash history is always retained in Debug. |
| `--input FILE` | Controller-zero replay after host input mapping. Decimal rows: frame, N64 buttons, stick X, stick Y. Frames start at 0 for every game; blank lines and `#` comment lines are allowed. |
| `--capture-every N` | Capture every N presentations to `build/<game>-debug/debug-output/<game>/frame-NNNNNN.ppm`; 0 disables capture. |
| `--wav FILE` | Record PCM through the shared audio backend; Media Player records its decoded MP3 PCM. |
| `--audio-trace FILE` | Record shared AI/playback queue timing as CSV. |
| `--frames N` | Presentation limit; 0 runs until close. Media Player counts decoded MP3 frames. |

Replay and screenshots apply to games using the N64 renderer/input helpers.
Media Player uses its own SDL interface, so `--input`, `--capture-every` and
`--audio-trace` do not apply to it. Bad Apple has no guest controller input.
The replay holds the last row until the next specified frame, including after EOF.
Use a final neutral row when buttons should be released. Malformed values or
out-of-order frames fail explicitly.

Write replay rows directly in a text file. This short example presses Start for
one presentation; choose timings that match the title and scene being reproduced:

```sh
cat > /tmp/quake-input.txt <<'EOF'
0 0 0 0
300 4096 0 0
301 0 0 0
EOF
./build/quake-us-debug/bin/quake-us --input /tmp/quake-input.txt --frames 900
```


The shared callback and function-candidate inspectors propose roots for review.
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
the reporter in both modes. Reports
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
Debug stack dumps include compiled C file names and line numbers in the terminal
and each JSON frame's `source` field. Linux uses `addr2line` (or
`llvm-addr2line`, selected at configure time); Windows uses the matching PDB.
Keep the executable's debug information available. Unresolved frames retain their
raw addresses. Generated game frames point into the build directory's C sources.
The inspector prefers the saved locations so rebuilding does not change them.

Game code does not provide its own failure callback or crash-report serializer.
