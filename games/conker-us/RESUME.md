# Conker US native bring-up — 2026-10-18

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

- Desktop X11/Vulkan works. The Nintendo logo is visible in the 600-presentation
  capture. The first 300 presentations exit successfully: 127 graphics tasks,
  139 audio tasks, 264 code-page loads, no evictions, and 100648 synthesized
  audio frames. The early samples are silent; later intro audio has been heard on the desktop.
- Five guest threads run original startup, asset DMA, scene setup, controller
  polling, graphics and audio command generation. No synthetic IPL thread.
- Longer runs now render the 3D N64-logo intro, visible in the frame-1200
  capture (`build/conker-us-debug/frame-1200.png`). Rendering accuracy and
  lighting still need comparison; title/menu and gameplay remain pending.
- A subsequent replay hit RT64's unsupported GPU reinterpretation assertion at
  about 33 seconds. Core `build/conker-us-debug/renderer.core` and GDB output
  `renderer-core.log` show a 64x64 CI8 framebuffer at physical `0x000de080`
  read as 32x64 RGBA16, no TLUT. Conker's graphics adapter now selects RT64's
  RAM-backed framebuffer-copy path, retaining packed-byte conversion and all
  renderer checks. Both modes build. Subsequent Debug replays passed this failure and the scene
  initializer, reaching the scene-update callback below.
- The latest replay passed the actor callback and failed on `0x1510aa20`
  at `0x15174a50`. This is scene descriptor byte +0x0b, using the 27-slot
  callable prefix at `0x8008cd04` (NULLs excluded, padding starts `0x8008cd70`).
  That table and newly exposed switches `0x150b6bf0` (10) / `0x151790e4` (5)
  are now checked. Debug/Release rebuilds are underway.
  Crash: `local/crashes/conker-us/crash-106235-571673770041583.json`.
- Actor callback `0x1513e070` now executes at `0x1513e058`. Its mode remap
  limits table `0x80089f64` to 11 entries. Metadata also includes the adjacent
  3-entry optional selector at `0x80089f90`, and both 18-entry cleanup families
  at `0x80089f9c` / `0x80089fe4`.
- Related signed-selector callbacks are checked at `0x15163588` (reviewed
  13-word prefix `0x8008b36c`, NULLs excluded), `0x151965f8` (10-slot
  `0x8008f890`) and `0x15199788` (four-slot `0x8008f8b8`). The middle table's
  slot 4 points at `0x15161700`, a JAL delay slot in `0x151616d0`; it remains
  excluded pending classification. Seeding it created an ownership conflict,
  which disappears when that unestablished entry is excluded. The original
  function remains translated. This does not supply behavior for slot 4.
- Audio event descriptors at `0x8002b070` have 150 rows of 16 bytes; callback
  column +12 is dispatched at `0x1000d664`. All 16 non-NULL slots are checked.
  `0x1000c350` now executes. Snapshot selector `0x800e0b94` advanced to 3
  (previous stalled intro was 1); this alone does not prove menu acceptance.
- Scene-object update `0x15013078` uses 26 slots at `0x80082e30`; secondary
  arrays are `0x80082ea0` (11 slots) and `0x80082ecc` (22 slots). NULLs are
  excluded. Its command switch `0x150133e0` is bounded to 19 slots and checked.
  The replay passed their former missing-target failure.
- Scene initialization callback `0x15011b94`, called at `0x1500e55c`, now
  executes. Its 38-slot table at `0x80082d90` is checked (slot zero bypasses),
  alongside five verified audio/scene callback entries installed by these
  initializers. The replay passed its former missing-target failure.
- Animation-event dispatch at `0x1506aec0` has a checked 205-word prefix at
  `0x800863fc`, ending at the five-byte script dispatch table `0x80086730`.
  NULLs and non-code word `0x0fa10cdf` at slot 165 remain excluded; its meaning
  is unclassified. Five-byte runner `0x1507bc14` is bound to direct JAL
  `0x15056b94`; its 214 pointer slots end before data `0x80086a88`. Both
  families' bounded switches are checked.
- Frame-1800 and frame-2400 captures show Conker animated on the stage, then
  posing with the Rareware sign. The prior replay reached about 40 seconds;
  later callbacks remain under investigation. `build/conker-us-debug/intro-start.json`
  and its converted text press Start from 25 seconds at five-second intervals.
  This has not established menu navigation yet.
- An intermittent intro stall also needs resolution. A 1800-presentation run
  exited cleanly after 292 graphics / 696 audio tasks but stopped advancing
  graphics around 15 seconds. Main thread 3 waits at `0x150187b8` on queue
  `0x800bea10`; the RSP/audio threads keep servicing tasks. Snapshot
  `build/conker-us-debug/intro-stall.rdram.bin` has `0x800be615 = 2` and frame
  counter halfword `0x800beaa8 = 0xffff`. These observations do not yet prove
  the counter's causal role. `RECOMP_DEBUG_QUEUE=0x800bea10` traces that queue;
  `RECOMP_CONKER_RAM=PATH` retains RAM after a bounded run. Several subsequent
  replays passed the stall point and reached later checked callback failures.
