# CPU profiling

## Built-in profiler, including Steam Deck

Every game using the shared command line accepts `--profile NEW_DIRECTORY`.
It collects process and per-thread CPU totals, hot functions, and sampled call
stacks inside the executable. It needs no Python, `perf`, root access, or kernel
permission changes. The built-in sampler currently supports **x86-64 Linux**.

From an extracted Steam Deck package, for example:

```sh
./launch.sh --profile deck-profile --profile-delay 20 --profile-seconds 30
```

Reach the scene during the first 20 seconds, then play it for the next 30 seconds.
The report is written when the capture finishes; the game continues running.
Increase the delay for a longer startup. The directory's parent must exist, and
the capture directory must be new. Use a different name for each measurement.

Without `--profile-seconds`, profiling runs until normal exit (Escape or the
`--frames N` budget). Close normally to save the report; forced termination or a
crash before the capture ends can lose it. A run that exits before the delay
expires prints a message and produces no CPU report. No collection runs unless
`--profile` is supplied.

The capture contains:

| File | Contents |
| --- | --- |
| `hotspots.txt` | Process CPU totals, all observed thread totals, and the top 100 functions by self cost |
| `thread-hotspots.txt` | Top 30 self-cost functions within each sampled thread |
| `functions.csv` | All resolved functions or unresolved instruction addresses, with self and inclusive sample counts |
| `threads.csv`, `summary.json` | CPU counters, user/kernel split, sampling errors, overruns, dropped samples, and stack-depth diagnostics |
| `stacks.folded` | Counted root-to-leaf stacks, grouped by native TID, usable with folded-stack flame graph tools |
| `samples.jsonl` | Aggregated leaf-to-root stacks with counts, TIDs, native PCs, module paths, and load bases/offsets |
| `maps.txt`, `maps-end.txt`, `process-status.txt` | Process mappings at capture start/end and initial Linux status |

Start with `hotspots.txt` to identify the busy thread, then find its TID in
`thread-hotspots.txt`. `recomp_s<section>_<vram>` identifies translated guest
functions; scheduler, memory, worker, audio, RT64, and driver symbols identify
native work. Folded stacks retain the caller paths behind those costs. C++ names
are demangled when libstdc++ is available. The collector also reads native ELF
symbol tables to name static runtime helpers and RT64's hidden functions. Local
functions include an address suffix to distinguish identical names. Stripped
libraries can still appear as module offsets; keep the matching executable and
libraries to resolve those addresses with `addr2line` or another native symbolizer.

CPU percentages use **one logical CPU = 100%** and can sum above 100%. Process
user/kernel totals include all process threads, including threads that exit
between observations. Per-thread counters are polled every 100 ms; short-lived
threads and the final CPU burst before thread exit may be missed. The signed
`thread_accounting_gap_seconds` shows process CPU minus the observed thread CPU;
small negative gaps can result from tick granularity and observation timing.
This profiles CPU execution, not GPU execution time or time blocked on a GPU.

The default is **99 samples per CPU second per thread**, using per-thread Linux
CPU timers and a dedicated realtime signal (`SIGRTMIN+4`). Sleeping threads
consume no sample budget. The collector discovers new threads at each poll and
excludes itself from sampling, while retaining its CPU counters. Timer overruns,
ring-buffer drops, resource limits, and individual timer failures are reported,
so unavailable samples are distinguishable from idle threads. The sampler has
bounded storage: 1,024 observed thread lifetimes, 65,536 unique instruction
addresses, 65,536 unique stacks, and 32 frames per stack. CPU totals continue
when a sampling storage limit is reached. `discarded_stacks`, `thread_limit_hits`
and `allocation_failures` flag lost detail.

Stacks use interrupted x86-64 registers and bounded, fault-safe reads through
`/proc/self/mem`; allocation, symbol resolution, and file output run on the
collector thread. Our generated code, runtime, audio adapter, renderer adapter,
RT64 and Plume retain frame pointers. Third-party code may omit frame pointers,
optimized tail calls may disappear, and deep stacks may be truncated. Samples
include user and kernel CPU time, but kernel work is attributed to the userspace
instruction where the signal is delivered; they do not provide kernel stacks.
Use the external perf workflow below for DWARF unwinding or kernel attribution.

