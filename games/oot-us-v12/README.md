# Ocarina of Time US V1.2

This profile targets the supplied US revision 2 cartridge image, SHA-1
`41b3bdc48d98c48529219919015a1af22f5057c2` (32 MiB). Game files live here;
ROMs and saves stay in `local/`, and generated sources stay in `build/oot-us-v12/`.
Run these commands from the checkout root:

```sh
.venv/bin/python games/oot-us-v12/run_oot.py \
  --rom '/home/cmartin/Downloads/N64/ROMS/Legend of Zelda, The - Ocarina of Time (U) (V1.2) [!].v64' \
  --render --frames 0

.venv/bin/python games/oot-us-v12/run_oot.py --render --build-only
.venv/bin/python games/oot-us-v12/run_oot.py --render --frames 0
.venv/bin/python games/oot-us-v12/run_oot.py --report-only
```

The launcher verifies and imports the ROM, discovers CPU functions, emits C,
and builds `build/renderer/oot_native_rt64`. It does not require the reference
source repository or a matching ELF. `--jobs N` controls build parallelism.
The default rendered check lasts 1,200 presentations; zero runs until window
close or Escape. Arrow keys control the stick; A/X are N64 A/B, S is Start,
Z is Z, C/V are L/R, I/K/J/L are C directions, and T/G/F/H are the D-pad.
F1 opens the RT64 inspector.

`--capture-every N` saves periodic screenshots, `--wav PATH` captures audio,
`--audio-trace PATH` records AI admission times and playback queue depth as CSV,
and `--mute` disables playback while keeping synthesis active. `--input-script`
accepts the same ROM-bound controller JSON format as the other game launchers.
The native SRAM device uses `<ROM path>.sram`, a 32 KiB save image. Save
creation and SRAM writes have passed initial validation; saving and resuming
later gameplay still need validation.

## Code loading

`prepare.py` reads the original DMA table at ROM offset `0x7960` and decodes
Yaz0 files for static translation. The analysis image appends those decoded
bytes to the verified cartridge image; `startup-entries.json` is bound to that
analysis image's hash. Runtime retains the original cartridge bytes and runs
the translated DMA manager and decompressor to load assets and code.

The initial image maps ROM `0x1000..0x7960` to `0x80000400..0x80006d60`.
Startup clears BSS through `0x8000b780`, enters `0x80000498`, and creates the
priority-10 idle thread at `0x8000063c`, with stack top `0x80007330`.
Main code loads at `0x800116e0`. Its complete decompressed image is verified
before its compiled section becomes callable. The temporary 64DD code image
is similarly verified, and its device probe reports that no drive is attached.

`overlays.json` records the 469 overlay files, canonical addresses and complete
file hashes. Their translated loader still chooses RAM allocations. The native
relocation adapter verifies each loaded file and applies its MIPS relocation
records; generated code reads relocated instruction immediates at the checked
sites. Function lookup maps loaded RAM entries back to compiled canonical
entries. Overlapping loads deactivate previous sections.

Layout and SDK research used the primary
[zeldaret/oot reference](https://github.com/zeldaret/oot), especially
[NTSC 1.2 segment metadata](https://github.com/zeldaret/oot/blob/main/baseroms/ntsc-1.2/segments.csv).
Stored byte fingerprints are measured from the pinned local ROM. Function
starts inferred from direct calls and stack setup remain a partial inventory.
Checked scene-command and player item-action tables supply callback roots.

Callback discovery can be repeated against the current coverage metadata:

```sh
.venv/bin/python games/oot-us-v12/run_oot.py --report-only
.venv/bin/python games/oot-us-v12/analyze_callbacks.py \
  --metadata build/oot-us-v12/rom/discovery/symbols.json
```

This writes `build/oot-us-v12/callback-candidates.json`. Each proposal includes
the ROM-image identity, a byte fingerprint and the data or instruction sites
that supplied its address. The shared scanner follows data pointers and
LUI/address pairs in CPU text, excluding instructions already owned by a
function and checked local jump tables. It does not execute candidate code.
Review candidates before using `--apply`, then repeat `--report-only` to check
ownership and transfers. New computed jumps still require checked table
metadata; a pointer can name a local switch label rather than a callback.

## Current boundaries

The original boot check, Nintendo 64 logo, animated Hyrule Field title sequence,
file select, name entry and new-game opening have passed. A 12,000-presentation
run completed through the opening scenes. This remains a partial CPU inventory;
full-game and broad regression checks are intentionally deferred.

The launcher services native events while waiting for the next presentation.
Sleeping through the whole remaining frame interval bunched retrace messages
together; the original audio manager discards duplicate retraces, starving AI
playback. A measured 30-second title/menu check changed from 28 underruns to zero
with event servicing during that interval. Zelda requests a 160 ms playback
reserve. The corrected two-minute opening check completed 7,200 presentations
and 3,826,848 audio frames, with one queue underrun during a scene transition
and no resets. Continued gameplay listening remains part of audio acceptance.

The NEAD filter adapter also preserves CPU-owned coefficient tables and caches
them locally, matching the pinned microcode's DMEM behavior. The source table
must remain available for the game to prepare a following task while DSP runs.
`ctest --test-dir build/renderer -R '^nead_filter$' --output-on-failure` checks
that ownership, coefficient reloads and INIT history.

JPEG SP tasks are explicitly skipped for this bring-up, with SP completion
posted to let the game proceed. `skipped_jpeg_tasks` in the run report records
the count. JPEG-backed scenes can have absent or stale backgrounds; no JPEG
execution is claimed. The CIC6105 boot challenge has a separate game-specific
native adapter. Ordinary SDK services use the shared native catalog, including
verified PI, VI, controller, timer and audio variants.

The audio backend recognizes the pinned Zelda NEAD program and executes its
commands through the native DSP library. Graphics use RT64's F3DZEX support.
Unsupported privileged OS internals remain explicit failure boundaries.
Failures write a crash JSON and guest RAM snapshot beside the selected report.
