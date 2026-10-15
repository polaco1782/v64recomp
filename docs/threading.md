# Native threading

Each libultra thread has a named `n64-<id>` pthread, a native stack and its own
`MipsContext`. A scheduler mutex and condition variables transfer an execution
token between the host and one guest thread. Priorities, FIFO ordering among
ready threads of equal priority, message queues and guest OSThread structures
remain managed by the shared runtime. Host scheduling priorities are not used
to implement libultra priorities.

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
Split graphics lists without a DP full sync keep their existing retirement
behavior. A yield request drains the current HLE list at its completion boundary;
HLE processing does not support suspending midway through a list.

Input, capture, statistics and destruction drain outstanding jobs when they need
their results. Backends must be destroyed before the runtime. Captured worker
failures retain their originating native stack and reach the application's
failure observer on the main thread.

Full-RAM snapshots and kernel thread handoffs have costs. This implementation
establishes native threading and processing overlap; comparative CPU and frame
pacing measurements are still needed before claiming a performance improvement.

Build the shared services with `cmake -S . -B build/host -G Ninja
-DRECOMP_GAME=none -DRECOMP_BUILD_RT64=OFF`, then `cmake --build build/host`.
Game diagnostic runs use the debug presets described in [debugging](debugging.md).