- Invalid collision-table access at `0x1510e69c` / `0x40000000` was reproduced
  twice. GDB caught `0x150a3a70` returning S0=8 while its saved FPR0 was zero.
  The taken branch-likely at `0x150a3b94` entered shared helper `0x150a3bac`
  and returned natively without resuming the outer loop/epilogue. It now has a
  checked continuation at `0x150a3b88`, alongside the fallthrough path. Generator
  continuation support includes conditional branches and tests execute both
  outcomes with ordinary/likely delay slots in Debug and Release. All 22 tests
  pass through discovery, metadata loading, generated C compilation and execution.
  All three direct jumps to that helper are also checked, including
  `0x150a5078 -> 0x150a5070`. The next replay passed the collision failure and
  reached the attached-actor callback above. That validates progress, not the
  whole intro. Debug and Release both build with the collision fix.
- The user confirmed hearing intro audio on the desktop. An incomplete replay
  recorded 1,106,897 nonzero PCM samples. Crash-interrupted WAV headers are not
  finalized; only a successful bounded run supplies the final capture rate/length.
- NAudio MIDI control-change handlers: commands 0..92 use `0x8002ba50`;
  commands 252..255 select four entries at `0x8002bbc4` in reverse order.
  Constructor `0x10009ffc` returns callback `0x10009cbc` at `0x1000a02c/30`.
  Sound setup at `0x15029f18` passes callback `0x150238d8` as stack argument 7.
  Streaming read callback `0x151f3c4c` is assigned by `0x15016258/5c` and
  dispatched from reader `0x151f8088` at `0x151f80e4`.
- Model descriptors at `0x80088c90` contain callback/post-callback/flags in
  twelve-byte rows. Constructor `0x15003c94..ce4` copies them into object
  fields `+0x38/+0x78/+0x92`. Checked tables seed 87 reviewed rows and exclude
  null targets/flag words. Object update callbacks at `0x8008a4e8` have 98
  entries, followed by six different helper callbacks at `0x8008a670`.
  Three draw tables at `0x8008a688`, `0x8008a7b0`, `0x8008a8d8` clamp to 74.
- `0x150adaf0` decodes animation bytecode; operand/opcode jump tables are checked
  in `profile.toml`. Valid opcode slots are 1..30, followed by a separate C
  callback table at `0x800886a4`. Do not mistake adjacent callbacks for local
  opcode labels.
- The callback wrappers `0x150ae0f8/108/118/12c` tail-enter `0x150ae140`.
  Their four adjacent 15-slot tables are at `0x80088420..0x80088510`.
  NULL entries are excluded. The nested call is at `0x150ae180`.
- Abort `0x150ae280` restores SP from `0x800d3858` and uses the shared epilogue
  at `0x150ae0ac` to restore the interpreter caller's registers/RA. Calls from
  `0x1509f850` at `0x1509f9a8` and `0x1509fbb4` must unwind the native callback
  and dispatcher too. Checked `unwind_calls` and `nonlocal_return_calls` model
  this behavior; tests execute normal, aborted and invalid-stack returns.
- Latest raw report: 4392 functions, 4329 ready, 62 hooked, one unsupported OS
  exception-return routine (`0x100078b4`). The inventory remains partial.
  Generation counts are not gameplay proof.
- Title/menu navigation, gameplay, 3D lighting accuracy,
  transitions, gameplay save/reload, and the matching Release replay remain pending.
  The original save probe returns 2 and reads its first four EEPROM blocks.
  There is no external blocker.

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

`conker.c` handles boot TLB, polled ROM reads, guest services and demand paging.
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

`graphics.cpp` fingerprints the actual F3DEXBG.NoN program and initialized
DMEM before binding its custom RT64 GBI. Text is ROM `0x36e90..0x38330`;
DMEM `0x3f410..0x3fb90`. Earlier matching prefixes are a different variant.
Ordinary F3DEX2/RDP operations use RT64; custom vertices, lighting, normals,
coordinate modifiers and four-triangle commands use the game adapter.

`audio.h` fingerprints NAudio CBFD boot/text/data. Task type 2 uses the
existing CBFD audio decoder. Its unused NULL yield buffer can retain size 0x400;
provided pointers, graphics buffers and audio resume remain strictly checked.

Save setup at `0x1501c5d4` requires a 16 Kbit EEPROM. The adapter configures
2048 bytes at the selected save path; checked SDK probe/read/write variants use
the shared service.

The shared VI mode service clears its initial BLACK state as the SDK does;
framebuffer swaps retain explicit blanking. Cartridge bus reads preserve the
low virtual boot/pager mappings.

`debug/analyze_switches.py` reports candidates for review and never edits profiles.
Keep runtime artifacts ignored and preserve the matching evidence for any new
failure. Remove task-created scratch artifacts only after recording findings.

Online cross-checks (analysis references, not build inputs):
[matching US ROM checksum](https://github.com/mkst/conker/blob/master/conker.us.sha1)
[object callback source](https://github.com/mkst/conker/blob/master/conker/src/game/done/game_1765E0.c),
[effect callback source](https://github.com/mkst/conker/blob/master/conker/src/game_18D770.c),
and [US symbol metadata](https://github.com/sciaschi/CBFD-Recompiled/blob/master/recomp/conker.us.syms.toml).
The symbol file lists internal assembly labels as separate functions; do not
bulk-import it over the checked shared-frame control flow.
