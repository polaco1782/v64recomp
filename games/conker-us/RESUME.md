# Conker port handoff — 2026-10-16

The user requested the repository's `add-game` skill for Conker, then asked to
stop at the next step and leave a handoff for Codex CLI. Work stopped after
static generation and validation. Resume the port through native boot and
rendered gameplay using `SKILL.md`; do not mistake this checkpoint for a playable
port. No external blocker has been established.

## Workspace and preserved state

- Repository: `/home/cmartin/Projects/v64recomp`.
- Original user ROM: `/home/cmartin/Downloads/N64/ROMS/Conker's Bad Fur Day (U) [!].z64`.
- Verified ignored copy: `local/conker.us.z64`.
- Starting status was only `?? SKILL.md`; preserve that user-provided file.
- No commits were made. Inspect current `git status --short` before editing.
- Added `games/conker-us/{profile.toml,startup.toml,startup-entries.json,release/generate.py,README.md,RESUME.md}`.
- Added `conker-us` to `tools/generate_game.py`'s static-generation choices.
- Added 27 complete-body libultra signatures to `tools/native_hooks.toml`.
- Added an in-progress entry to root README and `docs/status.md`.
- No CMake game registration, presets, editor launch entries, apps, runtime
  adapter, audio profile, or renderer extension have been added yet.

## Verified ROM and prepared images

The ROM is big-endian, US region `E`, revision byte 0, title `CONKER BFD`,
size `0x4000000`, header entry `0x80001000`. SHA-1:
`4cbadd3c4e0729dec46af64ad018050eada4f47a`.
MD5: `00e2920665f2329b95797a7eaabc2390`.

`release/generate.py` expands the paged code and data without external sources.

| Section | Guest address | Analysis offset | Size |
| --- | --- | --- | --- |
| `.entry` | `0x80001000` | `0x1000` | `0x50` |
| `.boot_tlb` | `0x80005ab0` | `0x5ab0` | `0x54` |
| `.init` | `0x10001050` | `0x1050` | `0x28080` |
| `.init_data` | `0x800290d0` | `0x290d0` | `0x43e0` |
| `.game` | `0x15000000` | `0x4000000` | `0x1fa130` |
| `.game_data` | `0x80082b20` | `0x41fa130` | `0x2e2a0` |
| `.debugger` | `0x16000000` | `0x42283d0` | `0x4d58` |

The first four sections are initially registered as loaded. Runtime handling
for the remaining sections has not been written.

- Original game blob starts at ROM `0x42450`. Its first word is the relative
  data offset `0x145ed8`. Starting at `0x42454`, XOR each page-table word with
  `0x8039ccca`. There are 508 offsets, starting at `0x800`, ending at
  `0x144700`, delimiting 507 compressed pages.
- Each block has a four-byte big-endian decoded size followed by raw DEFLATE.
  Pages are `0x1000` bytes except the final `0x130` bytes.
- Game code SHA-256:
  `0d18b3a2e8d8f13a7697bba714a3ad75902656871a9d11593c2607cb39c4665d`.
- Compressed game data is at original ROM `0x188328..0x19c7d8`. Data SHA-256:
  `0bbc00e1adfac5ffe6117f5be9c941575f87b341a77d808004603ac1cb87c670`.
- Debugger is copied from original ROM `0x19ea88..0x1a37e0`.
- Complete prepared analysis image SHA-1:
  `5b96fe8c03a0f0843af953b4a15644a3aea86b94`.
  `startup-entries.json` fingerprints this image, rather than the cartridge.
  It deliberately has `complete: false` and only two checked callback roots.

## Boot and native service findings

Addresses below were checked against the ROM disassembly. Proposed runtime
implementations remain untested.

1. IPL load maps ROM offset `0x1000` to physical `0x1000`. The profile currently
   copies `0x100000` bytes and advertises 4 MiB RAM. Entry clears
   `0x8002d4b0..0x80043b40`, sets SP to `0x800314b0`, then jumps to
   `0x80005ab0`. The entry itself uses `0x80001000`, not `0x80000400`.
