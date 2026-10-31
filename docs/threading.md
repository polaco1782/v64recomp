# Native threading

See the [thread ownership graph and blocking-receive sequence](process-flow.md#thread-ownership-and-concurrency)
for the scheduler, guest pthreads, synchronous HLE and RT64 presentation flow.

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
builds audio lists; native DSP synthesis runs on the main host thread. Rebuild
and restart to see new names in `top -H` or perf; existing processes retain
their original names.

Blocking queue operations and stopped threads park their pthread without losing
the translated C call stack. Destroying a parked thread wakes it for exit and
joins it before releasing its storage. Generated checkpoints still provide safe
guest preemption boundaries; disabled interrupts suppress checkpoint yields.
Device services poll on the host between dispatches, rather than making a host
round trip at every translated backedge. The default interval is 64 checkpoints.

Each generated backedge first asks whether a checkpoint is due. This decision
updates the thread's interval counter and checks priority preemption without
reading guest registers, polling devices or yielding. Only a due checkpoint
flushes cached GPRs, runs the scheduling action, and reloads the GPRs on return.
At a generated checkpoint, a reload is omitted only when straight-line analysis
proves that the resumed code overwrites that register before reading it or
reaching another control-flow/synchronization boundary. Read-modify-write and
merge-load inputs retain their reloads. All written GPRs are still saved before
yielding, so the scheduler sees the complete current context. Calls and returns
retain their existing synchronization; this optimization does not assume that
runtime callbacks preserve guest registers.
The priority check caches the highest ready-thread priority, including an empty
ready set, so unchanged loops do not scan the thread list on every backedge.
Readying, suspending, dispatching, stopping or retiring a thread, and changing
its priority, invalidate that cache. New threads are created stopped; restarting
a stopped queue waiter either keeps it waiting or readies it through the same
invalidation path. Actual dispatch still selects by priority and FIFO order.
The cache does not change checkpoint intervals or delay priority preemption.
Forced checkpoints still yield immediately when interrupts are enabled;
disabled interrupts skip both the decision and synchronization. Calls from
native OS hooks use the same decision with their already synchronized context.

Logical CPU interrupt enable and SDK interrupt masks belong to each guest
thread. Dispatch restores them, and blocking/preemption saves them before
another thread runs. A blocking receive inside a critical section must resume
with interrupts still disabled without disabling the next thread's checkpoints
and device polling. `tools/check_thread_interrupts.c` covers that transition,
including a higher-priority receiver preempting its sender.

Creating a new entry in a stopped thread object retires and joins its old native
stack before resetting its context. This supports reusable loader slots such as
RE2's; a runnable thread, the current thread, or a stopped queue waiter cannot
be overwritten. `tools/check_thread_recreate.c` verifies that a recreated thread
starts with its new argument and that the abandoned call stack never resumes.

Saved register contexts use a serialized-word cache for direct KSEG0/KSEG1 RAM
on little-endian hosts. A running thread refreshes its cache at synchronization;
a parked thread reuses its last context. Each synchronization checks the live
RAM against the cached bytes and restores changed context ranges, so guest or
native RAM writes cannot silently invalidate the saved state. One checked span
replaces per-register address resolution. The serializer writes only GPR/LO/HI,
PC, FCR31 and the configured FR=0 or FR=1 register area; status, exception and RCP
fields retain their existing ownership. Mapped objects, invalid spans and other
host byte orders use the original checked stores. Thread metadata, queue links
and event ordering continue through the existing synchronization paths.

`recomp_on_host` parks the guest while the main thread executes task-submission
and AI-output callbacks. SDL input, window creation, event handling and audio
queue admission stay on that thread. This preserves application callback state
and SDL's main-thread requirements.

Graphics display-list parsing, framebuffer readbacks and audio DSP synthesis
execute synchronously on the owner with guest CPU execution parked. Both use
live `Runtime.rdram`; there are no task RAM snapshots, full-RAM merges or
`n64-audio`/`n64-video` adapter threads. This follows the execution model of
UltraHLE's synchronous HLE path. The SP service still permits one active task.

RT64 captures vertices, matrices, textures and framebuffer data into its own
workload/presentation storage before returning from the owner call. Its internal
render, upload and present threads, GPU rendering and SDL playback remain
asynchronous and can overlap guest execution. No RT64 queue receives a pointer
into guest RAM for later consumption. Texture reload, capture and shutdown keep
the explicit RT64 queue barriers they require.

Audio writes PCM and decoder state directly into live RAM before queuing SP
completion. Graphics queues SP and applicable DP receipts after parsing and
readbacks; completion does not wait for GPU presentation. The task service
delivers queued guest events during polling, in SP-before-DP order. A split list
without DP full sync still retires after SP. HLE tasks finish at whole-list
boundaries; a late yield first polls their queued completion.

Fatal HLE failures now occur on the owner and use the shared crash reporter.
Backends must be destroyed before the runtime. Four-MiB games retain an eight-MiB
host allocation for RT64's physical address window and RAM dump; guest bounds
and Expansion Pak detection still use the configured four-MiB logical size.

The synchronous path removes snapshot copying, merge scanning and adapter-worker
handoffs, but also removes CPU/HLE overlap. Compare equivalent settled scenes
before claiming a gameplay improvement; see [CPU profiling](profiling.md).

Build the shared services with `cmake -S . -B build/host -G Ninja
-DRECOMP_GAME=none -DRECOMP_BUILD_RT64=OFF`, then `cmake --build build/host`.
Game diagnostic runs use the debug presets described in [debugging](debugging.md).
