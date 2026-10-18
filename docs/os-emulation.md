# OS emulation and native calls

N64 games include statically linked **libultra** routines for threads, message
queues, timers and hardware access. Compiling their MIPS instructions for the
host preserves instruction semantics; their expected environment still needs
an implementation. Linux does not supply N64 exception handlers, CPU control
registers, interrupt sources, memory-mapped devices or RSP/RDP processors.
The shared runtime implements the OS and device behavior that the translated
game uses, including blocking, completion messages and guest-visible state.

This is **high-level emulation** at verified OS/library boundaries. A recognized
guest function is bound to a native C handler such as `hook_os_recv_mesg`.
The handler receives `Runtime *` and `MipsContext *`, reads arguments from the
guest registers and stack, accesses guest RAM through checked helpers, and
writes results back using the MIPS calling convention. Guest addresses and
libultra structures remain meaningful to the original game code.

## What runs natively

The exact replacements depend on the selected ROM and profile. The shared
services cover these groups:

| Service | Example replaced calls | Native behavior |
| --- | --- | --- |
| OS initialization and addressing | `osInitialize`, `osGetMemSize`, `osVirtualToPhysical` | Initialize supported OS state, expose configured guest RAM and resolve guest addresses through native mappings. |
| Threads, queues and events | `osCreateThread`, `osStartThread`, `osSetThreadPri`, `osSendMesg`, `osRecvMesg`, `osSetEventMesg` | Manage guest thread state, priorities, message rings, blocking and wakeups. |
| Clocks, timers and interrupts | `osGetCount`, `osGetTime`, `osSetTimer`, `__osDisableInt`, `__osRestoreInt` | Maintain N64 counter ticks, timer deadlines, event delivery and interrupt-critical sections. |
| Cartridge transfers | `osPiStartDma`, `osEPiStartDma`, `osPiRawReadIo` | Copy verified ROM data into guest RAM, service supported cartridge/save accesses and deliver completion messages. |
| Video | `osCreateViManager`, `osViSetMode`, `osViSwapBuffer` | Maintain VI configuration, framebuffer swaps and retrace events used by the game and native renderer. |
| RSP tasks and audio output | `osSpTaskLoad`, `osSpTaskStartGo`, `osAiSetFrequency`, `osAiSetNextBuffer`, `osAiGetLength` | Validate task descriptors, dispatch graphics/audio work, model the AI DMA FIFO and publish supported completion events. |
| Controllers and saves | `osContStartReadData`, `osContGetReadData`, `osPfsInitPak`, `osPfsIsPlug`, `__osContRamRead`, `__osContRamWrite`, `osEepromRead`, `osEepromWrite`, `osEepromLongRead`, `osEepromLongWrite` | Expose native input through guest controller structures and SI events; persist configured EEPROM and Controller Pak devices. SRAM uses the cartridge-transfer service. |
| Cache maintenance | `osWritebackDCache`, `osInvalDCache`, `osInvalICache` | Validate supported ranges while coherent host memory supplies CPU/backend visibility. |
| Common memory/string routines | `memcpy`, `memset`, `bcopy`, `bzero`, `bcmp`, `strlen`, `strchr` | Operate on guest addresses while preserving byte order, return values and each routine's overlap behavior. |

Ordinary game logic, including movement, physics, AI and command-list
construction, remains compiled from the ROM. Graphics tasks go to RT64, while
supported ABI1 and Zelda NEAD audio tasks go to the native DSP backend and SDL
playback. These backends execute supported task operations separately from the
recompiled CPU code. Game-specific adapters handle custom startup, paging,
graphics extensions and overlay relocation. Device and task coverage remains
specific to each port; see [game support](status.md).

## Controller Pak saves

`RecompIOConfig.pak_mask` selects ports with a 32 KiB Controller Pak;
`pak_paths[4]` supplies an optional raw `.mpk` image for each port. A null path
keeps that Pak in memory. A new disk image receives the standard ID copies,
inode tables and empty 16-note directory, with 123 free 256-byte pages. Existing
images must be exactly 32768 bytes. Writes use a temporary file, `fsync` and
rename; a failed write reports `PFS_ERR_CONTRFAIL` and keeps the previous
in-memory contents. EEPROM and Pak files must have separate paths.

`osPfsInitPak` initializes the guest's 0x68-byte `OSPfs` handle. Controller
initialization, queries and `osPfsIsPlug` report the configured accessories.
The verified `__osContRamRead`, `__osContRamWrite` and `__osPfsGetStatus`
boundaries access the device directly, so file allocation, lookup, reads,
writes and deletion still run through the game's translated libultra code.
The [SDK file API](https://ultra64.ca/files/documentation/online-manuals/man-v5-2/allman52/n64man/os/osPfsReadWriteFile.htm)
uses 32-byte transfers within allocated notes. Only single-bank Controller
Paks are supported. Initialization reports damaged IDs or inconsistent
filesystems instead of automatically repairing or formatting existing saves.

Duke Nukem 64, Nightmare Creatures, Quake 64 and Shadow Man configure a Pak on
controller port 1. Their Release applications save to
`local/saves/<game-id>.mpk`; `--save FILE` overrides that path. Their Debug
applications use `<ROM-path>.mpk`. Other games keep their existing save-device
configuration. Gameplay save/load acceptance remains to be tested.

## Preserving scheduling and completion behavior

Each guest thread has a host pthread, native stack and `MipsContext`. The
scheduler grants an execution token to one guest CPU thread at a time and
implements libultra priorities itself. Blocking operations park that pthread
with its translated C call stack intact. Generated checkpoints provide safe
preemption boundaries, and disabled guest interrupts suppress checkpoint
yields.

For example, `osRecvMesg(queue, &message, OS_MESG_BLOCK)` parks the caller when
the queue is empty. Another ready guest thread can run. A matching timer,
retrace, device completion or message send can wake the receiver; it resumes
its existing call stack when scheduled. Returning immediately from every OS
call would break this synchronization and change the game's behavior.

Graphics and audio workers can run alongside guest CPU execution. Task workers
use private RAM snapshots; their changed bytes are committed before the host
publishes SP/DP completion events. AI buffer timing and completion are modeled
by the runtime. SDL sound-device progress does not determine guest AI completion.

## How replacements are selected

[`tools/native_hooks.toml`](../tools/native_hooks.toml) records known SDK/library
bodies. Discovery scans the configured ROM regions, uses instruction prefixes
to find candidates, and requires a **complete-body SHA-256 match** before
binding a shared handler. Explicit game-profile hooks take precedence for
custom behavior. Unrecognized routines follow the normal translation path;
unsupported instructions or device operations fail explicitly.

When a native target is known and uniquely registered, generated code can call
its handler directly while retaining loaded-section validation and the game's
`prepare_call` callback. Guest return-address updates and branch delay slots
retain their MIPS semantics. Dynamic or ambiguous targets use `recomp_lookup`.

See [native hooks](native-hooks.md), [threading](threading.md),
[audio](audio.md) and [the recompilation pipeline](recompilation.md)
for implementation details.