Profiling itself consumes CPU: collector work appears as `n64-profiler`, while
signal-handler work is charged to the sampled thread. Use `--profile-hz 49` to
reduce sampling overhead or raise it up to 1,000 for a short detailed capture.
Compare equivalent Release scenes, and measure final performance with profiling
disabled.

## Frame pacing and stutter

CPU sampling does not explain time spent asleep. Set `RECOMP_FRAME_TRACE` to a
new CSV path to record host presentation requests, graphics submissions, graphics
and presentation worker jobs, observed graphics completions, and display draw
callbacks. The parent directory must exist; existing files are rejected.

```sh
RECOMP_FRAME_TRACE=build/quake-frames.csv ./build/quake-us-release/bin/quake-us
python3 tools/profile_frames.py build/quake-frames.csv --start 30 --end 50
```

Exit normally before reading the buffered trace. Window bounds are seconds
after the first recorded event. Use the same settled scene, replay, build and
graphics settings for comparisons. `--json` emits the report as JSON.

The report includes median, p95, p99 and maximum intervals/job durations, and
counts display/presentation/full-sync intervals exceeding 50 ms. `graphics_end`
has value 1 when the task produced a DP receipt; split tasks without full sync
have value 0. Graphics begin/submit values are microcode addresses; presentation
values are VI origins. The receipt delay ends when the host observes the worker
finish, before RAM merging and guest event delivery.

These timestamps measure **CPU callbacks**, not GPU execution, compositor
scanout, or unique game frames. A full-sync task is not universally one game
frame. Display callbacks can repeat a buffer or include interpolation. Use them
alongside scene captures and task timing; avoid calling their rate game FPS.
Tracing performs buffered file writes and can affect timing, so also verify
ordinary play with tracing and sampling disabled.

Frame loops that sleep a whole presentation interval can delay guest VI/AI and
task servicing even when rendering is cheap. `recomp_game_pace(rt, deadline)`
runs useful guest work and polls services between display ticks, waking at the
earlier of a VI/AI/timer deadline or 1 ms when the guest is waiting. Set the
scheduler's `idle_thread` only after verifying that the corresponding priority-zero ROM
thread does no useful work. The guest clock must follow wall time before using
this helper; retain plain sleeping during synthetic-clock bootstrap or
frame-exact diagnostic runs.

| Games | Pacing policy, Debug and Release |
| --- | --- |
| Quake, Ocarina of Time, Nightmare Creatures | Shared `recomp_game_pace` |
| Hexen, Duke Nukem: Zero Hour | Shared helper through their parent-folder adapters |
| Perfect Dark, Conker, GoldenEye | Shared helper after the first-graphics wall-clock handoff; synthetic bootstrap retained |
| Duke Nukem 64 | Retains calibration-aware draining, detection of the game's clock-poll loop, and Debug sleep accounting |
| Super Mario 64 | Retains queue-aware draining and frame-exact Debug routes; those paths require the existing scheduler return and frame-count behavior |
| Shadow Man | Retains its existing loop pending verification of an idle-thread object; a default scheduler drain could otherwise spin on its idle thread |

The additional game migrations were checked by compilation/static review;
gameplay validation is left to the user. They do not establish a measured
performance gain for those games. Keep game-specific startup calibration,
input/replay boundaries, captures and presentation deadline policies in the
game adapter.

GoldenEye configures its ROM-verified priority-zero idle object so the helper
can wait without dispatching the empty idle loop. Once its clock follows wall
time, the app runs a small scheduler batch before presentation and services
the remaining guest work within the pacing deadline. Synthetic-clock startup
uses a separate drain capped at 4096 scheduler resumes.

## External capture with perf

Use `tools/profile_execution.py` on Linux to attach to a running game without
restarting it. Python 3 is sufficient for thread CPU accounting. Install your
distribution's `perf` package matching the running kernel for function sampling.
The tool does not change kernel permissions or invoke `sudo`.

Start a **Release** build, reach the scene to measure, and let shader compilation
settle. Release game targets retain symbols and frame pointers; shared RT64 uses
optimized `RelWithDebInfo` by default. Debug game code is unoptimized and includes
extra diagnostics, so its CPU consumption is not representative of Release.

