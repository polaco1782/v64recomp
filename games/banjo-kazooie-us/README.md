# Banjo-Kazooie US

Experimental port of US revision 0, pinned to SHA-1
`1fe1632098865f639e22c11b9a81ee8f29c75d7a` (16 MiB, big-endian).
Original cartridge DMA and RareZip decoding load both resident cores and the
shared level-overlay slot. The adapter verifies each complete decoded image
before activating its compiled functions. Eight MiB of guest RAM preserves the
original Stop 'n' Swop memory scan.

The Nintendo/Rareware logo sequence renders and audio works (confirmed by the
user). Intro/menu and gameplay callback bring-up is still in progress; playable
support and save/reload acceptance remain to be verified.

```sh
.venv/bin/python tools/import_rom.py --profile games/banjo-kazooie-us/profile.toml --rom /path/to/rom
python3 tools/build.py banjo-kazooie-us --debug
./build/banjo-kazooie-us-debug/bin/banjo-kazooie-us
```

Use `python3 tools/build.py banjo-kazooie-us --release` for Release. Both compile
`main.c`, which owns boot, runtime services and the frame loop. `generate.py`
emits `game_config.h` under the selected build directory. Audio profiles and
checked inventories live beside the code.
See [the common layout](../../docs/adding-a-rom.md#7-add-the-native-application-and-cmake-integration)
and [Debug options](../../docs/debugging.md).

## Boot state and logo timing

Keep the original protection checks running with the state left by this ROM's
CIC-6103 IPL3. Its copy loop at `0xa4000498` moves ROM `[0x4c0,0x768)` to
physical zero. The bootstrap restores that block from the cartridge. The
projection check at `0x8024c9c8` expects `0xac2b0010` at `0x1d8`. A mismatch
clips the sky and scene. The animation setter at `0x8028768c` expects
`0x10000003` at `0x238` and adds three seconds to each requested duration on a
mismatch. The profile also supplies the cartridge base and CIC ID 6103 at
`0x80000310`, which the player model checks when setting its scale.

Presentation uses the game's original 30 Hz rate. A 15-second Release comparison
showed steady 33.3 ms game/display intervals with original-rate presentation;
automatic 60 Hz interpolation produced uneven display-frame pairs. The adapter
arms native idle detection only when the main thread starts: the priority-zero
initial thread must first create the PI manager and main thread. Audio attaches
after the boot-to-core1 service handoff so that handoff preserves the AI sink.

## Discovery evidence

The November 8 bring-up adds ROM-checked mesh-transform, HUD item/transition and
map-node dispatch tables, plus ROM-verified actor, collision, geometry, basic
player-state and overlay callbacks. Tables retain complete-byte fingerprints, instruction
checks and verified selector ranges. Geometry opcodes come from model assets
without a CPU bounds check, so their verified pointer slots support individual
entry roots rather than a bounded dispatch declaration. The CPU inventory stays
partial; missing executable targets continue to fail explicitly.

The player-state setup copies four callback fields from 154 cartridge records
into a RAM table indexed by state ID. Discovery reads the four columns from the
fingerprinted source table and registers every non-null method. This covers
states such as landing and beak barge before an attract run enters them. Shared
discovery follows the sprite callback through its global slot and collision
predicates through their argument-forwarding wrapper.

Reference: [Banjo-Kazooie decompilation at
7b6a4c44](https://github.com/n64decomp/banjo-kazooie/tree/7b6a4c44a1a5bf80977adcf48d822ea9d6bdf899).
Its actor layouts, callback ABIs and model-command names were cross-checked
against the pinned, locally decoded ROM. It is a consultation source and is
not a generation or build dependency.
