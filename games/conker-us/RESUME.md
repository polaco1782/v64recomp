# Conker US native bring-up — 2026-10-22

Continue with the repository's `SKILL.md`. The port now has a native executable;
it is still being brought up through the intro. Playable acceptance is pending.
Preserve unrelated working-tree edits and the user-provided root skill file.

## Run from the repository root

```sh
cmake --preset conker-us-debug
cmake --build --preset conker-us-debug -j 4
./build/conker-us-debug/bin/conker-us --frames 1800 --capture-every 300 \
  --wav build/conker-us-debug/boot.wav
```

Use `conker-us-release` for the Release build. Rebuild after metadata changes;
old crash reports must be interpreted with their matching binary/generated C.
Raw discovery, without the explicit failing runtime boundaries:

```sh
.venv/bin/python tools/generate_game.py --game conker-us \
  --output build/conker-us-report --report-only
```

## Observed runtime and current work


Debug captures/reports are in `build/conker-us-debug/debug-output/conker-us/`.
Crashes and word-swapped RAM snapshots are under `local/crashes/conker-us/`.
Use `tools/inspect_crash.py REPORT.json`. Guest aligned words in the RAM dump
are little-endian; guest bytes use physical offset XOR 3.

## ROM and mappings

The ignored input `local/conker.us.z64` is a 64 MiB big-endian US revision 0 ROM:
SHA-1 `4cbadd3c4e0729dec46af64ad018050eada4f47a`,
MD5 `00e2920665f2329b95797a7eaabc2390`.
The prepared image SHA-1 is `5b96fe8c03a0f0843af953b4a15644a3aea86b94`.

| Section | Guest address | Analysis offset | Size |
| --- | --- | --- | --- |
| `.entry` | `0x80001000` | `0x1000` | `0x50` |
| `.boot_tlb` | `0x80005ab0` | `0x5ab0` | `0x54` |
| `.init` | `0x10001050` | `0x1050` | `0x28080` |
| `.init_data` | `0x800290d0` | `0x290d0` | `0x43e0` |
| `.game` | `0x15000000` | `0x4000000` | `0x1fa130` |
| `.game_data` | `0x80082b20` | `0x41fa130` | `0x2e2a0` |
| `.debugger` | `0x16000000` | `0x42283d0` | `0x4d58` |

The preparation hook verifies all decoded image hashes. Original ROM offsets
retain their DMA meaning. Runtime RAM is 4 MiB and both profile and scheduler
use independent FR=1 floating-point registers.

## Adapter details

`main.c` handles boot TLB, polled ROM reads, guest services and demand paging.
Boot maps `0x10000000` to physical zero, with an absent odd 4 MiB half. Services
initialize after BSS clear at `0x10022790`. First `osStartThread` retires the host
boot context and enters the actual scheduler. Main entry is `0x15007830`.

The pager uses the original guest allocators/cache (initially 235 slots), not a
flat copy over the heap. Management table `0x80043b40`, cache base `0x8003be74`,
slot count halfword `0x8003be78`, bitmap pointers `0x8003be70/7c`, ROM offset table
`0x800354fc`. It checks offsets against the pinned cartridge, inflates only the
requested page, updates guest mappings/ages and reproduces eviction. Game data
is verified before activating the prepared sections at `0x10005be0`.

Original RareZip blob: ROM `0x42450`; 508 page offsets at `0x42454`, XOR
`0x8039ccca`. There are 507 pages (0x1000 bytes, final 0x130). Compressed game data:
ROM `0x188328..0x19c7d8`. See `generate.py` for decoded image hashes.

Scheduler tail/run/active/running globals: `0x8002bdf0/f8/fc`, `0x8002be00`;
first thread object `0x800318b0`; main `0x80031ae0`. Tick clock 46875000;
VI period 781250. Timer sentinel `0x80042990`, list `0x8002bd70`, current time
`0x800429b0`, base counter `0x800429b8`, VI count `0x800429bc`.


`audio.h` fingerprints NAudio CBFD boot/text/data. Task type 2 uses the
existing CBFD audio decoder. Its unused NULL yield buffer can retain size 0x400;
provided pointers, graphics buffers and audio resume remain strictly checked.

Save setup at `0x1501c5d4` requires a 16 Kbit EEPROM. The adapter configures
2048 bytes at the selected save path; checked SDK probe/read/write variants use
the shared service.

The shared VI mode service clears its initial BLACK state as the SDK does;
framebuffer swaps retain explicit blanking. Cartridge bus reads preserve the
low virtual boot/pager mappings.

Keep runtime artifacts ignored and preserve the matching evidence for any new
failure. Remove task-created scratch artifacts only after recording findings.

Online cross-checks (analysis references, not build inputs):
[matching US ROM checksum](https://github.com/mkst/conker/blob/master/conker.us.sha1)
[object callback source](https://github.com/mkst/conker/blob/master/conker/src/game/done/game_1765E0.c),
[effect callback source](https://github.com/mkst/conker/blob/master/conker/src/game_18D770.c),
and [US symbol metadata](https://github.com/sciaschi/CBFD-Recompiled/blob/master/recomp/conker.us.syms.toml).
The symbol file lists internal assembly labels as separate functions; do not
bulk-import it over the checked shared-frame control flow.