2. `0x80005ab0` installs a wired TLB mapping from `0x10000000` to physical 0
   and jumps to `0x10001050`. The profile contains a full checked replacement
   for this custom startup function, but **`hook_conker_boot_tlb` has no C
   definition yet**. Its original pair covers physical 0 and `0x400000` with
   4 MiB halves. The shared mapper rejects an odd physical base equal to RAM
   size; derive how to represent that absent upper half for a 4 MiB runtime.
3. Bootproc `0x10001050` clears BSS again, unmaps TLB indexes 1..31 via
   `0x100061f8`, calls OS initialize `0x10022790`, gets/sets SR at
   `0x10022a40`/`0x10022a30`, and sets FCSR. It enables FR=1. These SR routines
   still need a real native boundary; do not silently ignore CP0 operations.
4. First thread: object `0x800318b0`, ID 1, entry `0x100010f8`, argument 0,
   SP `0x8002d8b0`, priority 5. It initializes PI/queues, creates the main
   thread at object `0x80031ae0`, ID 3, entry `0x10001194`, SP `0x800318b0`,
   priority 10, then lowers itself to priority 0 and spins at `0x1000117c`.
   Use the runtime's idle-thread handling/checkpoint at the checked loop.
5. `osCreateThread` stores status `0x0400ff03`; exception code saves independent
   64-bit FPRs. The profile sets `game.fpu_fr = 1`; the runtime scheduler must
   also use `.fpu_fr1 = true`.
6. Derive service initialization at/after BSS clearing, before `osInitialize`.
   Likely avoid a synthetic IPL thread: a host boot context can create the
   initial guest thread and return, but verify this with actual native control
   flow. If a synthetic thread is needed, reserve safe memory and destroy/join
   it before that RAM is reused. Do not copy another game's handoff PC.

Verified SDK globals:

| Purpose | Address |
| --- | --- |
| Thread tail | `0x8002bdf0` |
| Run queue | `0x8002bdf8` |
| Active queue | `0x8002bdfc` |
| Running thread | `0x8002be00` |
| Clock rate | `0x8002bd10` |
| VI clock | `0x8002bde8` |
| Final-ROM flag written by initialize | `0x800428e0` |
| Base timer pointer | `0x8002bd70` |
| Current time | `0x800429b0` |
| Base counter | `0x800429b8` |
| VI interrupt count | `0x800429bc` |

Read the ROM value at `0x2bd70` to derive the timer sentinel; that final value
has not been recorded. Other platform globals, save size, and event tables
still need verification.

## Paging findings and next implementation step

Mainproc clears/allocates memory, reads the game blob offset table via the
original DMA wrapper `0x10004514`, decompresses the game data with
`0x10006240` (entry wrapping `0x1000625c`), then builds page management state.
It calls custom cache setup `0x10005b04`, clears management entries via
`0x10001420`, initializes the free-page bitmap via `0x10005be0`, and calls
main-game entry `0x15007830` at call site `0x10001404`.

The next coding step is a shared Release/Debug Conker adapter implementing
boot mappings, service initialization, and the verified custom pager. References:
`games/goldeneye-us/release/pager.c`, `games/perfectdark-us-v11/perfectdark.c`,
`games/shadowman-us/release/pager.c`, and `include/recomp.h`'s
`resolve_address`, `address_userdata`, and `prepare_call` callbacks.

- Original TLB miss handler: init offsets `0x5c2c..0x61f8`.
- `0x10005b04` includes privileged Context/Wired writes followed by ordinary
  cache allocation code. Preserve allocation semantics while replacing the
  privileged boundary. It requests `0xeb` pages at startup.
- Management table: `0x80043b40`, eight bytes per virtual 4 KiB page (physical
  page pointer, age). The clear loop zeroes `0xfe0` bytes, including a sentinel.
- `0x8003be74`: physical cache base; `0x8003be78`: slot count (halfword);
  `0x8003be70`/`0x8003be7c`: free-bitmap start/end.
- `0x800354f8`: aligned compressed-page scratch buffer;
  `0x800354fc`: table of decoded original ROM page offsets.
- A miss takes the first free bit, clears it, and expands the corresponding
  ROM page into that slot. A fresh page's age is reset to zero. If no slot is
  free, the original eviction loop at `0x6138` selects the greatest signed age
  among occupied entries, increments ages during its scan, clears the victim
  mapping, and reuses its physical slot. Inspect bounds and sentinel carefully.
