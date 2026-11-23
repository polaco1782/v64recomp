# Banjo-Kazooie US

Experimental port of US revision 0, pinned to SHA-1
`1fe1632098865f639e22c11b9a81ee8f29c75d7a` (16 MiB, big-endian).
Original cartridge DMA and RareZip decoding load both resident cores and the
shared level-overlay slot. The adapter verifies each complete decoded image
before activating its compiled functions. Eight MiB of guest RAM preserves the
original Stop 'n' Swop memory scan.

The Nintendo/Rareware logo sequence renders and the user confirmed audio.
Desktop runs reached file select and Spiral Mountain on RT64 and GLES2. Full
progression and save/reload remain unverified.

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

## Desktop controls

WASD and mouse controls are enabled by default on RT64 and GLES2.

| Input | Action |
| --- | --- |
| W / A / S / D | Move relative to the camera. Hold Shift to walk. |
| Mouse | Orbit the follow, climbing, or Spiral Mountain camera. In first-person view, look around. |
| Space | Jump (A) |
| Left click | Attack (B) |
| Right click or Ctrl | Crouch (Z). Hold it with a C-button for learned moves. |
| Q / R | C-left / C-right |
| F / E | C-up (first-person view) / C-down (camera distance) |
| Tab | Start or pause |
| Arrow keys, Enter, E | Navigate menus, confirm, go back |
| Enter / Q / Space, E / R | Dialogue A / B |
| Esc / F4 | Release / recapture the mouse |
| Keypad + / − | Widen / narrow the gameplay field of view by one horizontal degree |

Dialogue keeps mouse capture and WASD active. Camera motion pauses during
text boxes and discards the movement made while they were open.

The default view is the cartridge’s 40° vertical FOV. Keypad − stops at that
original setting. Widening the view also expands the CPU visibility planes.
The HUD, dialogue backdrop, file select, and dedicated cutscene maps keep
their original framing.

In Spiral Mountain, desktop checks exercised forward and sideways movement,
mouse orbit outside Banjo’s house, widening the view and resetting it to stock.
The world projection went from 40° to 56.9° vertical over 20 keypad steps.
A GLES2 run also verified first-person entry, mouse yaw and pitch, and exit.

Set `RECOMP_MOUSE_SENSITIVITY` to adjust degrees per mouse count (default
`0.08`). Set `RECOMP_BANJO_MOUSE=0` for the original keyboard bindings.
Gamepads can be used with either setting.

## Experimental widescreen (GLES2)

Resize to a widescreen ratio such as 16:9 and press **F7** during gameplay in
Debug or Release. Press F7 again for the original view. This is off by default
and accepts drawable ratios above 4:3 through 21:9. Set
`RECOMP_BANJO_WIDESCREEN=0` to disable the experimental adapter entirely.
Dialogues, transitions, and non-gameplay modes fall back to the stock ratio.

Only the two checked world-camera calls use the wider perspective. The original
matrix routine and recompiled RSP still run, and the matching CPU visibility
planes expand horizontally. Vertical FOV, the keypad control, and the native
framebuffer size are unchanged by F7. The scene is rendered anamorphically and
then presented at the wider ratio, not stretched from the stock projection.

The regular HUD shares the framebuffer and can still look wider in this
prototype. Preserving its aspect requires marking the verified item HUD and
print draw regions before the RSP, then compensating those regions only. The
compiled microcode was verified to pass an RDP no-op marker unchanged in an
isolated task, but no markers are emitted into normal game display lists yet.
This mode has not been validated in gameplay.

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

The [BanjoRecomp US 1.0 symbol map](https://github.com/BanjoRecomp/BanjoRecompSyms/blob/6820055ca076e94e30e53d917bd9e5f71c28ca20/bk.us.rev0.syms.toml)
served as an independent check of the shared candidate scan. In a snapshot
with 6,017 registered functions, 442 upstream entries were absent from both
our registry and candidate report. Of those, 433 followed a return and delay
slot, including alignment padding. Following the end of a checked or probed
body reported 304 additional starts, all matching the upstream section and
address. Examples include the three-instruction viewport FOV and near-plane
getters. Of the remaining 138, the scanner identified 43 more from instructions
scheduled before the stack adjustment, and 36 from complete SDK signature
matches corroborated by a call or a preceding return. All 79 additional
section/address pairs match the reference. Following callback arguments,
SDK address relocations and corroborated direct calls identified 14 more,
all at the upstream section and address. This accounts for 397 of the 442.
Of the remaining 45, 34 contain only `jr ra; nop`, one marks RSP audio
microcode at `0x80272590`, and ten lack sufficient independent evidence of
a function boundary. The external map is only used for comparison.

Reference: [Banjo-Kazooie decompilation at
7b6a4c44](https://github.com/n64decomp/banjo-kazooie/tree/7b6a4c44a1a5bf80977adcf48d822ea9d6bdf899).
Its actor layouts, callback ABIs and model-command names were cross-checked
against the pinned, locally decoded ROM. It is a consultation source and is
not a generation or build dependency.
