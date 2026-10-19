# Native threading

See the [thread ownership graph and blocking-receive sequence](process-flow.md#thread-ownership-and-concurrency)
for the scheduler, guest pthreads, processing workers and RT64 presentation flow.

Each libultra thread has a named pthread, a native stack and its own
`MipsContext`. A scheduler mutex and condition variables transfer an execution
token between the host and one guest thread. Priorities, FIFO ordering among
ready threads of equal priority, message queues and guest OSThread structures
remain managed by the shared runtime. Host scheduling priorities are not used
to implement libultra priorities.

Games can provide an entry-address-to-name table through
`RecompSchedulerConfig.thread_names` and `thread_name_count`. Names must fit
Linux's 15-byte limit; the table and strings must outlive the scheduler. The
name is applied when the pthread starts, in both Release and Debug. Matching
uses the original guest entry address, so guest IDs remain available to the
emulated OS and diagnostics. Unknown entries retain the `n64-<id>` fallback.
GoldenEye emits its checked profile's labels as `n64-main`, `n64-idle`,
`n64-scheduler`, `n64-sound` and `n64-controller`. The translated sound thread
is distinct from the `n64-audio` DSP worker. Rebuild and restart to see new names
in `top -H` or perf; existing processes retain their original names.

Blocking queue operations and stopped threads park their pthread without losing
the translated C call stack. Destroying a parked thread wakes it for exit and
joins it before releasing its storage. Generated checkpoints still provide safe
guest preemption boundaries; disabled interrupts suppress checkpoint yields.
Device services poll on the host between dispatches, rather than making a host
round trip at every translated backedge. The default interval is 64 checkpoints.

`recomp_on_host` parks the guest while the main thread executes task-submission
and AI-output callbacks. SDL input, window creation, event handling and audio
queue admission stay on that thread. This preserves application callback state
and SDL's main-thread requirements.

The persistent `n64-audio` worker executes ABI1 DSP tasks. The `n64-video` worker
processes RT64 display lists and presentation; RT64 also retains its internal
workload and presentation threads. These workers can run alongside guest CPU
execution. A worker accepts one job at a time; the native SP service still
permits only one active RSP task.

A task takes a private RAM snapshot at submission. Workers access that snapshot,
then host task services merge only bytes changed by processing. Unrelated guest
writes survive; a differing guest write to a changed output byte causes a
controlled failure. The host publishes SP/DP receipts after committing output.
The shared snapshot helper loads guest RAM once into both private buffers on
SSE2 hosts. Merging skips unchanged 64-byte blocks, using SSE2 comparisons when
`USE_INTRISICS` is enabled, then checks/writes individual changed bytes in their
original order. Other builds use ordinary copies and block comparisons. Short
tails do not read beyond the RAM buffer. Both backends use these same helpers.
Split graphics lists without a DP full sync keep their existing retirement
behavior. A yield request drains the current HLE list at its completion boundary;
HLE processing does not support suspending midway through a list.

Input, capture, statistics and destruction drain outstanding jobs when they need
their results. Backends must be destroyed before the runtime. Captured worker
failures retain their originating native stack and reach the application's
failure observer on the main thread.

Full-RAM snapshots and kernel thread handoffs have costs. This implementation
establishes native threading and processing overlap; comparative CPU and frame
pacing measurements are still needed before claiming an overall gameplay
performance improvement. See [CPU profiling](profiling.md) for measured memory
helper improvements and their limits.

Build the shared services with `cmake -S . -B build/host -G Ninja
-DRECOMP_GAME=none -DRECOMP_BUILD_RT64=OFF`, then `cmake --build build/host`.
Game diagnostic runs use the debug presets described in [debugging](debugging.md).