## Capture

From the repository root, measure every thread for 30 seconds:

```sh
python3 tools/profile_execution.py threads --game goldeneye-us --duration 30
```

Capture thread counters and sampled function call stacks together:

```sh
python3 tools/profile_execution.py record --game goldeneye-us --duration 30
```

`--game` matches the executable basename and requires exactly one matching
process. Use `--pid 660450` instead when multiple instances are running. A sandbox
or PID namespace may hide a process that is visible in your desktop terminal;
run the tool in that terminal in this case.

By default, captures go to a unique `build/profiles/<timestamp>-<pid>/` directory.
Use `--output build/profiles/ge-dam` to name a capture. This directory must not
already exist, so a new capture cannot overwrite earlier evidence. Ctrl-C ends
the capture and saves partial counters; it does not stop the attached game.

`record` defaults to `--backend auto`: if perf is missing or fails, it keeps the
thread capture and prints that no function profile was captured. Use
`--backend perf` when function sampling is required; unavailable/failed perf then
returns a nonzero status. Check `summary.json` and `perf-record.log` for the actual
failure. Thread counters usually remain available even when perf permissions are
restricted. An empty function report can also mean the game was asleep during
the capture.

The default event is `cpu-clock:u` at 199 samples per second per active thread,
with `dwarf,16384` stack unwinding. It samples **userspace CPU time** and excludes
blocked time and kernel execution. Lower `--frequency 99` to reduce recording
overhead. `--call-graph fp` produces smaller recordings, but renderer or driver
stacks may be incomplete if those libraries omit frame pointers. Keep the
executable and generated source files used during capture available for analysis.

To sample one native thread while still accounting for every thread:

```sh
python3 tools/profile_execution.py record --pid 660450 --tid 660517 --duration 30
```

Thread IDs change between runs. Choose the current TID from the thread table or
`top -H -p PID`. If kernel CPU time dominates the thread table, `--event cpu-clock`
also requests kernel samples, subject to your system's perf permissions.

## Read a capture

Each capture contains:

| File | Contents |
| --- | --- |
| `summary.json` | Executable, build settings when available, capture options/status, total thread CPU, per-thread counters and sampled wait channels |
| `threads.csv` | Per-thread CPU percentages, user/system seconds and context-switch deltas |
| `threads.jsonl` | Periodic thread observations with cumulative CPU counters |
| `maps.txt` | Process mappings when readable |
| `perf.data` | Raw function samples, when perf succeeds |
| `perf-record.log` | perf diagnostics, including lost samples and capture errors |
| `perf-self.txt` | Direct CPU cost by thread and function, with pipe-separated fields |
| `perf-calltree.txt` | Inclusive cost and call paths by thread |

Thread CPU percentages use **one logical CPU = 100%**; total process usage can
exceed 100%. They are based on CPU counter deltas during this capture, rather
than the process's lifetime average. User/system seconds include kernel work in
the system column. The counters have the kernel's clock-tick granularity.
Short-lived threads may lose CPU time before their first or after their last
observation, and threads entirely between polls are missed. State/wait-channel
counts are observations, not time measurements or function samples. A thread
being asleep when observed does not imply it used no CPU between observations.

Inspect the interactive call tree, or generate reports filtered to a TID:

```sh
perf report -i build/profiles/ge-dam/perf.data
python3 tools/profile_execution.py report build/profiles/ge-dam --tid 660517
```

The latter writes `perf-self-660517.txt` and `perf-calltree-660517.txt`.
Filtered report percentages are relative to that thread's selected samples.
Unfiltered perf percentages are shares of the captured userspace CPU samples;
they are **not** the same as the thread table's percentages of a logical CPU.
`Self` measures a function's own execution. `Children` also includes callees;
inclusive percentages overlap and must not be added together.

Route hot symbols to the corresponding code:

