# CPU profiling

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

References: [perf record](https://man7.org/linux/man-pages/man1/perf-record.1.html),
[perf report](https://man7.org/linux/man-pages/man1/perf-report.1.html),
[Linux thread CPU counters](https://man7.org/linux/man-pages/man5/proc_pid_stat.5.html).
