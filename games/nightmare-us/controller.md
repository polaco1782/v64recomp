# Nightmare Creatures input adapter

The adapter targets the US cartridge with normalized SHA-1
`bea6c21b3ef5950bdf8cdb255955da6c4746eea8`. Hook addresses, structure fields and
button conversion below were checked against its MIPS instructions. Live
acceptance of this adapter is left to the user; static verification and builds
do not establish gameplay timing or camera feel.

## Mouse and keyboard

This is third-person mouse steering with the original following camera.
Horizontal mouse movement turns the character. Vertical mouse movement has no
effect: the adapter does not add a free camera or change camera pitch.

| Input | Action |
| --- | --- |
| Mouse left/right | Turn character left/right |
| W / S | Forward / backward |
| A / D | Native left / right side dodge |
| Left mouse | Strike / take nearby item |
| Right mouse | Kick |
| Space | Jump |
| Ctrl | Defend / accurate movement |
| Shift with W/S | Slow, accurate movement |
| E | Use selected inventory item |
| Wheel up / down | Select previous / next inventory item |
| R / Q | Previous / next inventory item |
| F | Display lives / hold the native inventory modifier |
| Tab | Start / pause |
| Arrows, Enter, E | Menu navigation, A/confirm, B/back |
| Esc / F4 | Toggle mouse capture / recapture |
| F10 | Quit |

A/D retain the game's side-dodge animations and collision rules. They do not
introduce FPS-style strafing. Combat combinations still go through the original
action state machine. Mouse wheel selects inventory items rather than changing
the character's melee weapon.

The adapter is enabled by default in both configurations. Set
`RECOMP_NIGHTMARE_MOUSE=0` to restore ordinary controller input and keyboard
bindings. `RECOMP_MOUSE_SENSITIVITY` specifies degrees per mouse count, default
`0.08`, accepted range `0.001..10`. Vertical inversion has no effect here.
Debug controller replay through `RECOMP_NIGHTMARE_INPUT` disables the adapter.

Capture releases when gameplay gates fail, the window loses focus or an overlay
captures input. Esc/F4 can recapture during eligible gameplay. Physical
controller gameplay is available with capture released. While captured, the
adapter owns movement and actions, while preserving Start from either device.

## Cartridge controller format

The priority-15 controller thread at `0x8007f51c` repeatedly calls:

1. `0x8007f440`: starts the SDK controller read, waits for its completion, reads
   `OSContPad` records and returns an array of pointers to them.
2. `0x80071d8c`: converts the first controller into the engine's older pad format.

The native service integration already uses a 1 ms serial transfer to let game
code run between controller reads. Mouse motion is **not** consumed on this
fast polling thread.

`0x80046c40` registers buffers at `0x800b93e0` and `+0x22` through `0x80071d34`.
Their pointers are held at `0x8009ff40` and `0x8009ff44`. The converter writes
status/type bytes at offsets 0/1, active-low buttons at `+2`, and centered
unsigned axes at `+4..+7`. The reported type is `0x70`.

Raw signed stick axes are multiplied by 127 and divided by the calibration
value at `0x8009ff4c`. The diagonal correction scales the vector by its length
divided by its largest component, then clamps each component to `[-127,127]`.
X is biased by 128; Y is inverted and biased by 128. D-pad directions also
force their corresponding axis to an endpoint.

The game reader at `0x800472ac` converts active-low buttons back to active-high
bits and publishes analog bytes at `0x800b9480` / `0x800b9481`. Its directional
dead zone is `[-30,+30]` around 128. Moderate vertical deflection also produces
the accurate-movement modifier where the original player state allows it.

| N64 input | Engine bit | Native action |
| --- | --- | --- |
| Start | `0x0008` | Pause |
| D-up / stick forward | `0x0010` | Forward |
| D-right / stick right | `0x0020` | Turn right |
| D-down / stick backward | `0x0040` | Backward |
| D-left / stick left | `0x0080` | Turn left |
| R | `0x0100` | Lives / inventory modifier |
| Z / L | `0x0001` / `0x0200` | Use item |
| C-up | `0x0400` | Side dodge left |
| C-right | `0x0800` | Side dodge right |
| C-left | `0x1000` | Jump |
| C-down | `0x2000` | Defend / accurate movement |
| A | `0x4000` | Kick |
| B | `0x8000` | Strike / take item |

