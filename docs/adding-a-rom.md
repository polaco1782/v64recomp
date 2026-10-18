# Adding a ROM and debugging a new port

Importing a ROM for an existing game and adding support for a different game are
different amounts of work. For an already supported revision, follow the ROM
import commands in the [README](../README.md#build-and-play). A new game needs
verified CPU mappings, discovery metadata, native services, a runtime application,
and graphics/audio support. Changing the filename or SHA-1 alone does not port it.

This guide follows the current repository implementation. Run shell commands from
the repository root. `newgame-us` is a placeholder for your new game ID; replace
it consistently. Addresses in the Duke examples apply only to that pinned ROM.

## 1. Prepare the tools

Install the build dependencies listed in the README: Python 3.11+, a C/C++
compiler, CMake, Ninja, OpenSSL 3, SDL2, X11/Xwayland and Vulkan. Initialize the
pinned renderer and Python environment:

```sh
git submodule update --init --recursive
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
```

These are the investigation tools and when to use them:

| Tool | Purpose |
| --- | --- |
| GDB | Stop at runtime failures, inspect host and guest state, catch native faults, interrupt hangs, set watchpoints. |
| GNU binutils: `nm`, `addr2line`, `objdump` | Find missing host symbols and resolve native addresses against the matching executable. |
| `tools/inspect_rom.py` | Identify a ROM before a profile exists; show normalized hashes, header, bytes, disassembly and range hashes. |
| `tools/import_rom.py` | Normalize and import a ROM after its profile pins size and SHA-1. |
| `tools/discover.py` | Follow CPU control flow using explicit load mappings; write `symbols.json`. |
| `tools/translate.py` | Audit instructions and targets; write `coverage.json`, optionally emit C. |
| `tools/generate_game.py` | Run registered game preparation, discovery, translation and CMake source splitting. |
| `tools/inspect_address.py` | Resolve a guest address to section, byte offset, entry/instruction ownership, gaps, errors and known references. |
| `tools/disassemble_function.py` | Resolve a generated name such as `recomp_s3_7f068008` through `symbols.json` and disassemble its verified MIPS instructions. |
| `tools/export_analysis_rom.py` | Export and verify the same prepared CPU image used by discovery, including decompressed code. |
| `tools/inspect_crash.py` | Summarize a controlled crash and resolve native frames with `addr2line`. |
| `tools/controller_replay.py` | Convert existing ROM-bound replay JSON to the debug app's text input format. |
| Optional MIPS disassembler/decompiler, such as Ghidra | Investigate loaders, BSS bounds, callbacks and data structures. Configure big-endian VR4300/MIPS III semantics and the verified address mapping. |

System tools such as GDB and binutils must be installed on the host. The shared
project scripts live in `tools/`; ROM-specific preparation and adapters belong
in `games/<game>/`. Rabbitizer, pinned in `requirements.txt`, supplies the scripts'
MIPS decoding. A matching original ELF/map helps name functions but is not required.

## 2. Identify the exact revision before importing it

```sh
.venv/bin/python tools/inspect_rom.py '/path/to/New Game.v64'
```

Record the normalized SHA-1, ROM size, header entry, country and revision bytes in
your port notes. SHA-1 here identifies the complete normalized ROM; the title and
revision byte alone cannot establish that the bytes match. The inspector accepts
`.z64`, `.v64` and `.n64` byte orders based on magic, independently of filename.
It reads the input without modifying it. Profile hashes refer to big-endian bytes,
so hashing the original byte-swapped file with `sha1sum` gives a different result.

Create `games/newgame-us/README.md` with the identity, initial evidence and current
milestone. Keep ROMs and extracted images in ignored `local/` or `build/`.

## 3. Recover the ROM-to-RAM mappings and boot state

Start with the entry address in the ROM header, then inspect IPL/startup code to
establish which ROM bytes it loads there. Determine each CPU code region's source
offset, guest address and byte size. Also identify BSS clear bounds, startup
parameters, RAM size, thread objects/stacks and scheduler globals. Do not infer a
code region's end from the end of the ROM: ROMs contain data and RSP microcode.

For an uncompressed mapped range:

```text
ROM offset = region.rom_offset + (guest_address - region.vram)
```

For example, Duke's profile maps ROM `0x1000` to guest `0x8004b400`. Therefore
guest `0x8004b450` corresponds to ROM `0x1050`. Inspect it with:

```sh
.venv/bin/python tools/inspect_rom.py local/duke.us.z64 \
  --offset 0x1050 --vram 0x8004b450 --size 0x40
```

The disassembler uses your supplied mapping; readable assembly is evidence to
investigate, not proof that a region is executable code. Check branches, delay
slots, callers and loaders together.

For compressed/paged code, implement deterministic preparation in
`games/newgame-us/generate.py`, exporting:

```python
def prepare(rom: bytes, profile: dict, output: Path) -> tuple[bytes, dict]:
    # Verify compressed inputs and decoded sizes/hashes.
    # Return an analysis image containing decoded code, and updated mappings.
    ...
```

Examples are OOT's [Yaz0/overlay preparation](../games/oot-us-v12/prepare.py)
and Perfect Dark's [RareZip preparation](../games/perfectdark-us-v11/generate.py).
Preparation may append decoded sections to the analysis image. Their reported
offsets then refer to that image, not to compressed cartridge offsets. Keep the
original normalized cartridge as the runtime ROM and reproduce the game's actual
loading/paging/overlay behavior in its adapter.

Discovery rejects overlapping load mappings. OOT assigns overlays separate
analysis addresses and handles runtime relocation in its adapter; simply listing
multiple overlays at the same address is insufficient. An actual CPU TLB address
also needs a verified runtime mapping. Masking an arbitrary virtual address into
RDRAM does not implement paging.

## 4. Write the pinned profile and import the ROM

Create `games/newgame-us/profile.toml`. The following is a concrete **Duke-only
example of the minimum static-discovery fields**, not values to reuse in a new
game. Replace all identity and mapping values with your measured results:

```toml
[game]
name = "Duke Nukem 64 (US)"
rom = "../../local/duke.us.z64"
rom_sha1 = "98d6778004becc672eba0a5e887f6e3f3d1b5c15"
rom_size = 0x800000
ram_size = 0x800000
entry_symbol = "entry_point"
code_sections = [".main"]
boot_sections = [".main"]

[discovery]
regions = [{name = ".main", vram = 0x8004b400, rom_offset = 0x1000, size = 0xbf880}]
```

`discovery.entry_vram` can override the header entry for a verified analysis
layout. `game.fpu_fr` defaults to 0; use 1 only when the game uses independent
64-bit FPRs, and set `RecompSchedulerConfig.fpu_fr1` consistently in both apps.
The runtime `[bootstrap]` fields are consumed by your game's config generator;
they are not a universal loader that automatically boots every profile.

Add checked roots, transfers, tables, hooks, boundaries and checkpoints directly
to this same `profile.toml` as the investigation advances. Every game uses one
complete profile for ROM import, source generation and diagnostic tools.

Once the new profile has the correct `game.rom`, size and hash:

```sh
.venv/bin/python tools/import_rom.py \
  --profile games/newgame-us/profile.toml --rom '/path/to/New Game.v64'
```

Import verifies identity before writing and refuses to overwrite different bytes.

## 5. Discover a small startup path and audit it first

For a flat image whose mappings are already present in the profile:

```sh
.venv/bin/python tools/discover.py \
  --profile games/newgame-us/profile.toml \
  --output build/newgame-us-report/symbols.json
.venv/bin/python tools/translate.py \
  --profile games/newgame-us/profile.toml \
  --metadata build/newgame-us-report/symbols.json \
  --output build/newgame-us-report --report-only
```

The standalone CLIs do not run a game's `prepare()` or `finalize_metadata()`.
For compressed code or game-specific finalization, register the generator as
described in step 7, then use:

```sh
.venv/bin/python tools/generate_game.py --game newgame-us \
  --output build/newgame-us-report --report-only
```

Read these artifacts before trying to compile:

| Artifact | Evidence to inspect |
| --- | --- |
| `symbols.json` | `sections`, function names/aliases, `instruction_addresses`, `discovery_errors`, `unvisited_ranges`, discovery origins and issues. |
| `coverage.json` | `summary`, each function's `status` and `errors`, metadata issues and code inventory completeness. |
| `pending-boundaries.json`, if the adapter emits it | Explicit unimplemented function bodies that will fail if reached. |

The normal loop is: inspect the first unsupported site, correct the mapping or
metadata/service, regenerate, and check that the specific error disappeared.
An inventory with many `ready` functions may still omit callbacks and overlays.
Unvisited bytes may be data or code; do not mark all gaps as code or claim a
complete port from a report-only run.

## 6. Resolve native services, callbacks and computed jumps

Ordinary OS/library routines should use the shared
[native hook catalog](native-hooks.md). It locates candidates using an instruction
prefix and requires the SHA-256 of the **complete routine body**. Do not copy
another ROM's addresses or accept a matching prologue as identification.

For an unrecognized routine: inspect its entire body and callers, establish the
ABI and return behavior, check whether the shared runtime already implements it,
then add the verified compiler/SDK variant to `tools/native_hooks.toml`. A catalog
record includes `symbol`, `handler`, `body_size`, `sha256` and `expected_words`.
Use the range hash from `inspect_rom.py --offset ... --size <full-body-size>`;
the size must come from analysis. A 16-byte callback fingerprint is insufficient
for a native replacement. Implement missing shared services in `runtime/` with
declarations in `include/`. Reserve game TOML hooks for game-specific behavior.

Callbacks often enter through pointers in data or through `jalr`, so direct-call
discovery can miss them. Investigate pointer source, loader and surrounding code
before adding a `[discovery]` root. Partial checked entry inventories use:

```json
{
  "format_version": 1,
  "rom_sha1": "<SHA-1 of the actual discovery image>",
  "complete": false,
  "entries": [
    {"vram": 2147487744, "origin": "checked-root", "sha256": "<SHA-256 of first 16 bytes>"}
  ]
}
```

That address is illustrative. Set `discovery.entry_inventory` to the inventory's
path relative to the profile. Record why each entry is a function. The helper
functions in `tools/metadata_candidates.py` propose pointer/materialized-address
candidates; they are not a standalone CLI or proof of code. Full classifications
have a different schema validated by `tools/code_inventory.py`.

For `unresolved indirect transfer`, inspect the exact `jr`/`jalr` and preceding
instructions, including the delay slot. Determine whether it is a switch, callback,
tail call or saved return. A checked switch table needs its instruction `pc`,
`expected_word`, `table_vram`, `count` and SHA-256 of all table bytes. Derive the
count from the bounds check, not from how many following words resemble pointers.
`jump_tables` describes local computed jumps; `function_tables` describes indirect
calls. Explicit `transfers` handles proven fixed target sets and return patterns.
The existing [Duke startup metadata](../games/duke-us/profile.toml) provides examples.

`allow_registered_indirect_calls` permits runtime lookup only with a checked
code/entry inventory. It does not discover missing entries or make arbitrary
targets safe. Keep loaded-section and `prepare_call` checks on direct calls too.
An `unsupported_function` boundary can make an explicitly partial build possible;
reaching it remains a porting failure to investigate.

## 7. Add the native application and CMake integration

Use this required layout for every game, retaining all game-specific state under
its directory. [AGENTS.md](../AGENTS.md) defines the repository-wide rules for
models adding or extending ports:

```text
games/newgame-us/
  README.md
  profile.toml
  generate.py
  audio.h
  ...                       # shared headers, loaders, hooks and adapters
  startup-entries.json       # when checked callback roots are available
  CMakeLists.txt
  release/
    app.c
    ...                     # additional Release-only sources
  debug/
    app.c
    ...                     # replay inputs and diagnostic-only code
```

Keep one complete `profile.toml`; do not add game profile inheritance chains or
alternative profile filenames. Keep all headers and shared adapters in the parent
directory. Each configuration has its own `app.c` and may use the parent folder;
it must not include or compile files from the other configuration's folder.

Read an existing app for structure, then replace its ROM-specific assumptions.
[Duke's release app](../games/duke-us/release/app.c) is a compact example, but its
thread addresses, timing calibration and audio profile are Duke-specific.

1. Export `write_config(profile, metadata, output, startup)` from `generate.py`.
   It writes the header consumed by your apps: pinned identity, boot addresses,
   runtime globals and any adapter descriptors. Add `prepare`,
   `finalize_metadata` or `configure_translation` only when needed, with checks.
   The shared driver calls preparation before discovery, finalization before
   saving metadata, and translation configuration only for actual generation.
   An optional `write_report(rom, profile, metadata, report, output)` writes
   game-specific analysis after translation in both generation and report mode.
   Set `REQUIRE_COMPLETE_INVENTORY = True` only for a complete CPU inventory;
   this retains the translator's discovery-issue and unresolved-boundary checks.
2. Initialize `Runtime` with the generated registry and boot-section states,
   verify/attach the original ROM through `recomp_game_rom`, load the verified
   initial CPU bytes, and supply the measured boot parameters. Reproduce BSS
   initialization without erasing thread objects or active runtime state.
   The ROM helper installs the shared crash reporter. Use `recomp_fail(message,
   address)` or `recomp_game_require(condition, message, address)` for all fatal
   errors in either mode; keep crash serialization and diagnostic capture in
   the core rather than adding a game-specific handler.
3. Configure clock, scheduler, platform, IO and task services using the measured
   guest globals. Choose synthetic boot-thread storage outside guest-owned data,
   stacks and framebuffers, and retire it before the game reuses that RAM.
4. Create the renderer and audio backend. Identify actual task types and microcode
   from submitted tasks. Supply checked audio profiles and any required graphics
   extensions; an unknown RSP task is not fixed by declaring it standard graphics
   or skipping it. Link `recomp_jpeg` if the port requires the supported JPEG path.
5. Run the scheduler, input, presentation and deadlines from the app loop. Keep SDL
   input/window operations on their owner thread. Join workers before teardown.
6. Add a debug app with startup assertions and useful reports/captures. Check its
   `--help`; per-game diagnostic argument parsing is not supplied just by creating
   a file under `debug/`. Use the same shared crash handler as Release; core
   dispatch retains the last 128 guest-call addresses in Debug.

The minimal game `CMakeLists.txt` is:

```cmake
set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/newgame.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/newgame-us.eeprom")
# Set RECOMP_GAME_SOURCES here if the adapter needs additional translation units.
# Use parent-folder paths for shared sources, e.g. set(RECOMP_GAME_SOURCES pager.c).
# Optional RECOMP_GAME_DEBUG_SOURCES / RECOMP_GAME_RELEASE_SOURCES select extra
# configuration-only sources using debug/ or release/ paths. app.c is automatic.
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
```

Use the actual save medium/path; add required target libraries after the include.
Register `newgame-us` in **both** root `CMakeLists.txt`'s `RECOMP_GAMES` and
`tools/generate_game.py`'s `GAMES`. The driver uses `profile.toml` and `generate.py`
in every game's parent directory. Shared headers belong there too; CMake includes
that directory and the selected configuration directory in both builds.
Explicit CMake configurations then work without presets:

```sh
cmake -S . -B build/newgame-us-debug -G Ninja \
  -DRECOMP_GAME=newgame-us -DCMAKE_BUILD_TYPE=Debug
cmake --build build/newgame-us-debug -j 4
```

Add Release/Debug configure and build presets to `CMakePresets.json` for the usual
short commands, and optional VS Code entries in `.vscode/tasks.json` and
`.vscode/launch.json`. Update the root import/game lists and `docs/status.md` with
the actual milestone. `cmake/Game.cmake` handles generated-source caching/splitting,
mode-specific app selection, debug flags, and `run`/`report` targets. Configuration
errors from generation are recorded in `build/newgame-us-debug/generation.log`.

## 8. Track a missing symbol or guest function

First classify the failure:

| Message or symptom | Where to investigate |
| --- | --- |
| Compiler says an identifier is undeclared | Missing C declaration/header, or wrong generated config. |
| Linker says `undefined reference to hook_...` | Native handler absent, wrong linkage, or source/library omitted from CMake. |
| Generator says symbol/root missing or ambiguous | Names/aliases, mappings, checked roots and profile inheritance. |
| Runtime says `unregistered or unloaded function` | Guest target lacks an entry, or its section/overlay is not active. |
| Runtime says `ambiguous loaded function` | More than one active registry entry for the canonical guest address. |

For a missing host symbol, search declaration, implementation and build wiring:

```sh
rg -n 'hook_example' include runtime backends games tools
cmake --build build/newgame-us-debug --verbose
# Replace this archive with the library expected to supply the symbol:
nm -A --defined-only build/newgame-us-debug/librecomp_runtime.a | rg 'hook_example'
```

If the declaration and implementation exist, verify the defining `.c` is compiled
and linked, and that a C++ implementation exports the required C linkage. A TOML
handler name does not create a C implementation. For a missing generated
`recomp_sN_ADDRESS`, inspect that build's registry in generated `functions.c` and
`generated.h`, selected functions, and `generation.log`; regenerate through CMake.

For a missing **guest** function, take the reported target and the caller's guest
PC/RA from GDB or the crash report. Inspect the matching metadata, for example:

```sh
.venv/bin/python tools/inspect_address.py \
  --metadata build/duke-us-debug/generated/duke-us/symbols.json \
  --address 0x8004b450 --rom local/duke.us.z64
```

Use your port's paths and address. The inspector reports section ID/name, byte
offset, aliases, exact instruction ownership, unvisited ranges and nearby errors.
With `--rom`, it verifies the image, scans discovered J/JAL sites and disassembles
the requested window. Checked indirect targets are reported even without ROM
bytes. It does not recover dynamic callers or prove which section is active.

For a compressed/paged port, export the prepared image first:

```sh
.venv/bin/python tools/export_analysis_rom.py --game newgame-us \
  --metadata build/newgame-us-debug/generated/newgame-us/symbols.json \
  --output build/newgame-us-debug/analysis-rom.bin
.venv/bin/python tools/inspect_address.py \
  --metadata build/newgame-us-debug/generated/newgame-us/symbols.json \
  --rom build/newgame-us-debug/analysis-rom.bin --address 0x80001000
```

Replace that illustrative address with the actual compiled guest address. The
exporter reruns the registered `prepare()` in a temporary directory and validates
the result against metadata; it refuses mismatched metadata and different existing
output bytes. It does not alter the runtime ROM. Coverage beside `symbols.json`
is loaded automatically; `--coverage` selects another matching report. Keep these
artifacts from one generation: equal ROM hashes alone cannot establish equal
profile or generator versions.

Now follow the evidence:

1. **Address unmapped:** check the DMA/decompressor, virtual address mapping and
   overlay relocation. For relocated code, convert the live address using the
   adapter's relocation records to the compiled analysis address first.
2. **Mapped but not an entry:** inspect the calling `jalr` and pointer producer.
   Decide whether this is a real callback, an internal label, a corrupted pointer
   or a bad return. Add a checked entry only after proving an actual function ABI.
3. **Entry exists:** search generated `functions.c` for it, check selection and the
   native handler, then inspect `rt->section_loaded[section_id]` at the failure.
   Confirm the loader activates it only after copying/relocating the correct code.
4. **Multiple active entries:** fix overlay ownership/activation and any canonical
   address mapping. `recomp_lookup` resolves canonical addresses first and rejects
   ambiguity; preserving that check exposes incorrect loading.
5. Regenerate and rerun the same trigger. Verify execution continues beyond the
   previous missing target. Returning silently from an unknown address hides it.

## 9. Capture and investigate a crash with GDB

Build Debug to track guest PCs at each translated instruction, including executed
delay slots. Release compiles these PC updates away, so its saved PC may only be
the initial thread entry. The failure's explicit address and host frames remain
useful in Release. A host instruction pointer such as `$pc` is an x86/host address;
`ctx->pc` is a guest MIPS address.

Reproduce with a bounded run, retaining logs and a separate crash directory:

```sh
mkdir -p local/diagnostics/newgame-us
./build/newgame-us-debug/bin/newgame-us --frames 300 \
  --crash-dir local/diagnostics/newgame-us \
  2>local/diagnostics/newgame-us/debug.log
```

`--frames` counts presentations in most apps; it is not a timeout or instruction
budget, so a guest hang may prevent the bound from being reached. Use a desktop
session for rendered tests. An SDL/X11/Vulkan initialization failure occurs before
game acceptance and does not establish a guest crash. Add `--trace-calls` for a
focused reproduction; `RECOMP_DEBUG_FULL=1` expands sampled diagnostics and can
produce substantial output. See [debugging options](debugging.md) for availability.

For a controlled failure routed through `recomp_fail`:

```sh
.venv/bin/python tools/inspect_crash.py /path/to/crash-report.json
```

Omitting the path selects the latest report under `local/crashes/` or `build/`,
which could belong to another game. Pass custom-directory reports explicitly.
Keep the exact executable, generated sources, metadata, log, JSON and `.rdram.bin`
together before rebuilding. `inspect_crash.py` uses recorded module paths and
`addr2line`; moved/replaced binaries cannot reliably resolve the old stack.
Its legacy "last tracked call/FP location" label refers to the saved guest PC;
the tracking precision depends on the build. Debug reports include `recent_calls`
from shared dispatch; Release leaves that history empty.

For a live diagnosis:

```sh
gdb --args ./build/newgame-us-debug/bin/newgame-us --frames 300
```

```gdb
set pagination off
break recomp_fail
run
bt full
thread apply all bt
info args
```

At `recomp_fail`, inspect its `message` and `address` arguments. Use `up` or
`frame N` to select the generated caller/native hook that has `ctx` and `runtime`
or `rt` in scope, then:

```gdb
p/x ctx->pc
p/x ctx->gpr[31]
p/x ctx->gpr[29]
p/x ctx->gpr[4]@4
list
```

`gpr[31]` is guest RA, `gpr[29]` SP, and `gpr[4..7]` the first four o32 arguments.
For a normal JAL/JALR caller, RA often points eight bytes past the call because
of the delay slot. Saved-RA/shared epilogues and nonlocal returns need their actual
ABI, not a blanket `RA - 8` rule. Use the address inspector on both failing target
and call site. To stop on an ordinary generated entry, take the exact C name it
reports, for example `break recomp_s0_8004b450` for the Duke example. A native hook
uses its handler's C name instead. A breakpoint at guest address `*0x8004b450`
would address host memory and is not the way to break on translated code.

For SIGSEGV/SIGABRT outside controlled checks, GDB stops at the signal. Capture
`bt full`, `thread apply all bt`, registers and the first meaningful project frame.
Optional host cores can be enabled with `ulimit -c unlimited`; retrieve the core
according to the host's `kernel.core_pattern`/systemd configuration, then load it
with `gdb /path/to/exact/executable /path/to/core`. A sandbox that denies `ptrace`
cannot run live GDB: reproduce from a permitted desktop terminal. Crash-report
inspection itself does not require ptrace.

## 10. Find the cause of corrupted memory or a hang

Follow the bad value to its writer, rather than changing the final check:

1. Read the failure message and classify its address: guest pointer, instruction,
   physical byte, task/microcode address or host fault. Not every failure address
   is an instruction PC.
2. Select the responsible thread/frame. Inspect registers and the load/store or
   call that consumed the value. Check width, alignment and pointer provenance.
3. Compare the affected memory against BSS, heap, stack, thread objects, task
   output and framebuffer ranges. For direct KSEG0/KSEG1 aliases only, physical
   address is `guest & 0x1fffffff`; custom TLB/paged addresses need the adapter.
4. Watch the known RAM location during the next reproduction to find the writer.
   For an aligned word in direct RDRAM, after selecting a frame with `Runtime *rt`:

   ```gdb
   set $guest_word = 0x803ff000
   set $physical = $guest_word & 0x1fffffff
   p/x rt->rdram_size
   watch -l *(uint32_t *)(rt->rdram + $physical)
   continue
   ```

   This is an illustrative address; use the corrupt location and check it is
   within RDRAM. Recreate watchpoints after relaunch because the host buffer can
   move. Guest RAM is word-swapped: byte address `p` is stored at `rdram[p ^ 3]`,
   a 16-bit access at `p ^ 2`, and an aligned 32-bit word at `p`. Arbitrary byte
   dumps are not big-endian ROM bytes. Do not use a direct-RAM watchpoint formula
   for custom virtual mappings or a worker's separate RAM snapshot.
5. Fix the lifetime, mapping, ABI or instruction semantics that produced the bad
   state. Rerun the original trigger and continue beyond it; check the evidence
   that would have detected the old failure.

A concrete example is OOT's `guest wrote an in-flight task output` at physical
`0x003ff003`. It fell within the synthetic IPL object at guest `0x803ff000`, which
overlapped memory reused for a framebuffer. `recomp_worker_merge` correctly detected
conflicting writes. A stopped guest thread still had scheduler-synchronized state;
stopping it did not release the RAM. The fix retires it from `oot_prepare_call`
at `0x80005f20`, before graphics submission, with a debug guard that it is dead.
See [the hook](../games/oot-us-v12/hooks.h) and
[debug task checks](../games/oot-us-v12/debug/app.c). This illustrates the chain:
failure byte → guest alias → object ownership → writer/lifetime → earlier retirement.

For a hang, interrupt GDB with Ctrl-C and inspect **all** threads. If a guest is
spinning, inspect its PC/RA and the condition it awaits. If guest threads are
blocked on queues, trace the expected sender, VI/timer deadline, SI/PI completion
or SP/DP event through `runtime/`. Check whether backend work finished and whether
the owner published its event. `recomp_scheduler_run`'s resume budget is not an
instruction limit: code without checkpoints/hooks may never yield. Avoid calling
scheduler/runtime functions from GDB merely to inspect them; they can mutate state
or deadlock. Read structures and existing snapshots/logs instead.

For suspected host memory errors, a separate sanitizer build can help. Apply
compile **and** link sanitizer flags consistently to the app and implicated shared
libraries; instrumenting only a generated file misses runtime/backend writes.
Keep that evidence separate from normal-build timing and behavior.

## 11. Validate the port and record its boundary

First get startup and rendered gameplay working; then broaden regression coverage.
Use separate Debug and Release build directories:

```sh
cmake -S . -B build/newgame-us-release -G Ninja \
  -DRECOMP_GAME=newgame-us -DCMAKE_BUILD_TYPE=Release
cmake --build build/newgame-us-release -j 4
./build/newgame-us-release/bin/newgame-us --frames 600
```

Check progressively: boot/BSS and original thread creation; first task and menu;
interactive gameplay; graphics correctness, audio, input and saves; scene/level
transitions and overlay reloads; repeated launches and longer play. Run the same
failure trigger in Debug and Release after a fix. A muted run can isolate playback
issues but cannot validate audio. A successful build, report, headless startup or
nonzero frame counter alone does not prove rendered gameplay or a complete port.

After changing discovery/emission/runtime behavior, run the focused checks for
that change and any affected game's reproduction. The portable diagnostic-tool
checks added with this guide run without ROM assets:

```sh
.venv/bin/python -m unittest discover -s tools/tests -p test_porting_tools.py -v
git diff --check
```

Update the game's README and `docs/status.md` with what actually ran, remaining
unsupported functions/tasks, unresolved transfers and progression gaps. For a
partial handoff, add `games/newgame-us/RESUME.md` with ROM identity, commands,
artifact paths, last validated milestone, exact next blocker and reproduction.
Keep a short evidence trail for each crash fix: trigger → report/PC → mapping →
root cause → change → rerun beyond the previous failure.
