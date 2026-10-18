---
name: add-game
description: Add or finish an N64 game port in v64recomp, from ROM identification and checked symbol discovery through native execution, rendered gameplay, and cleanup of temporary tests and unused files. Use when bringing a new ROM revision into this repository or resolving an incomplete port's startup and runtime blockers.
---

# Add a game to v64recomp

Work from the repository root. Carry the port through boot, menus, and playable
rendered gameplay with working input and audio. A successful build or static
report is an intermediate milestone. Prioritize getting the game running before
broad regression work, then finish focused validation, documentation, and cleanup.
Continue investigating failures until the requested milestone works or a concrete
external blocker prevents further progress; record the exact blocker and the
next actionable step when that happens.

## Read the current implementation

Inspect `git status --short` and applicable `AGENTS.md` instructions first;
preserve unrelated changes. Read [README.md](README.md),
[recompilation](docs/recompilation.md), [native hooks](docs/native-hooks.md),
and [debugging](docs/debugging.md). Consult [OS emulation](docs/os-emulation.md),
[threading](docs/threading.md), and [audio](docs/audio.md) for relevant failures.

Choose the closest existing port by loading model and runtime requirements:

- `games/quake-us/`: a relatively simple initial ROM-to-RAM mapping.
- `games/oot-us-v12/`: compressed images, DMA, overlays, relocations, and callback analysis.
- `games/perfectdark-us-v11/`: compressed Rare images and custom virtual-address mapping.
- `games/goldeneye-us/` and `games/shadowman-us/`: custom paging/TLB behavior.

Use these as implementation references. Derive addresses, fingerprints, RAM size,
microcode, scheduler globals, save type, and timing from the new ROM. Existing
ports have different completeness levels and are not universal templates.

## 1. Identify the ROM and establish the load map

1. Locate the user-provided ROM. Identify its byte order, normalize it, and compute
   the normalized size and SHA-1. Inspect the header for title, region, revision,
   and entry address. Choose a stable ID such as `title-us` or `title-us-v12`.
   Do not fetch or commit ROMs or extracted copyrighted assets.
2. Create `games/<game>/profile.toml` pinned to those exact bytes. Once the profile
   exists, use `tools/import_rom.py --profile games/<game>/profile.toml --rom PATH`
   to verify and place the normalized ROM in ignored `local/`.
3. Disassemble the entry path. Trace initial copies, decompression, BSS clearing,
   jumps to system/main entry, and initial thread creation. Determine ROM offsets,
   guest virtual addresses, load sizes, code/data boundaries, and 4/8 MiB RAM use.
   A region may contain data and RSP microcode as well as CPU instructions.
4. Describe discovery regions, boot/code sections, roots, bootstrap parameters,
   BSS range, thread objects/stacks/priorities, and OS globals. Check every range
   against actual RDRAM and its aliases. Reserve synthetic boot thread memory so
   it does not overlap loaded data, stacks, heaps, framebuffer, or task buffers.
5. Model compressed code and overlays before discovering them. Implement
   deterministic image preparation and fingerprint the resulting analysis image
   using the existing loader conventions. Preserve the original ROM identity for
   import/runtime validation. Keep analysis offsets distinct from original ROM
   DMA offsets and runtime addresses. Handle relocations, PI DMA, section
   load/unload state, and virtual mappings in the game adapter as needed.

A matching decompilation, ELF, map, or older emulator symbol table can supply
hypotheses. Verify its revision and compare the relevant bytes and layout before
using it. Names and addresses from another revision are not evidence by themselves.
Do not introduce an external source tree as a build dependency for symbol lookup.

## 2. Integrate the game with the current build

Keep game-specific files under `games/<game>/`. Put shared services in `runtime/`,
`include/`, `backends/`, or `tools/` only when their behavior is actually shared.
Follow the required filenames and source placement in [AGENTS.md](AGENTS.md).
Use the same structure for every new game; older layouts and external reference
projects do not override it. Keep all headers, shared adapters and generation
helpers in the game's parent folder. Reserve `debug/` and `release/` for their
own entry points and configuration-specific sources or diagnostic inputs.

Create the files the port needs:

- `profile.toml`: exact ROM identity, load map, bootstrap configuration, checked
  discovery roots/transfers, hooks and policy. Use one complete profile per game.
