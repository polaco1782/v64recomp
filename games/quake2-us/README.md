# Quake II US revision 0

The port uses the original cartridge code, RT64 graphics, standard NAudio and
**8 MiB of RDRAM with the Expansion Pak enabled**. The cartridge's startup screen
confirms “Expansion Pak detected. Features enabled.” Full-game progression is
not yet validated. Debug and Release build, the bounded Debug replay reaches
Strogg Outpost with first-person rendering and nonzero audio output, and the user
confirmed that the game runs well. Further testing stopped at the user's request;
save/reload, sustained audio quality and broader progression remain unverified.

Single-player mouse-look and WASD are enabled by default. Mouse wheel changes
weapons; Space/right-click jumps; Shift changes run speed while held; Tab pauses.
Esc releases/recaptures the mouse and F4 recaptures it. See
[controller findings and adapter controls](controller.md) for the complete
mapping, verified ROM evidence and remaining gameplay checks. This adapter has
static verification and Debug/Release build coverage; the user confirmed it
works well in gameplay.

## ROM and launch

Supply `Quake II (U) [!].z64` locally. The exact normalized identity is:

- Size: `0xc00000` (12 MiB), big-endian.
- SHA-1: `2edb00e602e7c2813a5d6a04dfec80487627237a`.
- Header title: `QUAKE II`; country `0x45`; revision `0`; entry `0x80000400`.

Run from the repository root:

```sh
.venv/bin/python tools/import_rom.py --profile games/quake2-us/profile.toml \
  --rom '/path/to/Quake II (U) [!].z64'
cmake --workflow --preset quake2-us-release
./build/quake2-us-release/bin/quake2-us
```

Controller Pak storage defaults to `local/saves/quake2-us.mpk`. Use `--save FILE`
for a separate image. Both configurations use the same game adapter and shared
core crash reporter. Debug has its own entry point, input replay and captures:

```sh
cmake --workflow --preset quake2-us-debug
./build/quake2-us-debug/bin/quake2-us --frames 2400 \
  --capture-every 240
```

Debug replay events count host presentations, while the cartridge uses guest VI
time. Capture overhead and a slower build can change the menu timing. Inspect
the resulting screens before treating a replay as gameplay acceptance. Diagnostic
output is under `build/quake2-us-debug/debug-output/quake2-us/`; crash reports and
RAM snapshots are under `local/crashes/quake2-us/`.

Quake II defaults to **optimized Debug** (`RECOMP_OPTIMIZE_DEBUG=ON`). The
translated CPU, runtime, audio and renderer adapter use compiler optimization
while retaining `RECOMP_DEBUG`, instruction counters, checked guest memory,
the overlay, captures, assertions, debug symbols and the matching Debug CRT.
RT64 uses its shared release-optimized build with symbols. MSVC iterator tracking
is disabled by default for all games, including Debug; the user confirmed a
substantial improvement in Quake II's larger scenes after this change.

Optimized source stepping can skip lines or hide locals. For unoptimized
stepping, configure the same build directory with
`-DRECOMP_OPTIMIZE_DEBUG=OFF`, then rebuild. MSVC's `/RTC` compiler checks are
disabled in optimized Debug because they conflict with `/O2`; project-level
guest validation and crash diagnostics remain enabled. Changing this option
rebuilds the game and its local runtime/backends, not the shared RT64 cache.
`RECOMP_TRACE_CALLS` is read once at startup; enabling it still prints every call
and can substantially slow the game.

