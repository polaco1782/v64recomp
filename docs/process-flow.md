# Complete process flow

v64recomp translates a verified N64 ROM's CPU instructions into C during the
build. The host compiler produces native game code. During play, that code uses
shared native services for the N64 OS, devices, graphics and audio.

The graphs below show the whole lifecycle, followed by the generation pipeline,
runtime execution, threading and failure investigation. Solid arrows show control or data
flow; dotted arrows show configuration inputs or feedback.

## Whole lifecycle

```mermaid
flowchart TD
    ROM["User-provided N64 ROM"] --> Import["Normalize byte order<br/>Verify exact size and SHA-1"]
    Profile["games/&lt;game&gt;/profile.toml<br/>Checked mappings, roots, tables and hooks"] -.-> Import
    Import --> Local["Verified original ROM in local/"]
    Local --> Generate["Prepare analysis image if needed<br/>Discover functions and translate MIPS to C"]
    Profile -.-> Generate
    Generate --> Sources["Generated CPU C files<br/>Registry, headers and reports in build/"]
    Sources --> Compile["Host C/C++ compilation and linking"]
    Shared["Runtime + game adapters<br/>Debug or Release app<br/>Shared RT64 + native audio"] --> Compile
    Local --> Embed{"Embed ROM content?"}
    Sources -. "checked instruction ownership" .-> Embed
    Embed -->|"Yes, supported layout"| Strip["Erase verified CPU instructions<br/>Preserve offsets and required data<br/>Generate embedded content source"]
    Strip --> Compile
    Compile --> Binary["Native game executable"]
    Binary --> Pack{"Optional pack target?"}
    Pack -->|Yes| UPX["UPX compression and integrity test<br/>Separate packed executable"]
    Pack -->|No| Launch["Launch game"]
    UPX --> Launch
    Embed -->|"No: external ROM"| External["Original ROM remains runtime input"]
    External --> Load
    Launch --> Load["Allocate runtime and verify ROM content<br/>Install shared crash reporter"]
    Load --> Boot["Load boot sections and parameters<br/>Initialize scheduler, devices and backends<br/>Enter compiled guest startup"]
    Boot --> Play["Run compiled game logic<br/>Schedule guest threads and process events"]
    Play --> Output["RT64 graphics + SDL audio<br/>Controller input + persistent saves"]
    Output --> Play
    Play --> Exit{"Close or frame limit?"}
    Exit -->|No| Play
    Exit -->|Yes| Cleanup["Drain workers and finalize captures<br/>Destroy backends, then runtime<br/>Free ROM memory"]
    Play -->|"Controlled failure"| Crash["Shared crash JSON + RDRAM snapshot"]
    Generate -->|"Discovery or translation issue"| Review["Inspect evidence and fix owning layer"]
    Crash --> Review
    Review -. "checked metadata or generator fix" .-> Generate
    Review -. "runtime or backend fix" .-> Compile
```

Embedding and UPX packing are independent options. Embedding currently requires
the prepared analysis image to equal the original cartridge image; compressed
layouts that append decoded code are rejected. CPU stripping follows verified
instruction ownership and checked exceptions. Partial discovery can leave
undiscovered CPU code in the embedded content. See [ROM content packaging](rom-packaging.md).

## Generation and compilation