The conversion is established by `0x80071ff0..0x80072160`; the player-action
consumer is `0x800052e8`. The ROM's controls strings also describe defend,
jump, kick, strike, rotation, forward/back movement, item use and side jumps.
The cartridge [instruction booklet](https://www.scribd.com/document/824249982/Nightmare-Creatures-USA),
Game Controls and Inventory sections, corroborates the original user-facing
actions. Its OCR is imperfect; the ROM determines the exact bit assignments.
No matching N64 decompilation was identified during this investigation.

## Hook scope and movement order

Three complete-body wrappers preserve the translated originals:

| Function | Role |
| --- | --- |
| `0x80034850` | Establish the lifetime of the main gameplay loop |
| `0x800052e8` | Consume one accumulated host command for the current player |
| `0x800472ac` | Substitute the game-facing actions only within that player call |

The action wrapper only injects for the call at `0x80034b00`, return address
`0x80034b08`, with the current player as `a0`. Its reader call is at
`0x80005364`, return `0x8000536c`. Other pad readers serve menus, pause, demo and
other flows; those retain the original implementation. The null-player call at
`0x80034b28` is not an injection site.

After actions return, the main loop calls animation at `0x800039a0`, movement
at `0x8000bc30`, and collision processing at `0x80019cf4`. The adapter updates
heading after the original action handler and before these calls. It does not
write position, bypass collision or replace animation/combat functions.

The player pointer is stored at `0x800b4b70`. Player `+0x20` points to a transform
whose `+0x48` word holds heading. Left turning adds to this field; right turning
subtracts (`0x80003ab4`). Angle consumers shift it right by 4 and mask to 12 bits
(`0x8001517c`, `0x8005455c`), while the sine lookup at `0x80007424` uses the same
16-unit precision. A full turn is therefore 65,536 heading units.

Each eligible update applies `-mouse_dx * sensitivity * 65536 / 360`, retaining
fractional units and wrapping the result. It does not multiply by frame time or
feed deltas repeatedly through the controller thread. Camera following stays
under cartridge control; vertical mouse delta is intentionally unused.

W/S and combat buttons become native engine bits. The adapter sets analog X to
neutral and Y to the selected forward/back magnitude, so turn acceleration from
the stick is not added to mouse steering. Wheel detents use the original
R-plus-left/right inventory path, clearing only the corresponding previous-input
bits in player `+0x33c` to allow consecutive detents. Native inventory selection
and animations remain authoritative.

## State checks and verification

Capture and injection require the main gameplay loop to be active, valid aligned
player/transform/state pointers inside RDRAM, and the original session checks
at `0x800b7660 + 0x22` and `+0x24` to be nonzero. The state pointer comes from
`0x80097860`. The combined exclusion masks are `0xc8009870` on state word 0 and
`0x03000040` on word 1, derived from the input, frame-control and pause paths.
These exclude pause/menu bit `0x40`, demo/record paths `0x30`, transitions and
other native control locks. Player flags at `+0x30c` must include mask `0x1` and lack
the original input rejection mask `0x01780000`.

These are code-derived eligibility checks, not live confirmation of every
death/cutscene/level transition. Ineligible updates, capture release and gameplay
exit clear pending motion. Controller Pak data and game configuration are not
modified by the adapter.

The single `profile.toml` contains all hook declarations, reviewed function
hashes and exact instruction operands. The shared `tools/input_evidence.py`
checks ROM identity, complete body hashes, discovered boundaries, instruction
ownership, decoded addresses/offsets, call/delay pairs and original translation
coverage. It also serves Quake II. Generated headers and `input-hooks.json`
remain under the chosen build directory.

Static coverage remains 1,207 ready functions, 50 native hooks and the same 10
pre-existing explicit boundaries for unused SDK/RSP internals. All three wrapper
originals are ready. Debug and Release builds are checked; gameplay testing
should cover both characters, mouse direction, camera following, movement and
dodges, attack combinations, jump/block, item use/scrolling, pause and Esc/F4,
focus loss, death/restart and level changes.