- `startup-entries.json`: ROM-bound partial function-entry inventory when needed.
- `generate.py`: configuration emission and optional preparation/metadata
  hooks, following `tools/generate_game.py`'s current interface.
- Parent-folder headers, adapters and audio profiles shared by both builds.
- `release/app.c`: the playable runtime and any Release-only sources.
- `debug/app.c`: bounded diagnostics, replay, captures, and startup checks.
- `CMakeLists.txt` and `README.md`: build integration and accurate game instructions.

Do not split the profile into additional TOML files or introduce an inheritance
chain. Export `write_config(profile, metadata, output, startup)` from the
parent-folder `generate.py`, with optional hooks as documented in
[adding a ROM](docs/adding-a-rom.md). Keep game-specific generation logic there
rather than adding game-ID branches to the shared driver.

Register the ID in root `CMakeLists.txt`'s `RECOMP_GAMES` and
`tools/generate_game.py`'s `GAMES`. Add Release/Debug configure and build presets
in `CMakePresets.json`, and matching `.vscode/tasks.json` and `launch.json`
entries. Follow `cmake/Game.cmake` for ROM/save defaults, additional sources,
generated headers, source splitting, and mode selection. Ensure new preparation
inputs participate in CMake dependencies and generation cache invalidation.
List shared translation units in `RECOMP_GAME_SOURCES`; use
`RECOMP_GAME_DEBUG_SOURCES` / `RECOMP_GAME_RELEASE_SOURCES` for additional
mode-specific sources. Neither mode may include or compile files from the other
mode's folder. Check source lists, tool paths and docs against this layout before
handing off the port.

CMake is the user-facing build/run selector. Python handles generation and
analysis. Do not recreate obsolete per-game Python build/launch wrappers.
Share required runtime adapters between Release and Debug. Keep normal Release
play quiet, with correctness checks and crash reporting; keep routine traces,
captures, reports, and diagnostic probes in Debug. Persistent translation fixes
belong in checked metadata or generator code, never in generated C.
All games use `recomp_fail(message, address)` or
`recomp_game_require(condition, message, address)` for fatal errors.
`recomp_game_rom()` installs the common core crash reporter in both modes,
including positional debug launches. Do not implement per-game failure callbacks
or crash writers; extend the shared reporter when additional common diagnostics
are needed. Guest-call history is recorded by core dispatch in Debug.

## 3. Discover functions and triage missing symbols

Use the actual generation pipeline, including the game's preparation hooks:

```sh
.venv/bin/python tools/generate_game.py --game <game> \
  --output build/<game>-report --report-only
```

Read both `symbols.json` and `coverage.json`. Inspect per-function errors,
metadata issues, unsupported instructions, ownership conflicts, and unresolved
transfers rather than only summary counts. Use `tools/discover.py`,
`tools/translate.py`, `tools/code_inventory.py`, and `tools/common.py` to confirm
the accepted schemas. Use the same complete `games/<game>/profile.toml` throughout
discovery, translation and runtime configuration so registered-call policy and
checked metadata remain consistent.

Distinguish a missing semantic name from a missing executable target. A verified
address-based function name is sufficient for translation. For each blocker,
record the guest PC, caller and delay slot, section, target, mapped ROM/image
offset, instruction bytes, and evidence for the proposed classification.

| Symptom | Investigation and durable fix |
| --- | --- |
| Host compiler/linker reports a missing C symbol | Check generated header/config output, handler declarations and definitions, CMake source lists, and the selected game. Fix the producing input or build dependency. |
| Direct guest call has no discovered callee | Map the encoded target to the correct section; verify its body and boundaries. Correct the region or add a checked root, then rerun discovery. |
| `unregistered or unloaded function` | Check the registry and loaded-section state separately. Trace the caller and target register. Missing callbacks need checked roots; present but unloaded functions need the loader/overlay lifecycle fixed. |
| Unresolved `jalr` callback | Trace pointer-table reads, registration code, relocations, and address materialization. Verify candidate function entries and seed discovery. Dynamic registered-call policy still requires a valid loaded target. |
| Computed `jr` or switch dispatch | Find the index/bounds calculation and table, including the delay slot. Verify table size, bytes, every destination, and whether destinations are local labels or separate functions. Add checked jump-table/transfer metadata. |
| Overlapping functions or nonsense instructions | Revisit code/data classification, function ownership, shared tails, delay slots, section overlap, decompression, and relocations. Do not grow a code region merely to absorb the error. |
| Privileged instruction, hardware access, or unknown SDK routine | Determine whether this is an OS/device boundary, a game-specific pager, or real CPU code. Use the shared native catalog/runtime or a checked game adapter as appropriate. |

