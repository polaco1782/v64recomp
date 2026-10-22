# RE2 technical bring-up

The current prepared analysis image has SHA-1
`923396abc251e332452edcb1494965de8c465d40`. Regenerate metadata before using
inspection tools; older crash RAM remains useful, but older metadata can name
different sections or analysis bytes.

Boot, main and overlays 3/2/4/5/11/39 are described in the single profile. Overlays
2/4/5 share `0x8017b410` and use `overlay_group` with section-qualified roots
and tables. Their compressed bytes are cartridge offsets; the appended
analysis offsets are never DMA addresses. Loader activation requires the
complete decoded image hash after the original inflater returns.

The opening movie and title menu render. A completed Debug run recorded
1,800 presentations, 3,006 graphics tasks, 734,208 audio frames and audio peak
24,273; its WAV contains nonzero stereo PCM. Gameplay and audible quality
remain unaccepted. The deterministic `debug/options.txt` replay reaches
overlay 5's controller configuration. Before that mapping was added,
the loader stopped at `0x8017b410` with a complete 0x5200-byte decoded image.
The captured image matched overlay 5 exactly. Its callback `0x8017b4a4` comes
from main module-table record 4 at `0x8010b300`.

Retained private evidence:

- `build/re2-us-v1-debug/movie-validation.log`, `movie.wav`, and
  `debug-output/re2-us-v1/boot-report.json` describe the completed movie run.
- `local/crashes/re2-us-v1/crash-2779708-886167805170423.json` and its
  `.rdram.bin` contain the decoded overlay 5 trigger before mapping it.
- `build/re2-us-v1-sdk/overlay5.json` records the full ROM/RAM comparison.

Reproduce the comparison from the repository root:

```sh
.venv/bin/python games/re2-us-v1/debug/inspect_overlay.py --index 5 \
  --ram local/crashes/re2-us-v1/crash-2779708-886167805170423.json.rdram.bin
```

The old long Down hold repeated into Options. `debug/new-game.txt` now uses
a brief Down pulse to select New Game and confirms difficulty and character.
The character-selection freeze was a missing native SDK binding: thread 4
waited at `0x8009bfa4` for queue `0x80132140` after translated `osEPiStartDma`
at `0x80006f60` returned -1 because guest `__osPiDevMgr.active` remained zero.
The caller ignored that result. Its message requests 0x8000 bytes from SRAM
device `0x08000000` with handle base `0xa8000000`. The complete reviewed
0x94-byte SDK body is now bound to `hook_os_epi_start_dma`, and both modes
configure the existing 32 KiB SRAM backend with a separate `.sra` save path.
The old `.mpk` file must not be reused as SRAM.

The initial SRAM read, initialization write and reread complete, producing a
32,768-byte `.sra` beginning with `BIO2`. A subsequent launch loads that file
and completes its read. `build/re2-us-v1-sdk/epi-bound/report.txt` confirms the
complete-body native match. Private pre-fix thread/RAM evidence is in
`build/re2-us-v1-debug/transition-state.txt` and `transition.rdram.bin`.

The replay now reaches main module-table entries `0x800995fc` (thread 4) and
`0x8009c370` (thread 5). Both are checked roots. The module-slot setter
`0x8004cd54` stores its callback argument at slot +0x1b8, consumed by
`osCreateThread` at `0x8004cb60`; callbacks `0x80052f40` and `0x8009acec`
use that same verified path. Their bounded switches are pinned in the profile.
The matrix helpers require signed 64-bit `DADD`; the shared emitter now supports
it with overflow failure and keeps trapping operations when the destination is
`$zero`. `tools/check_dadd.py` exercises ordinary results, aliases, overflow,
zero destinations and delay slots under UBSan at O0/O2 in both configurations.

Gameplay resource inflation next reached the fixed-Huffman allocator callback
`0x800040d4` from `0x80003c50`. Its temporary-stream pointer setup at
`0x80004148..0x8000414c`, complete body and ABI are checked in the inventory.
`build/re2-us-v1-sdk/game-inflate/` records the pre-fix diagnosis.
The audio transfer callback `0x800b44f4` is also checked: its fifth initializer
argument becomes stream +0x190 and is called at `0x800b4b30`. CPU sample filter
helpers `0x800b682c/6844/6854/686c` are computed from four never-taken BLTZAL
link values and passed in t3 to eight JALR sites in `0x800b6788`. Their shared
saturation tail ends at `0x800b68c0`; the following bytes are strings.