| Symbols | Code to inspect |
| --- | --- |
| `recomp_s<section>_<address>` | Generated game functions; section and guest entry address are encoded in the name |
| `ram_address`, `recomp_load*`, `recomp_store*` | Shared memory helpers in `runtime/memory.c` |
| `mips_fp_*` | Floating-point helpers in `runtime/fpu.c` |
| `sync_guest`, `save_context`, `recomp_scheduler_run` | Guest scheduling/context serialization in `runtime/scheduler.c` |
| `recomp_worker_merge` | RAM snapshot reconciliation in `runtime/worker.c` |
| `RT64::…`, Vulkan driver symbols | Renderer/driver CPU work |
| Audio synthesis functions | Audio backend/HLE work |

Map a generated function's guest address to discovery evidence using the
existing address inspection tool:

```sh
.venv/bin/python tools/inspect_address.py --help
```

For instruction-level inspection, run `perf annotate -i CAPTURE/perf.data` and
select a hot symbol. Optimization and inlining can make source attribution
approximate. CPU samples identify execution cost; diagnosing time blocked on
locks or GPU presentation may require separate scheduling/GPU measurements.

## GoldenEye threads

The host thread is named `goldeneye-us`. Its Release frame loop drains guest
scheduling, services audio/graphics completion, updates input, presents and waits
for the next frame. Guest pthread names come from the entry-address labels in
the game profile, with `n64-<guest ID>` as a fallback for unknown entries.
GoldenEye uses `n64-main` (formerly `n64-3`), `n64-idle` (`n64-1`),
`n64-scheduler` (`n64-2`), `n64-sound` (`n64-4`) and `n64-controller` (`n64-5`).
The old suffix identified a guest thread, not a host CPU number. `n64-video` is the shared
graphics worker that processes display lists and presentation. RT64 also creates
its own workload, texture, buffer and presentation threads. `n64-audio` is the
audio worker.

High host-thread CPU can come from services invoked on behalf of the guest:
whole-RAM task snapshots, snapshot merges and guest context serialization. Use
the call tree to distinguish these from actual game computation on `n64-3`.
Many voluntary context switches suggest frequent scheduler/worker handoffs, but
the count alone does not measure their CPU cost. Compare the same scene and
settings before and after an optimization, and check both CPU use and frame time.

### Observed GoldenEye capture, 2026-10-18

A 15-second capture of the user's running Release process, PID 660450, measured
66.8% total CPU across all observed threads. The main host thread used 24.6%,
guest thread 3 used 23.5%, and `n64-video` used 11.1%. These are percentages of
one logical CPU, including user and system time. The scene was controlled by
the user; this capture does not establish performance across other levels.

In the host thread's userspace **Self** samples, `recomp_worker_merge` accounted
for 29.0%, RAM address resolution for 18.3%, RAM snapshot copies (`memmove`) for
17.7%, and `recomp_store` for 12.8%. The call tree traced copies to graphics/audio
submission, merges to their completion services, and many checked stores to
`sync_guest` / `save_context`. This makes task snapshots/merges and repeated
context serialization concrete optimization candidates.

On guest thread 3, `mips_fp_binary` accounted for 24.0% of that thread's userspace
Self samples, address resolution 13.0%, and `recomp_store` 7.3%. Graphics-worker
samples centered on RT64 texture hashing/loading, vertex handling and texture
coordinates. The host and guest thread 3 made approximately 19,200 and 17,900
voluntary context switches per second respectively, consistent with frequent
handoffs. Kernel cost is included in the CPU table but excluded from this
userspace function recording; these switches alone do not quantify its cost.

The capture and filtered per-thread reports are under the ignored directory
`build/profiles/goldeneye-live-660450-confirmed/`. The original executable was
retained there under `executable/` before rebuilding for readable thread names.
No runtime performance changes were made as part of this measurement.

### Memory helper optimization, 2026-10-18

The subsequent shared-runtime change replaces four-byte merge comparisons with
64-byte comparisons. SSE2 compares four vectors and skips the block when all
bytes match; changed blocks still use the original byte-level conflict checks
and ascending write/failure order. Snapshot creation feeds both private buffers
from each input vector, avoiding the second full-buffer read. Audio and graphics
both use `recomp_worker_snapshot`. `USE_INTRISICS=0` retains ordinary copies and
portable block comparisons.

