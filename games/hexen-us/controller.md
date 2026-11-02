# Hexen US input adapter

The adapter targets revision 0, normalized cartridge SHA-1
`a602839132f6cca71f175fb72039897705ba4661`. Input evidence is checked after
LZH preparation verifies the decoded program SHA-256. The original cartridge
identity remains separate from the prepared analysis image.

## Controls

| Input | Action |
| --- | --- |
| W / S | Forward / backward |
| A / D | Strafe left / right |
| Mouse | Horizontal turn and native vertical look |
| Shift | Run at the class's native speed |
| Left mouse | Attack |
| E | Use door / switch |
| Space | Jump |
| Wheel down / Q | Next owned weapon |
| Wheel up / R | Previous owned weapon |
| Right mouse | Use selected artifact, once per click |
| F | Toggle native automap with the default Control Stick scheme |
| Tab | Start / menu |
| Arrows / Enter / E | Menu directions / N64 A / N64 B |
| Esc / F4 | Release or toggle capture / recapture |
| F10 | Quit |

Mouse capture requires a live single-player level, a valid player actor, no
pending game action, no demo, and no menu/pause. Focus loss and overlays release
capture through the shared renderer. Inactive input clears pending motion,
wheel detents and key edges. While captured, mouse/keyboard own gameplay input
and retain Start from a physical controller. Release capture to use a controller.

`RECOMP_HEXEN_MOUSE=0` disables the adapter and restores the ordinary shared
keyboard map. `RECOMP_MOUSE_SENSITIVITY` defaults to `0.08` and accepts
`0.001..10`: horizontal degrees per mouse count and vertical native look units
per count. `RECOMP_MOUSE_INVERT_Y=1` inverts vertical look. Native pitch remains
clamped to `[-110,90]`; fractional motion is retained between updates.
Debug `--input` controller replay disables the adapter.
Inventory selection and automap zoom retain their original controller behavior
with capture released; the wheel is assigned to weapons while captured.

## Verified command boundary

`0x800d7740` builds commands through `0x800d8860`. Commands occupy twelve
eight-byte slots per player at `0x8013d8b0`. The game ticker at `0x800d9504`
copies the current slot into `players[i]+6`; the player array begins at
`0x8013da88`, with stride `0x19c`. The adapter wraps the full original
`P_PlayerThink` body at `0x80106f04`, only for player zero and return PC
`0x80104edc` from the play ticker at `0x80104e80`.

It polls live SDL input on the owner thread immediately before each guest
player update, including updates serviced during pacing. It then updates the
current command and calls the original body once. Movement, collision, damage,
weapon ownership, use handling, jumping and artifact effects remain guest code.
It does not consume mouse motion on the controller polling thread.

| Command offset | Format |
| --- | --- |
| `+0` / `+1` | Signed byte forward / side movement |
| `+2` | Signed halfword angle turn; guest adds it to actor yaw shifted by 16 |
| `+4` | Attack `1`, use `2`, next weapon `0x0c`, previous weapon `0x14` |
| `+5` | Packed native look/fly; captured input clears it |
| `+6` | Selected artifact ID in low six bits; jump `0x80` |

The native mover at `0x80106284` consumes these fields. Player state is at
`+4`, class at `+0x0e`, look direction at `+0x22`, centering at `+0x24`, and
selected artifact at `+0xbc`. The class/speed movement tables are
`0x80120f10` (forward) and `0x80120f80` (side); the speed power at `+0xd0`
multiplies movement by 3/2 unless morphed (`+0x194`). Weapon key presses and
artifact clicks are edges; the shared wheel buffer delivers one queued detent
per guest update. F is remapped to native C-Up rather than the shared FPS pad
bits, so automap events still go through the original responder.

`profile.toml` records full-body and table fingerprints plus instruction words
for each accessed global/field. Generation writes `hexen_input_config.h` and
`input-hooks.json`. The latter reports static verification with
`runtime_verified=false`; generation never claims gameplay acceptance.

[Chocolate Doom's Hexen player code](https://github.com/chocolate-doom/chocolate-doom/blob/master/src/hexen/p_user.c)
and [game code](https://github.com/chocolate-doom/chocolate-doom/blob/master/src/hexen/g_game.c)
were used as structural references. They are PC engine code, not matching N64
source; the addresses, field widths, limits, tables and command semantics above
were checked independently against this cartridge's MIPS.

## Validation

Earlier MSVC Debug and Release builds passed.

Release boot and rendered attract mode were observed. Live adapter gameplay,
camera feel, pause/focus recapture, class selection, and artifact/weapon behavior
remain for the user's in-game test. Earlier controller acceptance in the port
README predates this adapter.