For callback discovery, use `pointer_entries` and `materialized_entries` from
`tools/metadata_candidates.py`; OOT's `debug/analyze_callbacks.py` demonstrates
their use. Adapt ranges to the new game and exclude known switch tables/data.
Treat pointer scans, stack-prologue scans, and valid-looking MIPS as candidates.
Review call sites, instruction flow, alignment, section identity, and code/data
ownership before adding entries. Do not blindly apply another game's analyzer.

Checked partial entries bind `rom_sha1`, entry VRAM, origin, and a SHA-256 of the
first 16 bytes. That prefix identifies an entry; it does not establish full body
size. Jump-table metadata binds the transfer's `expected_word`, table address,
count, and table SHA-256; explicit transfers check their instruction and targets.
For overlapping overlay addresses, preserve section identity and verify the
schema's ability to distinguish them rather than merging by address alone.
Derive all checks from the verified ROM or prepared image, not guessed constants.

Rerun discovery and the report after each coherent metadata correction. Confirm
the original issue disappeared and no new ownership conflict appeared. A partial
inventory stays `complete = false`. A complete code inventory must classify all
configured bytes, with checked coverage and fingerprints.

## 4. Resolve native services without hiding missing behavior

Reuse `tools/native_hooks.toml` for ordinary SDK OS/library functions. Recognition
requires a complete ROM-body SHA-256; prefix resemblance or a name match is
insufficient. For a new compiler/SDK variant, establish its boundaries and
behavior, then add a verified fingerprint using the shared handler. Implement a
missing common service in the shared runtime. Reserve explicit game hooks for
custom startup, hardware, loaders, or other game-specific behavior.

Honor the guest ABI: register widths/sign extension, arguments and return values,
stack layout, guest byte order, OS structures, queue semantics, priorities,
blocking behavior, timing, and completion events. Use guest memory helpers.
Retain loaded-section checks and `recomp_prepare_direct_call` for direct native
dispatch; dynamic or ambiguous destinations retain checked `recomp_lookup`.

Do not stub unknown functions to return success, ignore task errors, relax
fingerprints, or disable worker conflict checks to make boot proceed. Explicit
unsupported failure boundaries can isolate unimplemented paths during partial
bring-up, with a verified address and reason. They remain unresolved work and
cannot stand in for behavior reached by the acceptance replay.

## 5. Run, diagnose, fix, and replay

Configure and build separate modes once generation is viable:

```sh
cmake --preset <game>-debug
cmake --build --preset <game>-debug -j 4
./build/<game>-debug/bin/<game> --frames 300
```

Inspect `generation.log` when configuration fails. Check Python dependencies and
the populated pinned `third_party/rt64` submodules before attributing toolchain
failures to the port. The current playable CMake target requires graphics and
audio; use a bounded diagnostic path if headless bootstrap isolation is needed.

Use short bounded runs to reach successive milestones: BSS/system entry, initial
thread ABI, queues/events, first graphics/audio task, title, menu, gameplay, and
a level/mission or overlay transition. Extend bounds as progress requires.
Capture the exit result and inspect final artifacts; a process that was merely
started is not a passed run.

For crashes, retain the matching executable/generated sources, failure JSON,
RDRAM snapshot, recent calls, thread registers, and task state. Use
`tools/inspect_crash.py`, Debug instruction PCs, and GDB when needed. Reproduce
the exact failing input/path, explain the causal chain from evidence, fix the
owning layer, regenerate/rebuild, and replay beyond the previous failure point.
For hangs, inspect runnable/blocked threads, queue ownership, event delivery,
guest time, busy waits, and worker completion rather than raising budgets alone.

Pay particular attention to these runtime boundaries:

- A synthetic IPL thread must not return into a boot `BREAK` or remain owned by
  the scheduler after its guest memory is reused. Stopping it may still allow
  saved-state synchronization. At a verified handoff, destroy/join it through the
  runtime thread lifecycle before that memory becomes framebuffer/task data.
  Derive the handoff from this game; do not copy another ROM's return address.