Local microbenchmarks on the Ryzen 7 5700X, compiled with Clang `-O3`, measured:

| Operation | RAM size | Previous | Optimized |
| --- | --- | --- | --- |
| Merge, unchanged | 4 MiB | 0.711 ms | 0.077 ms |
| Merge, one changed byte per 4 KiB | 4 MiB | 0.706 ms | 0.125 ms |
| Merge, 32 changed bytes per 4 KiB | 4 MiB | 0.723 ms | 0.129 ms |
| Create both snapshots | 4 MiB | 0.181 ms | 0.148 ms |
| Merge, one changed byte per 4 KiB | 8 MiB | 1.425 ms | 0.275 ms |
| Create both snapshots | 8 MiB | 0.637 ms | 0.508 ms |

These are medians of three runs, each averaging 60 merge or 80 snapshot
iterations over preallocated, warmed buffers. Merging a completely changed
buffer was approximately unchanged (4 MiB: 2.576 vs 2.548 ms). This measures the
helpers, not total game CPU or frame rate; the original gameplay perf capture
predates these changes.

SIMD and portable helpers each passed 7,000 differential cases under Address
and UndefinedBehavior Sanitizers, including conflicting writes, conflict
addresses, partial-write order, unrelated guest writes, equal concurrent output,
unaligned buffers, short/tail sizes and guard pages. LeakSanitizer was disabled
because the execution sandbox does not support it. Release and Debug runtime
builds passed. Benchmark sources/results and the bounded GoldenEye smoke runs
are under `build/profiles/memory-optimization/`. GoldenEye Release completed an
1,800-frame run with its intro visibly rendered and no runtime failure. This
startup smoke check does not establish gameplay performance or full-game
regression coverage.

### Inline variable-address RAM loads, 2026-10-18

Release `recomp_load8/16/32/64` now inline the direct-RAM path even when the
guest address is computed at runtime. The address classification and byte/word
ordering are unchanged. ROM, MMIO, physical addresses and mapped/custom-pager
addresses still go through the shared helpers. Debug keeps alignment/range
diagnostics. This does not add page translation caches or change stores.

On the same host, Clang `-O3` load-loop microbenchmarks measured these medians:

| Width | Previous | Inline variable path |
| --- | --- | --- |
| 8-bit | 1.569 ns/load | 0.642 ns/load |
| 16-bit | 1.098 ns/load | 0.688 ns/load |
| 32-bit | 1.302 ns/load | 0.566 ns/load |
| 64-bit | 1.308 ns/load | 0.565 ns/load |

Each result is the median of five 12-million-load loops over 1,024 computed,
aligned addresses in a warmed 64 KiB RAM region, alternating KSEG0/KSEG1 aliases.
Separate translation units compiled against the previous/current headers call
the same runtime library. Physical-address fallback loops remained approximately
unchanged (about 13–15 ns/load). Disassembly confirmed that direct 32-bit reads
execute inline; the shared helper is called on the fallback branch.

GoldenEye's 35 generated CPU objects grew from **9,224,565 to 11,059,040 bytes of
`.text` (+19.89%)**, using the existing Release preset. The larger instruction
footprint is a material tradeoff: these focused timings do not establish an
overall gameplay improvement, and a same-scene CPU/frame-time comparison is
still needed after restarting the rebuilt game.

Release and Debug each passed 159,368 load comparisons covering all widths,
physical/KSEG aliases, native page mappings, custom backing with unusual
alignment, ROM reads and PI MMIO dispatch. Failure checks retained out-of-range
diagnostics and Debug alignment failures. The fixtures used Address and
UndefinedBehavior Sanitizers; LeakSanitizer was disabled for the sandbox.
Release and Debug runtime builds passed. Evidence is under
`build/profiles/inline-loads/`. GoldenEye Release also completed an 1,800-frame
startup run with the intro visibly rendered and no runtime failure; full
gameplay performance remains unmeasured.

References: [perf record](https://man7.org/linux/man-pages/man1/perf-record.1.html),
[perf report](https://man7.org/linux/man-pages/man1/perf-report.1.html),
[Linux thread CPU counters](https://man7.org/linux/man-pages/man5/proc_pid_stat.5.html).