For the remaining scene-dependent slowdown, use the shared `RECOMP_TIMING` CSV
report in either configuration; see [performance timing](../../docs/debugging.md#shared-performance-timing).
Capture both a smooth area and the slow area in one run. Process CPU utilization
alone does not establish whether the original game, scheduler or renderer is
limiting the frame rate. The report separates graphics wall time from calling-
thread CPU time to help distinguish processing cost from waits.

## Verified implementation

The initial load maps ROM `0x1000..0x72940` to guest
`0x80000400..0x80071d40`. The entry clears BSS
`0x80071d40..0x800a7e10`, sets SP to `0x80095fe8`, and jumps to `0x80000450`.
That startup creates mainproc at `0x800004ac`, which creates gameproc at
`0x80000540`. The synthetic IPL thread is retired at mainproc's first PI-manager
call, before allocation can reuse its storage. Debug checks the BSS clear and
the instruction return address at this handoff.

Mainproc's final path at `0x80000510` lowers its own priority to zero, then loops
over `osYieldThread` at `0x80000520`. Generation checks the complete transition
and loop before configuring object `0x80094c60` as the scheduler's idle thread.
Startup still runs at priority 10; only its priority-zero phase is parked.

`osMemSize` is a data global at `0x80000318`, initialized to `0x800000` alongside
the 8 MiB runtime allocation. The cartridge reads it directly at `0x80006354`
and selects Expansion Pak features when it exceeds 4 MiB; no function hook is
needed for this read.

One complete `profile.toml` binds the ROM, load, bootstrap, switch tables and
function tables. `startup-entries.json` is a **partial** entry inventory: each
root has a first-16-byte fingerprint and reviewed callback provenance. It does
not classify every byte as code or data. Normal SDK routines use complete-body
SHA-256 signatures in the shared `tools/symbols.json` catalog.

Callback groups include:

- The 120 entity name/spawn records at `0x80066770`, selected by the bounds check
  at `0x80039444`. Types 0 and 79 have null callbacks.
- The 41 item records at `0x80065adc`, stride `0x3c`, with pickup/use/weapon
  pointers at offsets 4/8/12. Weapon firing passes through `0x8004775c`.
- The 18 controller actions at `0x80065764`, shared by buttons and analog axes.
- The 45 Mus sequence handlers at `0x8006baa0` for opcodes `0x80..0xac`.
- Entity think/touch/use/pain/die fields, mover completion and monster callbacks.
- Monster move descriptors and frame arrays: `0x80036ee8` bounds the frame
  index, reads `{first,last,frames,endfunc}`, and invokes `{ai,distance,think}`
  records at stride 12. The reviewed arrays contain 143 move descriptors.

The graphics task uses the cartridge microcode at `0x80062320`; it runs through
the existing RT64 adapter. The audio task uses boot/code/data at
`0x80060e20` / `0x800636f0` / `0x8006e2e0`, with exact task descriptors and hashes
in `audio.h`. Standard NAudio opcode 14 is a volume-rate update, which differs
from the filter in the existing MP3 and Conker modes.

The initial single-frame stall came from an untranslated next-framebuffer query
reading stale guest VI context after native swaps. Binding that SDK body to the
shared getter restores graphics scheduling. The current-mode query now reads
the selected native VI mode through the shared OS service as well.

## Analysis and references

```sh
.venv/bin/python tools/generate_game.py --game quake2-us \
  --output build/quake2-us-report --report-only
.venv/bin/python games/quake2-us/debug/analyze_callbacks.py \
  --output build/quake2-us-callback-review.json
```

The callback analyzer only reports candidates and animation structure evidence;
it never modifies the inventory. Review pointer producers, consumers and complete
function boundaries before adding entries.

Public research did not identify a matching N64 Quake II decompilation. The
[Q2N64 PC project](https://github.com/Joca64/Q2N64---PC) ports maps to the PC
engine; its code and addresses are not used here. SDK identification used the
repository's pinned [n64sym signatures](https://github.com/shygoo/n64sym/tree/ccf4600f3389f1a84bde23339225cf372fdf7712),
followed by local body/ABI review. Audio identification was checked against
[Mupen64Plus RSP HLE](https://github.com/mupen64plus/mupen64plus-rsp-hle/blob/8a7a472a7172eb2c8725b305eae26818ed7b51a2/src/hle.c)
and the bundled standard NAudio command dispatcher at that revision.
No external source tree or downloaded game assets are build dependencies.
