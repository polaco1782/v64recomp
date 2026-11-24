# Turok 2: Seeds of Evil US 1.0

The US revision-0 desktop port reaches first-person play in the Port of Adia.
GLES compiles the verified Acclaim
RSP image instead. Both renderers use the same desktop controls and ABI1
audio service. RT64 validation covered the title, new-game menus, both opening cinematics,
on-foot movement, mouse look, pausing and returning to play. Later levels and Controller
Pak save/reload still need gameplay validation.

## Build and run

Import your own ROM from the repository root:

```sh
.venv/bin/python tools/import_rom.py --profile games/turok2-us/profile.toml \
  --rom '/path/to/Turok 2 - Seeds of Evil (U) [!].z64'
python3 tools/build.py turok2-us --release
./build/turok2-us-release/bin/turok2-us
```

The normalized ROM is `local/turok2.us.z64`, size `0x2000000`, SHA-1
`fb0400f21e3f043939ab56500c7b12a3231006f1`. Other revisions are rejected.
The game uses 8 MiB RDRAM and a Controller Pak at
`local/saves/turok2-us.mpk`. Save configuration is integrated; persistence has
not been accepted in gameplay. The corresponding Debug workflow is
`python3 tools/build.py turok2-us --debug`. Append `--gles` to either workflow
to select the alternative renderer.

A Linux Debug GLES2 replay rendered the Controller Pak prompt, title, menus and
the paused Port of Adia view (capture at presentation 2,400). The headless run
hit its wall-clock limit before completing the requested 3,250 presentations.
The separate line-microcode automap, alternate VI modes and live desktop
controls on GLES2 remain unverified.

## Desktop controls and field of view

WASD keeps the cartridge's C-button movement on foot and while climbing ladders.
Move the mouse to look around, including relative to the wall while climbing.
Left-click fires, Space or right-click jumps, Q and R select
weapons, and F operates the map. Tab pauses while the mouse is captured.
In menus, Tab or S presses Start, Enter or A confirms, and E or X goes back.
During either New Game prologue scene, press **Tab** or **Enter** (controller **Start** or **A**) once to go straight to the Port of Adia. Leave the buttons released to watch the scenes.
Escape releases the mouse and F4 captures it again. Menus and physical
controllers use the standard N64 bindings. Set `RECOMP_TUROK_MOUSE=0` for those
bindings throughout, `RECOMP_MOUSE_SENSITIVITY` for degrees per count (default
0.08), or `RECOMP_MOUSE_INVERT_Y=1` to invert vertical look. Debug controller
replays retain the recorded pad values.

Keypad + and − adjust horizontal FOV in one-degree steps from the first on-foot
camera's original angle to 140°. The change preserves the guest's focal-length
ratio during area transitions and zoom. The world matrix, object tests, room
planes and bounds use the expanded view. The HUD retained its screen position
in the Port of Adia; the equipped weapon appeared smaller at the wider angle.

