# Quake 64 US controls and camera

Default Debug/Release controls for normal single-player gameplay:

| Input | Action while captured |
| --- | --- |
| Mouse | Turn/look without stick acceleration or automatic pitch centering |
| W / S | Forward / backward |
| A / D | Strafe left / right |
| Left mouse | Fire |
| Space or right mouse | Jump |
| Shift | Invert the first player's autorun setting while held |
| Wheel up / down | Previous / next weapon, one selection per detent |
| Q / E or R | Next / previous weapon |
| Tab | Start/pause |
| Keypad + / - | Horizontal FOV, original 90 degrees through 140 |
| Escape / F4 | Release / recapture mouse |
| F10 | Quit |

Capture starts automatically in live gameplay. Focus loss, pause, intermission,
death and inactive gameplay discard queued motion. Menus retain arrow keys,
Enter/A and E/B. Physical controllers retain native bindings with capture
released; Start remains native while captured. Multiplayer retains original
input/camera behavior. Controller bindings and Pak data are not rewritten.
The reviewed table has no separate crouch, reload, use or status action.

Settings at launch: `RECOMP_QUAKE_MOUSE=0` restores the original mapping;
`RECOMP_MOUSE_SENSITIVITY` is degrees/count, default `0.08`, range `0.001..10`;
`RECOMP_MOUSE_INVERT_Y=1` reverses vertical look. Debug `RECOMP_QUAKE_INPUT`
replay disables mouse injection. FOV has no environment lookup: keypad presses
change one degree, holding repeats, and each launch starts at the original view.
The console prints changes. FOV works independently of mouse capture.

## Input evidence

These findings apply to US revision 0, normalized SHA-1
`25a7fe0342490d3c23328040bb34f8748995537a`. The
[pinned PC Quake source](https://github.com/id-Software/Quake/tree/bf4ac424ce754894ac8f1dae6a3981954bc9852d/WinQuake)
was a structural reference, not matching N64 source. Addresses and layouts were
derived independently from the cartridge's MIPS.

| Boundary | Verified role |
| --- | --- |
| `0x800469d8` | Native action dispatcher; gameplay caller returns to `0x8003b528` |
| `0x80046808` | Original movement follows command construction |
| `0x80086ec8` / `0x8008e720` | Current client pointer / first client; stride `0xa8`, controller index `+4`, entity pointer `+8` |
| Client `+0x0c` | Command: yaw/pitch deltas `+0/+4`, forward/side/up floats `+8/+0xc/+0x10`, flags `+0x14`, previous flags `+0x18` |
| `0x8008e880` | Current entity; view pitch/yaw `+0xcc/+0xd0`, health float `+0x1c0`, action bits `+0x27c` |
| Entity `+0x29e` | Angle-reset flag; start from model angles when set |
| `0x8006d3e0` | Complete 16-entry action table; all five dispatch sites are declared in the profile |

The dispatcher executes once, then injection supplies native movement and action
fields before original collision, physics and weapon processing. Input is polled
on the owner thread at the eligible guest command, as well as in the outer
presentation loop. Deltas are consumed once without delta-time scaling. Pitch
uses native bounds `[-70,+80]`; yaw wraps at 360 degrees.

Base movement is 200 forward/back and 250 strafe, with a 2x run multiplier,
selected by autorun XOR Shift. Bits are attack `1`, jump `4`, next weapon `8`
and previous weapon `16`; held latches `0x100`/`0x200` prevent repeats. Wheel
detents force fresh weapon edges. Entity actions receive `flags & 0x1f`.

Capture/injection require running `0x8006caa8 == 1`, phase `0x8006caa4 == 0`,
one player at `0x80086ecc`, no intermission at
`0x800774c8`, controller zero and an aligned live entity with a nonzero movement
type and positive health. Injection also requires the gameplay return PC and
matching current client/entity pointers. Pause restores running 1/phase 0 on
resume.

`0x8006caa0` is the difficulty selector, initialized to 1 (Normal), rather than
a gameplay-mode flag. Its menu choices are Easy, Normal, Hard and Nightmare.
The initial adapter incorrectly required this value to be zero, preventing
capture, command injection and FOV activation on Normal and higher settings.
Difficulty is now excluded from these gates; its ROM address/default remain
recorded as evidence to prevent reintroducing the mistake.

## Projection and CPU visibility

`guFrustum` at `0x800574f0` receives bounds `-4/+4`, `-3/+3`, near 4 and far
4096 from `0x800284ac`: horizontal 90 degrees, vertical about 73.74 at 4:3.
The adapter saves the fixed matrix at `0x80078160`, scales only its X/Y diagonal
terms by `1/tan(horizontal/2)`, and restores all original bytes at the minimum.
Depth and native view transformations remain intact. The render wrapper at
`0x80028378` selects the eligible gameplay matrix and leaves it in RAM because
the later RSP task reads its address. Ineligible views select the original.

CPU routine `0x80027378` builds four planes from forward `0x800781f0`, right
`0x80078200` and up `0x800781e0`. Temporarily reducing right/up by inverse
tangent factors widens those planes with one degree of margin; the native
vertical conservative span is never narrowed. Original distances, types and
sign bits remain guest code; all six basis words are restored after the call.
Box culling at `0x800260f0` consumes these planes. `0x800276b0` submits the GPU
matrix. Effects at `0x8002db24` emit 3D vertices, so no Duke-style independent
screen-size correction is applied. The 3D weapon view follows the projection.

The single `profile.toml` stores full body hashes and decoded operands.
`controls_analysis.py` verifies them during generation; `generate.py` emits
`quake_controls_config.h` and `controls-hooks.json`, with `runtime_verified=false`.
ROM discovery/translation and configuration emission passed. No standalone tests
were added. MSVC Debug compiles and links; Release has not been rebuilt and the
game has not been launched. Movement feel, recapture,
death/respawn, weapon cycling, level transitions, edge visibility and combined
F7 widescreen behavior await gameplay acceptance. F7 expansion is separate from
the 4:3 reference FOV.