- For `guest wrote an in-flight task output`, map the conflicting physical byte
  to guest aliases, thread objects/stacks, buffers, and task outputs. Fix the
  lifetime or scheduling error while retaining `recomp_worker_merge` checks.
- Verify graphics and audio microcode/task profiles independently. Trace malformed
  ADPCM or unsupported RSP tasks to their producer/profile/decoder. Muting output
  does not establish correct audio execution.
- Keep SDL window/input on the owner thread. Service VI/AI/timers/task completion
  often enough between presentation deadlines. Verify the game's counter/retrace
  calibration and avoid contaminating it with host startup delays.
- Exercise actual PI DMA/decompression, overlay activation/unloading/relocation,
  save type/persistence, and controller polling rather than assuming boot proves them.

## 6. Establish playable acceptance, then check shared regressions

Use the real X11/Xwayland desktop and Vulkan renderer. Headless boot or successful
Vulkan initialization cannot establish rendered gameplay. If desktop access is
unavailable, continue independent work and report that acceptance gap explicitly.

Verify title/menu navigation, starting play, visible scene updates, movement and
game-specific actions, working controller input, and audible audio. Check a
meaningful transition and save/reload when available in the tested path. Retain a
small deterministic replay in `games/<game>/debug/` when it supports reproduction;
use `tools/controller_replay.py` for the supported JSON-to-text input conversion.
Inspect captures visually. Separate presentation counts from actual game progress.

Build and run Release through the same exercised path; Debug success alone is
insufficient. Check a normal run without capture overhead for crashes, timing,
audio underruns/resets, and unintended routine output. Record duration, frame/task
counts, replay, and evidence paths. Do not infer game FPS from helper benchmarks
or claim full-game support from startup coverage and one playable scene.

After the game runs, run focused checks for the shared code actually changed and
smoke an affected existing game. For emitter changes, regenerate Debug/Release,
inspect affected MIPS semantics including delay slots/branch-likely where relevant,
and verify Debug PC tracking stays compiled out of Release. Add lasting regression
tests for demonstrated bugs when they provide meaningful coverage; avoid tests
that just mirror implementation or assert generated wording.

## 7. Clean tests and remaining useless files before finishing

Cleanup is part of completing the port. Inspect the final tracked and untracked
diff against the starting status and identify files created by this work.

- Remove temporary tests, throwaway harnesses, one-off ROM scanners, scratch
  scripts, experimental patches, duplicate fixtures, unused copied game code,
  obsolete launchers, and abandoned configuration. Consolidate useful analysis
  into the maintained tools or the game's Debug support.
- Remove diagnostic logging/probes from normal Release play. Keep only useful
  maintained regression tests and reproducible replay inputs; do not delete a
  meaningful test because it exposes an unresolved failure.
- Clean task-created disposable binaries, logs, WAVs, RAM dumps, screenshots,
  caches, and intermediate reports once their findings are recorded and they are
  no longer needed. Preserve evidence and matching build artifacts needed for an
  unresolved crash. Keep generated outputs under ignored `build/`, and private
  runtime data under ignored `local/`; commit only intentional documentation images.
- Before deleting a file, check imports/includes, CMake, generation dependencies,
  tests, presets, editor tasks, and documentation references. Preserve required
  inventories, fingerprints, adapters, licenses, user ROMs, saves, texture packs,
  and unrelated existing files. Use targeted deletions; do not blanket-clean the
  worktree or shared `local/` and `build/` trees.
- After cleanup, regenerate/build in a fresh directory when needed to show the
  port does not depend on deleted scratch artifacts. Rerun the affected acceptance
  path if executable inputs changed. Run `git diff --check` and review status for
  leftover references and accidental generated/private files.

Update the game's README, root game/import/build lists, and
[docs/status.md](docs/status.md) with the exact ROM revision, commands, observed
milestones, remaining unsupported paths, and unverified progression. Replace
stale instructions rather than preserving multiple competing workflows.

Finish with copy-pasteable repository-root build/run commands, what was observed
in gameplay, what checks passed, any concrete remaining blocker, and the cleanup
performed. Clearly distinguish playable acceptance from complete conversion.
