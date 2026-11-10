# Native threading

See the [thread ownership graph and blocking-receive sequence](process-flow.md#thread-ownership-and-concurrency)
for the scheduler, guest fibers, synchronous HLE and RT64 presentation flow.

Each libultra thread has a stackful fiber, a native C stack and its own
`MipsContext`. The scheduler resumes one fiber at a time on the main owner OS
thread; blocking and checkpoints switch back to the scheduler's main stack.
Guest scheduling does not create pthreads, acquire a scheduler mutex or wait on
condition variables. Priorities, FIFO ordering among ready threads of equal
priority, message queues and guest OSThread structures remain managed by the
shared runtime.

The pinned minicoro implementation performs the stack switches. Each fiber
reserves an 8 MiB virtual stack by default, with physical pages allocated as
used; `native_stack_size` can select another size. Suspended or retired fibers
release their stack when destroyed. The owner and each fiber retain separate
host floating-point environments in addition to their guest FCR31/FPR state.

Games can provide an entry-address-to-name table through
`RecompSchedulerConfig.thread_names` and `thread_name_count`. Names must fit
15 bytes; the table and strings must outlive the scheduler. These are logical
guest-thread labels used in diagnostics, rather than native OS thread names.
Matching uses the original guest entry address, so guest IDs remain available to the
emulated OS and diagnostics. Unknown entries retain the `n64-<id>` fallback.
GoldenEye emits its checked profile's labels as `n64-main`, `n64-idle`,
`n64-scheduler`, `n64-sound` and `n64-controller`. The translated sound thread
builds audio lists; native DSP synthesis runs on the main stack. All guest CPU
samples now belong to the main OS thread in `top -H` and perf; generated function
names still identify the guest code being sampled.

Blocking queue operations and stopped threads suspend their fiber without losing
the translated C call stack. Destroying a parked thread discards its suspended
stack without resuming its guest code. Generated checkpoints still provide safe
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
and device polling, including when a higher-priority receiver preempts its sender.

Configure `idle_thread` only when that thread has completed initialization.
Games that start their initialization thread at priority zero can call
`recomp_scheduler_set_idle_thread` at the verified handoff to the main thread.
This keeps its startup work intact and leaves the guest thread runnable while
letting the owner wait for hardware deadlines instead of executing its busy loop.

Creating a new entry in a stopped thread object retires and destroys its old native
stack before resetting its context. This supports reusable loader slots such as
RE2's; a runnable thread, the current thread, or a stopped queue waiter cannot
be overwritten. A recreated thread starts with its new argument; the abandoned
call stack never resumes.

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

`recomp_on_host` yields the guest fiber while the scheduler executes task-submission
and AI-output callbacks on its main stack. Calls already on the main stack execute
directly. SDL input, window creation, event handling and audio
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

Guest and HLE failures occur on the owner and use its shared crash reporter.
Diagnostic unwinding stops at the guest fiber's entry stack boundary.
Backends must be destroyed before the runtime. Four-MiB games retain an eight-MiB
host allocation for RT64's physical address window and RAM dump; guest bounds
and Expansion Pak detection still use the configured four-MiB logical size.

The synchronous path removes snapshot copying, merge scanning and adapter-worker
handoffs, but also removes CPU/HLE overlap. Compare equivalent settled scenes
before claiming a gameplay improvement; see [CPU profiling](profiling.md).

Build the shared services with `cmake -S . -B build/host -G Ninja
-DRECOMP_GAME=none -DRECOMP_BUILD_RT64=OFF`, then `cmake --build build/host`.
Game diagnostic runs use the debug presets described in [debugging](debugging.md).