The local ROM confirms the on-foot look at `0x0024ef70`, world camera at
`0x0027d160`, and their caller-specific input and projection boundaries.
The generator checks the original function bodies and instructions against
`profile.toml` and records them in `input-hooks.json`. An isolated run of the generated guest
routines checked the projection at 140°, restored the original matrix, and
verified widened corners, bounded mouse pitch and native stick input. In a live
Port of Adia run, keypad + widened the stationary player’s view from 59.4° to
104.4°. The world projection’s X/Y scale changed from 1.753/2.338 to
0.776/1.034, and keypad − restored both original values. Mouse input rotated
the player by 49.9°, and W and D moved the player across the dock. [Retail recompilation reference](https://github.com/n64hub/turok-2-recomp/blob/da8f3d2d50f06af034ed3a74f5f09b23729f8aa9/us.toml)

## Verified loading and callback discovery

The complete SN64 boot stub installs a global pair of 1 MiB TLB pages at virtual
`0x00200000`, backed by physical `0..0x200000`, and calls `0x0028d380`.
The initial cartridge copy is ROM `0x1000..0x101000` to `0x80000400`.
The original main clears BSS at `0x800c4000..0x8014a000`. Its thread setup uses
FR=1 and the SN64 SDK's `0x230`-byte thread objects. Native SDK replacements
require complete-body catalog fingerprints, including aliases of duplicated
SDK bodies in the ROM.

The Expansion Pak path loads ROM `0x14a000..0x186ae0` to `0x80780000` and maps
a pair of 256 KiB pages at `0x00400000`. The native map boundary verifies all
`0x3cae0` copied bytes before activating the compiled `.virtual` section. The
code bank remains loaded by the original guest loader.

Data reservations beginning at `0x00480000` are paged by the original VM manager
and reader. The C11 pager delivers a checked fault to those guest threads and
requires an installed mapping before retrying. It does not manufacture missing
pages. Startup has exercised page reads, DMA, mapping and caller restart.
Synthetic bootstrap storage occupies the reserved Expansion Pak code-bank tail;
the real main thread retires the boot continuation at its verified first SDK
call (`0x29e3e0`, return `0x288890`) before heap allocation.

`profile.toml` owns verified switch tables, the eighteen engine modes and all
mapping/hook configuration. `startup-entries.json` records ROM-bound callback
producers and consumers. Shared discovery automatically follows fingerprinted
`osCreateThread` entries, proven register/stack callback arguments and constant
indirect calls, including a valid entry sharing a return delay slot.

Before another executable run, review related callback families together:

```sh
.venv/bin/python tools/analyze.py review-candidates --game turok2-us \
  --build-dir build/turok2-us-debug --preflight
```

Read `summary.json`, `ranked.json` and the linked SDK report under that build's
`candidate-review/turok2-us/preflight/`. These reports describe the discovered
graph, not complete game coverage. Menu descriptors, actor state records, class
initializers and embedded animation methods are reviewed as families. Pointer
stores are checked against the corresponding object and indirect-call consumer;
an address-looking value alone does not establish a callback.

## Current graphics boundary

The first captured graphics task uses text `0x800bc400`, data `0x800bf740` and
an ordinary byte-sized display-list descriptor. Exact ROM spans and task hashes:

| Task region | ROM offset | Bytes | SHA-256 |
| --- | --- | --- | --- |
| Graphics text | `0xbd000` | `0x1000` | `962fad8d596512b7a4b95f2c2ad8736f2b09b0dad70ae2cd793100df8ffb42af` |
| Graphics data | `0xc0340` | `0x800` | `c4ba783211d6f282eea4575dde556da810b0a617833ac6b1e7932dbc4610b409` |

Every submitted graphics task verifies the complete `0x1390`-byte program and
`0x420`-byte data span in guest memory, including the RSP overlay images beyond
the task descriptor's nominal text size:

| Verified span | SHA-256 |
| --- | --- |
| Text and overlays | `6734f26b67b25c6a521dc2ee56d343d33c8005ea4001c2db83fbabf539e62e57` |
| Microcode data | `462006091cd750bfb2f191d8da8f6221d64fd4257852cacab4aacd94e27b3ff1` |

The initial black output also required complete-body native bindings for this
SDK's VI mode, special-feature and current-framebuffer routines. Guest SDK
context writes alone did not configure the native VI service. Captures include
the legal screen, Expansion Pak notice, opening cinematics and first-person
Port of Adia. Individual point lights still need validation. The separate
Acclaim L3DEX2 automap requires a line-microcode adapter. The title-to-gameplay replay produced stereo PCM at 22,047 Hz.

The Debug replay enters New Game and presses Start once at frame 2,640.
Both prologue scenes finish through the original mode transitions. Player
control returned by frame 2,771 in the recorded run. A separate replay let
Adon’s scene play through and skipped the Port introduction with A.

```sh
./build/turok2-us-debug/bin/turok2-us --frames 3250 \
  --input games/turok2-us/boot-menu.txt --capture-every 300
```

To inspect the captured microcode and the first display-list commands:

```sh
.venv/bin/python tools/analyze.py rsp-task local/crashes/turok2-us/REPORT.json \
  --rom local/turok2.us.z64 --commands 16
```

The separate DRAM-output DP path also remains under review: this ROM's
`osDpSetNextBuffer` uses a 64-bit length in `a2:a3`, so a conventional two-argument
native binding would be incorrect. The current FIFO graphics task does not
establish that path's acceptance.

## Reference evidence

[mgrz18/turok2-decomp](https://github.com/mgrz18/turok2-decomp) supplies a matching
ROM identity, SN64 naming and VM investigation hypotheses. Its current layout
and microcode notes contain claims that differ from the verified runtime bytes;
the profile and checks above follow this cartridge. In particular, an SDK string
does not prove binary identity or backend decoder support.
[SirBraga/turok2-recomp](https://github.com/SirBraga/turok2-recomp) was also
consulted for hypotheses. Neither external project is a build dependency and no
external runtime implementation was copied. Reference branches were consulted
on 2026-11-11; their later changes do not update this port automatically.

[GLideN64's Acclaim handlers](https://github.com/gonetz/GLideN64/blob/master/src/uCodes/F3DEX2ACCLAIM.cpp)
and [lighting implementation](https://github.com/gonetz/GLideN64/blob/master/src/gSP.cpp)
were consulted for command/layout hypotheses. The C11 implementation was written
independently and checked against the RSP instructions in this ROM.