- Preserve guest-visible management/bitmap state, read-only code semantics,
  loaded-section checks and task-worker RAM conflict checks. Do not load the
  entire code into a guessed RDRAM range over the game's heap.
- Main-game entry `0x15007830` is itself currently unsupported: inspect its
  computed `jr` in `coverage.json`, derive and fingerprint its switch table.
- `0x10012020` also needs checked computed-jump metadata. Most other remaining
  unsupported functions are in the game image. More callback discovery is
  necessary; use verified pointer/materialized roots and review ownership.

## Graphics/audio and integration still to do

- The vendored RT64 GBI list has no Conker/CBFD implementation. Do not assume
  F3DEX2 compatibility or claim rendering support. Inspect the first actual
  task and derive/implement its commands, as needed, under game-specific code.
- `third_party/rsp-hle/alist_naudio.c` contains CBFD-specific operations, but
  `include/recomp_audio.h` currently exposes ABI1, ABI1_LINEAR, NEAD_OOT and
  NAUDIO_MP3 only. Trace the actual task and add a verified CBFD profile/ABI
  path if required; muting audio does not prove synthesis.
- Add game `CMakeLists.txt`, shared runtime sources, Release and Debug apps,
  root `RECOMP_GAMES`, both CMake presets, VS Code tasks/launch configs, and
  appropriate documentation when an actual target exists.
- Follow the skill through bounded boot, real desktop title/menu/gameplay,
  input/audio, a meaningful transition, saves, then Release acceptance.
  `DISPLAY=:0` and X11 sockets X0/X1 were present; desktop access is untested.

## Checks completed and artifacts

```sh
# Regenerate the honest raw report:
.venv/bin/python tools/generate_game.py --game conker-us \
  --output build/conker-us-report --report-only
# Emit partial C with explicit failing boundaries:
.venv/bin/python tools/generate_game.py --game conker-us \
  --output build/conker-us-generated
```

Raw report: 1,385 functions; 1,281 ready, 47 hooked, 57 unsupported;
147,349 instruction words. One global metadata issue remains: 1,873,880 mapped
bytes are unvisited/unclassified. No complete-conversion claim is justified.
Code generation succeeds using 57 explicit failing boundaries. There is still
an undefined custom hook, so this is not a link/build/runtime check.

All 68 generated C units passed `cc -std=c11 -Wall -Wextra -Werror
-Wno-unused-label -Iinclude -Ibuild/conker-us-generated -fsyntax-only FILE`.
The 27 new shared catalog bodies were rehashed against the ROM; every stored
body ends in its actual `jr ra` and delay slot. Recognition over the original
mapped regions of all eight existing game profiles completed without ambiguous
matches. Prepared-image recognition and runtime regression checks are pending.

Reports/logs: `build/conker-us-report/{symbols.json,coverage.json,generation.log,
emission.log,syntax-check.json}`. Generated C/config and
`pending-boundaries.json`: `build/conker-us-generated/`.

Public reference metadata from [mkst/conker](https://github.com/mkst/conker)
was used only as address/layout hypotheses and compared with local ROM bytes.
Its [US cartridge layout](https://github.com/mkst/conker/blob/master/conker.us.yaml),
[analysis layout](https://github.com/mkst/conker/blob/master/conker/conker.us.yaml),
[symbols](https://github.com/mkst/conker/blob/master/conker/symbol_addrs.us.txt),
and [compressed blob layout](https://github.com/mkst/conker/blob/master/game.us.rzip.yaml)
are cached under ignored `build/conker-research/`. No external source tree is a
build dependency. The repository README has some stale offsets; prefer the
checked US YAML and the ROM. One reference symbol names `0x100262c0` as
`osPiGetStatus`; its bytes actually read AI length and the existing shared
catalog already recognizes it as `osAiGetLength`.

Cleanup removed the one-off catalog scanner and duplicate extracted images.
Private ROMs and analysis/generated artifacts remain under ignored local/build
paths. No executable, gameplay, audible audio, save/reload, or Release acceptance
has been tested.