```mermaid
flowchart TD
    Configure["CMake preset<br/>Select game, build directory and Debug/Release"] --> Driver["cmake/Game.cmake invokes<br/>tools/generate_game.py"]
    Inputs["Pinned original ROM + profile.toml<br/>Game generate.py + checked inventories<br/>Shared generator code and hook catalog"] --> Driver
    Driver --> Cache{"Input fingerprint matches<br/>and outputs exist?"}
    Cache -->|Yes| Reuse["Reuse generated files"]
    Cache -->|No| Prepare["Optional game prepare hook<br/>Decompress or map analysis code"]
    Prepare --> Discover["discover.py + Rabbitizer<br/>Decode MIPS and walk control flow"]
    Checks["Verified roots and function entries<br/>Jump tables, transfers and inventories<br/>Complete-body native hook fingerprints"] -.-> Discover
    Discover --> Symbols["Optional finalize_metadata hook<br/>Write and validate symbols.json"]
    Symbols --> ConfigTranslate["Optional configure_translation hook<br/>Normal generation only"]
    ConfigTranslate --> Translate["translate.py + emit.py<br/>Validate selected function bodies"]
    Translate --> Coverage["coverage.json<br/>Ready, hooked, unsupported and unresolved status"]
    Translate --> Report{"Report-only mode?"}
    Report -->|Yes| ReportEnd["Reports only<br/>No compilation or launch"]
    Report -->|No| Valid{"Selected functions satisfy<br/>translation policy?"}
    Valid -->|No| Repair["Review mappings, ownership and instructions<br/>Correct checked inputs or implementation"]
    Repair -.-> Inputs
    Valid -->|Yes| Emit["Emit functions.c and generated.h<br/>MIPS semantics become C control flow and helpers"]
    Emit --> Config["Game write_config hook<br/>Emit boot, device and game configuration"]
    Config --> Split["Split CPU bodies into cpu-*.c<br/>Keep function registry in functions.c<br/>Write sources.cmake and cache manifest"]
    Split --> NativeBuild["Compile generated C + selected app<br/>Game adapters + shared runtime and backends"]
    Reuse --> NativeBuild
    RT64["cmake/SharedRT64.cmake<br/>Reusable RT64 and dependencies<br/>build/shared/rt64/"] --> Link["Link native executable"]
    NativeBuild --> Link
    Link --> Run["Launch compiled game with verified content"]
```

The translator preserves register widths, sign extension, branch delay slots,
guest addresses and floating-point behavior. Guest memory helpers and
`MipsContext` carry those semantics into the generated C. The host compiler then
optimizes and compiles it; CPU instruction decoding belongs to generation.

Known SDK/library bodies bind to native handlers after complete-body fingerprint
checks. Explicit game hooks handle verified custom boundaries. Unsupported
boundaries may be represented by functions that fail when reached, so successful
generation does not establish complete game support.

The driver caches generation against the ROM and generator inputs. CMake tracks
the generated source list, and unchanged generated content keeps its timestamps
to avoid unnecessary recompilation. Game-specific preparation and configuration
stay in `games/<game>/generate.py`.

## Runtime, devices and worker completion

Startup details vary by game. The common structure allocates guest RDRAM and the
compiled function registry, attaches verified ROM content through
`recomp_game_rom()`, installs boot mappings and parameters, initializes services
and backends, and enters the compiled startup code.

```mermaid
flowchart TD
    Main["Main host thread<br/>SDL window, input and host callbacks"] --> Input["Read controller state<br/>Update native input service"]
    Input --> Scheduler["Guest scheduler<br/>Priorities and execution token"]
    Scheduler --> Guest["One guest CPU pthread runs at a time<br/>Own MipsContext and native call stack"]
    Guest --> Calls{"Next call or operation"}
    Calls -->|"Compiled function"| Registry["Fixed call or recomp_lookup<br/>Validate registered target and loaded section"]
    Registry --> Logic["Native game code<br/>Gameplay, physics, AI and list construction"]
    Logic --> Guest
    Calls -->|"Verified OS/library hook"| OS["Native services<br/>Threads, queues, timers, memory and devices"]
    OS --> Block{"Blocking operation<br/>cannot complete?"}
    Block -->|Yes| Park["Park guest pthread<br/>Preserve translated call stack"]
    Park --> Scheduler
    Block -->|No| Guest
    OS --> DMA["PI/EPI transfer<br/>Copy ROM or supported cartridge data<br/>Then publish completion"]
    Content["Verified ROM content<br/>Assets, tables and code-loading data"] --> DMA
    DMA --> RAM["Guest RDRAM<br/>Checked address access"]
    DMA --> Events
    OS --> IO["Controller and save services<br/>Guest structures, EEPROM, SRAM or Pak"]
    IO --> Files["Persistent files in local/"]
    IO --> Events
    OS --> SP["Submit supported graphics or audio task<br/>One active SP task<br/>Take private RAM snapshot"]
    RAM --> SP
    SP --> Kind{"Task type"}
    Kind -->|Graphics| Video["n64-video worker<br/>RT64 display-list processing and readbacks"]
    Kind -->|Audio| Audio["n64-audio worker<br/>Validate microcode and DSP commands<br/>Synthesize PCM in private RAM"]
    Video --> GPU["RT64 render and presentation queues<br/>Vulkan GPU → window"]
    Video --> Merge["Host commits changed task output bytes<br/>Retain unrelated guest writes<br/>Reject conflicting writes"]
    Audio --> Merge
    Merge --> Events["Post applicable device completion events<br/>SP/DP, PI, SI and AI messages"]
    Merge --> RAM
    RAM --> AI["Game queues PCM through native AI service<br/>Two-entry FIFO and guest-clock timing"]
    AI --> PCM["Main-thread PCM sink<br/>SDL buffering → speakers<br/>Optional WAV capture"]
    AI -->|"Guest buffer consumed"| Events
    Clock["Guest clock<br/>Timer deadlines and VI retraces"] --> Events
    Events --> Wake["Deliver registered messages<br/>Wake eligible blocked threads"]
    Wake --> Scheduler
    Guest --> Checkpoint["Generated checkpoint<br/>Safe preemption and host service boundary"]
    Checkpoint --> Main
    Main --> Present["Request presentation and pace frames<br/>Handle close or bounded-run limit"]
    Present -->|Continue| Input
    Present -->|Exit| Drain["Drain outstanding work<br/>Finalize enabled captures and statistics<br/>Destroy backends before runtime"]
```

