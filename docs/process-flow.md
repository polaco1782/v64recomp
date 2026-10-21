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
    Exit -->|Yes| Cleanup["Drain RT64 queues and finalize captures<br/>Destroy backends, then runtime<br/>Free ROM memory"]
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

## Runtime, devices and task completion

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
    OS --> SP["Submit supported graphics or audio task<br/>One active SP task<br/>Park guest for owner processing"]
    RAM --> SP
    SP --> Kind{"Task type"}
    Kind -->|Graphics| Video["Owner: RT64 display-list processing<br/>Readbacks use live RAM"]
    Kind -->|Audio| Audio["Owner: validate microcode and DSP commands<br/>Synthesize PCM in live RAM"]
    Video --> GPU["RT64 render and presentation queues<br/>Vulkan GPU → window"]
    Video --> Merge["HLE returns with output in live RAM<br/>Queue task completion receipts"]
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

Graphics HLE parsing, readbacks and audio DSP run synchronously on the owner
while guest CPU execution is parked. They use live RAM without full-memory
snapshots or merges. Graphics completion follows display-list processing,
readbacks and capturing render inputs; RT64 rendering and presentation remain
asynchronous. Guest AI completion follows the guest clock rather than the host
sound device. See [native threading](threading.md) for the RAM ownership contract.

Custom paging, overlay loading, relocation and graphics extensions belong to the
game adapter. Section load state determines which compiled guest addresses are
callable. The scheduler allows only one guest CPU thread to execute at a time.

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
        Host["Run scheduler and host callbacks<br/>Poll timers, devices and task receipts"]
        Dispatch["Pick highest-priority ready guest<br/>Signal its condition variable"]
        Submit["Validate task and reserve SP<br/>Execute HLE synchronously using live RAM"]
        Commit["Poll queued SP/DP receipts<br/>Publish completion messages"]
        SDL --> Host
        Host --> Dispatch
        Host --> Submit
        Submit --> Commit
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
    subgraph Renderer["RT64 internal threads and GPU"]
        Render["Workload/render queues<br/>Rendering and supporting workers"]
        Present["Presentation thread<br/>GPU scheduling and window output"]
        Render --> Present
    end
    Dispatch -->|"Select one"| Game
    Dispatch -->|"Select one"| Sound
    Return --> Host
    Callback -->|"Owner executes; guest then resumes"| Host
    Submit -->|"Captured graphics inputs"| Render
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
    H->>A: Grant execution token
    A->>A: osRecvMesg with blocking flag; queue empty
    A->>H: Mark WAIT_RECV, release token and park
    Note over A: Native stack and MipsContext remain intact
    H->>B: Grant token to next ready guest
    B->>H: Submit task through recomp_on_host; park B
    H->>H: Process HLE using live RAM; queue completion
    H-->>B: Owner callback returns; resume B
    B->>H: Block or reach checkpoint yield
    H->>H: Poll queued task receipts
    H->>H: Deliver registered completion message
    H->>H: Make A ready; apply guest priority ordering
    H->>A: Grant token when A is selected
    A->>A: Original receive completes; continue same call stack
```

This example uses a queued HLE completion to satisfy the receive. Timer, VI, PI,
SI and AI events or another guest's queue send can also wake a receiver. A full
queue can similarly park a blocking sender until space becomes available.

### What can overlap

| Execution context | Work and concurrency |
| --- | --- |
| Main host thread | Runs SDL, scheduling, graphics HLE and audio DSP while the guest is parked; waits while a guest owns the token. |
| One guest pthread | Executes translated game code; other guest CPU pthreads remain parked. |
| RT64 internal threads and GPU | Render and present captured workload data independently; can overlap guest CPU execution without accessing live guest RAM. |
| SDL audio device thread | Consumes queued host PCM independently; does not drive guest AI completion timing. |

The SP service allows one active task. HLE writes live RAM before queuing
completion receipts; polling delivers SP before DP. Split lists without full
sync retire after SP. A whole-list HLE call cannot be suspended midway through
processing; a late yield polls its queued completion first.

Shutdown drains RT64 queues, destroys backends before the runtime and joins guest
threads before releasing their storage. HLE failures occur directly on the owner
and use the shared crash reporter. See [native threading](threading.md) and
[audio](audio.md) for memory ownership and buffering.

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