An intermittent movie wait at `0x8009f66c` was traced independently. Thread 1
spun with interrupts disabled, movie state 1 and flag `0x801344f4 = 1`.
The runtime had queued VI message 3 before an already-completed SP message 1.
The blocked queue still contained `[1, 4, 0]`; processing SP would clear the
flag. `build/re2-us-v1-debug/movie-order-state.txt`, `movie-order.rdram.bin`
and the pre-fix `gameplay-validation.log` captured the ordering (the replay log
is overwritten by later runs). The shared scheduler now polls task receipts
and drains their event FIFO before clock/platform services enqueue elapsed VI.
The second drain still delivers newly generated device events. The synthetic
`tools/check_task_event_order.py` uses real task, queue, clock and VI services,
including a one-message queue that previously lost SP entirely. It fails with
the pre-fix scheduler and passes the patched Debug runtime at O0/O2.
Both Debug's 4,800-presentation replay and a 295-second Release run passed
that wait. The next failure was the actor-state callback `0x80055c14`, called
at `0x80055bac` with actor a0, input a1 and update context a2. The complete
nine-entry table at `0x8010dc10` ends before the independent global at
`0x8010dc34`. State 1 uses separate 12-entry input/update arrays at
`0x8010dc38` and `0x8010dc68`, indexed by actor byte +5. Their next data at
`0x8010dc98` is packed animation IDs. State 2's six handlers at `0x8010dce8`
and state 3's single handler at `0x8010dd00` have separate consumers. These
arrays and seven bounded local switches are now checked in the profile.
Private diagnosis and disassembly are under `build/re2-us-v1-sdk/actor-update/`.
The later enemy/room coverage builds with 1,579 ready and 62 hooked functions, zero unsupported.
The current `debug/play-through.txt` replay continues through the complete
new-game intro and applies movement/action inputs after the Start skip to test the street.
Keep RE2 diagnostics and replay
inputs in this source directory; private reports, binaries and ROM/RAM data
belong under ignored `build/` and `local/`.

After the actor-state fix, the 298-second Debug replay passed `0x80055c14`
and failed at the room-script dispatch `0x8006da6c`, opcode handler
`0x8006ef68`. The script cursor comes from context +0x1c, indexes the byte
opcode table at `0x80113898`, and handlers return continue/yield/end in v0.
The complete 141-entry table ends at `0x80113acc`; padding precedes the ten
0x174-byte script contexts at `0x80113ad0`. Every slot resolves to a reviewed
script-handler entry, and the last handler `0x80073658` ends at `0x800736a4`.
The table and its operand switches are checked in the profile. Evidence is
`build/re2-us-v1-sdk/street-next/`, with pre-fix crash
`local/crashes/re2-us-v1/crash-2822261-898229204663089.json` and RAM.

The next Release failure at `0x8016e880` came from the enemy dispatcher
`0x80065304`, not module overlay 7. All 52,112 decoded bytes match record 11
plus record 39's 1,910 raw byte patches. The patched SHA-256 is
`3d853615fad8b19f524510d72c6344c340827ed4a0768c45671bd137c5224bc2`.
`debug/inspect_overlay.py --index 39 --source-index 11 --ram` verifies that
comparison against `crash-2831701-898719169875975.json.rdram.bin`.
Both fixed-slot images are prepared from declarations in `profile.toml`; the
original game inflates and patches RAM at runtime. Enemy inflater calls use
RA `0x80032840` / cleanup RA `0x80032850`. The adapter checks the complete
unpatched output, invalidates overwritten sections, then verifies the fully matching patched variant at actor-dispatch entry
`0x80065220`, before its first indirect enemy lookup. Verification must run
once at that boundary: repeating full-image hashing in high-priority OS
callbacks delayed the loader and produced a black transition screen. `prepare_call` itself runs after a target's loaded check;
verification cannot wait until lookup of the initially unavailable enemy entry.
The earlier analysis image without enemy variants had SHA-1
`39cad0b1a053431fc281dbf0308288a8287b910c`; historical diagnosis reports pin it.
The current private export is `build/re2-us-v1-analysis-enemies.z64`.

After enemy-slot activation, collision dispatch `0x80066a48` reached missing
`0x80067a70`. Its complete 14-entry primitive table at `0x801126c0` is checked;
the low-nibble selector does not make the packed data at `0x801126f8` into
additional handlers. The following failure was room-object predicate
`0x80073f20` from `0x80073a78`. The same complete 15-entry array at
`0x80115290` serves three JALR sites with payload pointers at record +0x0c or
+0x14. Mutable globals begin at `0x801152cc`. These predicates and their
six-state local switch are now checked. Private evidence is under
`build/re2-us-v1-sdk/actor-ready-next/` and `collision-next/`; the latter crash
has its matching executable and generated metadata retained in
`build/re2-us-v1-release/collision-replay/`.