Graphics and audio workers can overlap guest CPU execution. Both use private
snapshots; worker output reaches guest RAM before the associated completion is
published. Graphics task completion follows display-list processing, readbacks
and copying of render inputs; it does not wait for GPU presentation. Guest AI
completion follows the modeled guest clock rather than the host sound device.

Custom paging, overlay loading, relocation and graphics extensions belong to the
game adapter. Section load state determines which compiled guest addresses are
callable. The scheduler still allows only one guest CPU thread to execute at a
time, even while processing workers are active.

## Thread ownership and concurrency

Each guest libultra thread has a real pthread, its own native stack and a
`MipsContext`. A scheduler mutex and condition variables transfer the execution
token between the owner and a selected guest thread. The runtime implements
guest priorities and FIFO order among equally ready threads. Linux thread
priorities do not determine libultra scheduling.

```mermaid
flowchart LR
    subgraph Owner["Main host thread / scheduler owner"]
        SDL["SDL window and controller events<br/>PCM queue admission and frame pacing"]
        Host["Run scheduler and host callbacks<br/>Poll timers, devices and worker receipts"]
        Dispatch["Pick highest-priority ready guest<br/>Signal its condition variable"]
        Submit["Validate task and reserve SP<br/>Snapshot live RAM into before + working buffers"]
        Commit["Collect finished job<br/>Merge changed output and check conflicts<br/>Publish completion messages"]
        SDL --> Host
        Host --> Dispatch
        Host --> Submit
        Commit --> Host
    end
    subgraph Guests["Guest CPU pthreads — one execution token"]
        Game["Game / scheduler / controller threads<br/>Compiled guest functions"]
        Sound["Guest sound thread<br/>Compiled sequences and audio-list construction"]
        Park["Other guest threads parked<br/>Condition-variable wait preserves call stack"]
        Return["Block, stop or checkpoint yield<br/>Return execution token to owner"]
        Callback["recomp_on_host request<br/>Park until owner finishes callback"]
        Game --> Return
        Sound --> Return
        Game --> Callback
        Sound --> Callback
    end
    subgraph Workers["Persistent processing workers — private RAM"]
        Video["n64-video pthread<br/>RT64 display-list processing"]
        Audio["n64-audio pthread<br/>Native DSP synthesis"]
    end
    subgraph Renderer["RT64 internal threads and GPU"]
        Render["Workload/render queues<br/>Rendering and supporting workers"]
        Present["Presentation thread<br/>GPU scheduling and window output"]
        Render --> Present
    end
    Dispatch -->|"Select one"| Game
    Dispatch -->|"Select one"| Sound
    Return --> Host
    Callback -->|"Owner executes; guest then resumes"| Host
    Submit -->|"Graphics job"| Video
    Submit -->|"Audio job"| Audio
    Video -->|"Finished processing and readbacks"| Commit
    Audio -->|"Finished DSP output"| Commit
    Video -->|"Copied render inputs"| Render
    Park -. "Message, start or wake makes eligible" .-> Dispatch
```

