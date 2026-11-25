# Native debugging

## Shared performance timing

The shared desktop pacer honors the host presentation deadline between guest
fiber dispatches. Timer, VI, AI and RSP receipts are still polled on the owner
thread. When the process falls a full display slot behind, it rebases the host
presentation clock instead of drawing a burst to repay old deadlines. This
does not alter guest clock rates, priorities, interrupt ordering, or audio
sample rates. A native task already in progress can still finish after that
deadline, and an external workload can still exhaust CPU or GPU time.

For load-related glitches, compare a normal run and a run with the usual
background activity using separate timing CSVs. In Debug, also use
`--audio-trace FILE` to compare queued audio and observed underruns. Measure
under the same renderer, resolution and scene; a frame drop alone does not
identify an audio, CPU scheduler or graphics bottleneck.

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

| Category | What it measures |
| --- | --- |
| `dispatch` | Guest scheduler resumptions; count only. |
| `checkpoint` | Due checkpoint actions, not skipped loop checks; count only. |
| `context_sync` | Wall time serializing guest thread contexts and queues. |
| `guest_run` | Wall time executing a guest fiber until it yields or requests a main-stack callback, including local stack-switch overhead. This is **not** isolated guest CPU time. |
| `host_callback` | Native services invoked through the guest-to-owner callback path. |
| `graphics` | Display-list processing and submission, including render submission and synchronization. This is **not** GPU execution time. |
| `graphics_cpu` | Calling-thread CPU time during graphics submission; excludes blocked time, render workers and GPU execution. Windows CPU accounting is coarse, so compare multi-second totals. A zero count means no valid CPU sample. |
| `present` | Presentation preparation in the renderer. |
| `audio` | Native DSP synthesis. |
| `sleep` | Sleeps requested by the shared frame-pacing helper. |
| `display` | Completed swap-chain draws, including interpolation; count only. |
| `interpolated` | Display ticks that presented a generated midpoint. |

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

Configure and build with `python3 tools/build.py <game> --debug`. The binary is
`build/<game>-gles-debug/bin/<game>`; use `--help` for options. Both configurations
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
scripted validation modes have been removed. Use F12 for the shared cheat menu.

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

F6 toggles RDP wireframe in both Debug and Release. Triangles draw their edges,
and textured HUD and background rectangles draw their outlines. Guest color and
depth clears still run. Color readback includes the wireframe.
`RECOMP_DUKE_PERFORMANCE=1` and
`RECOMP_NIGHTMARE_PERFORMANCE=1` retain those games' timing logs.

The shared callback and function-candidate inspectors propose roots for review.
The shared crash inspector reads both release and debug reports. Use the
matching executable and generated sources to resolve addresses accurately.

To display the original MIPS and available recompiled x86-64 instructions for a
generated function in one run, use:

```sh
.venv/bin/python tools/analyze.py disassemble recomp_s3_7f068008
```

For an unidentified SDK routine or a hardware-register failure, generate a
diagnostic SDK symbol/handler report against the matching build metadata:

```sh
.venv/bin/python tools/analyze.py sdk-symbols --game hexen-us \
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
`failure.severity` as `warning`; the process keeps running.
An unrecognized graphics microcode skips its display list and publishes SP/DP
completion. Unsupported game-specific display-list commands skip that command.
Visuals from a skipped operation may be missing until the next task.
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

### Finding missing indirect targets across games

To record the indirect transfers taken during play, give a Debug build a new
CSV path. Play the scene and exit normally. The runtime flushes each distinct
edge before dispatch, so the file is also usable if the game stops at a missing
target. For example, these commands capture and inspect a Turok 3 session.

```sh
tools/build.py turok3-us --debug
build/turok3-us-debug/bin/turok3-us --trace-edges build/turok3-first-run.csv
.venv/bin/python tools/analyze.py transfers \
    build/turok3-us-debug/generated/turok3-us/coverage.json \
    build/turok3-first-run.csv --output build/turok3-transfers.json
```

Use the same commands with another game ID. Pass additional CSV paths from
other levels or saves to merge the distinct edges. Add `--site 0x00269c0c`
to inspect one instruction, or omit the CSVs to inspect static coverage alone.
Each trace records the cartridge SHA-1 and a digest of the analysis section
layout, including the section bytes. A blank destination section means the
loaded mappings could not identify a unique section at that instant.

The report covers translated register calls, computed jumps, returns and
relocated direct calls. Ordinary direct branches and calls already carry a
destination in the instruction. `bounded` identifies an inferred local switch
with a checked index bound, `checked` a declared target set, `partial` known
targets at an open call site, and `unknown` a site without an enumerated target.
Returns are counted separately. A `registered-target` observation identifies a
compiled function reached at a site without a matching static edge. A `review`
or `unlisted-site` observation gives the source PC and destination to investigate.
Gameplay observations leave the static classification intact.

### Recovering a missing CPU target during play

Desktop builds try a MIPS interpreter when a generated call or computed jump reaches an unregistered target. It verifies the caller, delay slot and destination instructions against live RDRAM. When a guest TLB mapping is absent, the interpreter can use a loaded direct-mapped section that the profile binds to the same cartridge bytes. Only function boundaries in the loaded overlay apply when several overlays share an address. For decompressed sections it verifies the loaded bytes against page fingerprints from the prepared ROM. It checks both paths of conditional branches before changing guest state. The original caller has already executed its delay slot. Execution resumes at the destination with the current guest registers.

The interpreter grows its instruction plan on demand up to 16,384 words from the entry. It handles common integer and floating-point instructions, branches, local jumps, and calls back into normal dispatch. For a recognized switch, discovery follows the index guard and reads the case addresses from the verified ROM table. The interpreter checks every case before execution and checks the selected register value at the jump. If it cannot establish the loaded section, verify the code, or execute the complete graph, it reports the reason and lets live repair take over. Live repair receives the loaded destination section when available. A failure after guest writes have begun uses the runtime crash reporter because restarting the callback could repeat those writes. Set `RECOMP_CPU_FALLBACK=0` to compare behavior using the existing dispatcher alone.

After a successful return, the runtime saves the caller, target, cartridge and section-layout identities, and target byte fingerprint under `local/cpu-observations/<game>/`. Close the game and run the usual desktop build, for example `tools/build.py turok3-us --release`. The prepare step tries the observations in an isolated generation and checks that the previously compiled functions still translate. It writes accepted edges to `[[discovery.observed_transfers]]` in the game profile. Calls become function entries and local jumps become destinations in the original caller. Rebuilding compiles these paths into the executable. The build keeps conflicting observations for review and prints their filenames.

On normal game exit, the CPU discovery summary shows the number of functions in the build and the distinct functions recovered during that run. It counts functions that returned through the interpreter by direct call (`JAL`) or indirect call (`JALR`), and new function entries provided by loaded live-repair modules. A function can appear in more than one group and counts once toward the total. The same address in two overlays counts as two functions. Computed jump destinations (`JR`) have their own count because a jump can land inside a function. The saved-observation count includes each caller-to-target pair written during the run.