The main thread waits while a guest owns the token. A guest calling
`recomp_on_host()` parks while the main thread executes the requested callback,
then resumes at the same call site. Window/input handling, task submission and
PCM queue admission therefore retain their required host-thread ownership.

At generated checkpoints, guest interrupts being disabled suppresses checkpoint
yields. Otherwise the scheduler can preempt for ready higher-priority work;
the default interval of 64 checkpoints also returns control to the owner for
device polling. Forced yields can return earlier. Blocking queue calls yield
when their condition cannot be satisfied, independently of the normal polling
interval.

### A blocking receive and wakeup

```mermaid
sequenceDiagram
    participant H as Main host / scheduler
    participant A as Guest pthread A
    participant B as Guest pthread B
    participant W as Task worker
    H->>A: Grant execution token
    A->>A: osRecvMesg with blocking flag; queue empty
    A->>H: Mark WAIT_RECV, release token and park
    Note over A: Native stack and MipsContext remain intact
    H->>B: Grant token to next ready guest
    par Guest CPU work
        B->>B: Execute compiled game code
        B->>H: Block or reach checkpoint yield
    and Previously submitted worker job
        W->>W: Process private RAM snapshot
        W-->>H: Job result ready for collection
    end
    H->>H: Poll receipts and merge task output
    H->>H: Deliver registered completion message
    H->>H: Make A ready; apply guest priority ordering
    H->>A: Grant token when A is selected
    A->>A: Original receive completes; continue same call stack
```

This example uses a worker completion to satisfy the receive. Timer, VI, PI, SI
and AI events or another guest's queue send can also wake a receiver. A full
queue can similarly park a blocking sender until space becomes available.

### What can overlap

| Execution context | Work and concurrency |
| --- | --- |
| Main host thread | Runs SDL and owner callbacks while the active guest is parked; collects results and updates shared device state. |
| Selected guest pthread | Executes compiled CPU code with the execution token; other guest pthreads wait. |
| Guest sound pthread | Builds audio lists as ordinary guest CPU work and shares the same execution token. |
| `n64-audio` | Executes native DSP jobs against private RAM; can overlap guest CPU execution. |
| `n64-video` | Processes RT64 display lists against private RAM; can overlap guest CPU execution. |
| RT64 internal threads / GPU | Consume copied render inputs and handle rendering/presentation; can continue after graphics SP completion. |
| SDL audio device thread | Consumes queued host PCM independently; does not drive the guest AI completion clock. |

Each processing worker accepts one job at a time. The shared SP service allows
one active RSP task, so separate video and audio pthreads do not imply two
simultaneous active guest SP tasks. Rendering and presentation can continue after
the graphics SP task releases the processor for an audio task.

Snapshots and result commits occur under owner control while guest CPU execution
is parked. For each byte, the merge compares the original snapshot, worker result
and current guest RAM. Unchanged worker bytes are ignored. A changed worker byte
is committed if live RAM still contains the original byte or already equals the
result; a third, conflicting value triggers a controlled failure. The host posts
completion only after that merge.

Shutdown drains and joins workers before releasing their state, destroys
backends before the runtime, and wakes/joins parked guest threads before freeing
their storage. Worker failures preserve their originating native stack and are
reported through the owner thread. See [native threading](threading.md) for the
scheduler configuration and [audio](audio.md) for AI timing and SDL buffering.

## Investigation and validation loop

```mermaid
flowchart TD
    Change["Change checked profile, metadata,<br/>generator, runtime or backend"] --> Build["Regenerate and build affected configuration"]
    Build --> Built{"Generation and build pass?"}
    Built -->|No| Static["Inspect generation.log, symbols.json,<br/>coverage.json and compiler/linker output"]
    Static --> Cause["Identify the owning layer and concrete cause"]
    Built -->|Yes| Replay["Run bounded scenario with known input<br/>Boot → title/menu → gameplay → transitions"]
    Replay --> Result{"Observed result"}
    Result -->|"Controlled runtime failure"| Crash["Shared crash JSON + RDRAM snapshot<br/>Thread/register/task state and native stack<br/>Recent calls and tracked PCs when available"]
    Crash --> Inspect["inspect_crash.py + address/function tools<br/>Match executable, generated code and ROM identity"]
    Inspect --> Cause
    Result -->|"Hang or host fault"| Debug["Inspect threads, queues and pending events<br/>Use GDB or host core dump for host faults"]
    Debug --> Cause
    Result -->|"Wrong output or behavior"| Compare["Compare input, RAM, tasks, frames and audio<br/>Trace producer, consumer and event ordering"]
    Compare --> Cause
    Cause --> Change
    Result -->|"Scenario passes"| Milestone["Record exactly what was exercised<br/>Gameplay, audio, input, saves and transitions"]
    Milestone --> More{"Required acceptance paths remain?"}
    More -->|Yes| Replay
    More -->|No| Handoff["Document verified milestone<br/>and remaining port limitations"]
```

Persistent fixes belong in checked inputs or maintained source. Generated C is
recreated by the build. Static coverage, successful compilation and a booting
window establish different milestones; gameplay and save/load behavior require
their own exercised scenarios.

For CPU investigation, capture a representative Release scene with
`tools/profile_execution.py`, attribute cost by thread, then inspect native
stacks and generated functions. See [CPU profiling](profiling.md).

## Implementation map

| Stage | Main implementation and reference |
| --- | --- |
| ROM normalization and identity | [`tools/import_rom.py`](../tools/import_rom.py), [`tools/common.py`](../tools/common.py) |
| Game mappings and generation hooks | `games/<game>/profile.toml`, `games/<game>/generate.py`, [adding a ROM](adding-a-rom.md) |
| Generation orchestration | [`tools/generate_game.py`](../tools/generate_game.py), [`cmake/Game.cmake`](../cmake/Game.cmake) |
| Discovery and translation | [`tools/discover.py`](../tools/discover.py), [`tools/translate.py`](../tools/translate.py), [`tools/emit.py`](../tools/emit.py), [recompilation](recompilation.md) |
| Native hook matching | [`tools/symbols.json`](../tools/symbols.json), [native hooks](native-hooks.md) |
| Diagnostic SDK symbols and handler coverage | [`tools/inspect_sdk_symbols.py`](../tools/inspect_sdk_symbols.py), [SDK report interpretation](native-hooks.md#identifying-rom-routines-and-reviewing-handler-gaps) |
| Optional embedded content | [`tools/package_rom.py`](../tools/package_rom.py), [ROM content packaging](rom-packaging.md) |
| Optional executable compression | [`cmake/PackUPX.cmake`](../cmake/PackUPX.cmake) |
| Runtime entry and boot | `games/<game>/debug/app.c` or `release/app.c`, [`runtime/game.c`](../runtime/game.c), [`runtime/bootstrap.c`](../runtime/bootstrap.c) |
| Dispatch, guest memory and FPU | [`runtime/dispatch.c`](../runtime/dispatch.c), [`runtime/memory.c`](../runtime/memory.c), [`runtime/fpu.c`](../runtime/fpu.c) |
| Guest scheduling and OS | [`runtime/scheduler.c`](../runtime/scheduler.c), [`runtime/os.c`](../runtime/os.c), [threading](threading.md), [OS emulation](os-emulation.md) |
| Time, devices and task receipts | [`runtime/time.c`](../runtime/time.c), [`runtime/platform.c`](../runtime/platform.c), [`runtime/tasks.c`](../runtime/tasks.c), [`runtime/video.c`](../runtime/video.c) |
| Input and saves | [`runtime/io.c`](../runtime/io.c) |
| Graphics and textures | [`backends/rt64.cpp`](../backends/rt64.cpp), [`backends/textures.cpp`](../backends/textures.cpp), [textures](textures.md) |
| Audio synthesis and output | [`backends/audio.c`](../backends/audio.c), [`third_party/rsp-hle/README.md`](../third_party/rsp-hle/README.md), [audio](audio.md) |
| Shared renderer build | [`cmake/SharedRT64.cmake`](../cmake/SharedRT64.cmake) |
| Crash investigation | [`tools/inspect_crash.py`](../tools/inspect_crash.py), [debugging](debugging.md) |
