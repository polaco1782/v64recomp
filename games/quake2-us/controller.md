# Quake II controller and mouse adapter

These findings apply to **Quake II US revision 0**, normalized SHA-1
`2edb00e602e7c2813a5d6a04dfec80487627237a`. Addresses and action names come from
the cartridge's MIPS code and strings. They are statically verified; live mouse
timing, state transitions and gameplay acceptance remain for user testing.
The N64 engine differs from the PC version, so PC source layouts were not used.

## Player controls

The adapter is enabled by default in Debug and Release for one active player.

| Host input | Action |
| --- | --- |
| Mouse | Turn and look, without stick acceleration or automatic pitch centering |
| W / S | Forward / backward |
| A / D | Strafe left / right |
| Left mouse | Fire |
| Space or right mouse | Jump / positive vertical movement |
| Shift | Toggle speed while held: walk with the default autorun setting |
| Wheel up / down | Previous / next weapon, one selection per detent |
| Q / E or R | Next / previous weapon |
| F | The cartridge's `button status` action |
| Tab | Start/pause |
| Arrow keys, Enter, E | Menu directions, A/confirm, B/back |
| Esc | Release or recapture mouse |
| F4 | Recapture mouse |
| F10 | Quit |

Mouse capture releases for menus, focus loss, the inspector/cheat menu, death
and intermission. Death uses the original controller actions; Enter sends A
and provides an action edge for the cartridge's respawn handling. Physical
controllers use the original bindings when capture is released. While captured,
the adapter owns movement, view and gameplay actions; controller Start remains
available. Multiplayer and demo playback retain the original input path.

Settings:

- `RECOMP_QUAKE2_MOUSE=0` restores the ordinary controller/keyboard mapping.
- `RECOMP_MOUSE_SENSITIVITY` is degrees per mouse count, default `0.08`, range
  `0.001..10`; the setting is shared with GoldenEye.
- `RECOMP_MOUSE_INVERT_Y=1` inverts vertical mouse movement.
- Debug `RECOMP_QUAKE2_INPUT` controller replay disables the mouse adapter.

The checked action table has no separate crouch, reload or use action. R is
previous weapon here. Controller configuration and Controller Pak saves are
not rewritten by the adapter.

## Controller path and command timing

| Entry / site | Verified role |
| --- | --- |
| `0x80000d88` | Packed controller getter; `a0` is the controller index |
| `0x80024dd0` | Gameplay action dispatcher; `a0` is a client pointer |
| `0x8002b844` / return `0x8002b84c` | Player update calls that dispatcher with entity `+0x68` |
| `0x80024a10` | Builds the player command; `a0 = client + 0x100` |
| `0x8002511c` / return `0x80025124` | Dispatcher calls the command builder |
| `0x8002b8f8` | Player update calls movement at `0x800226e8` after command construction |
| `0x8002253c` | Movement normalizes yaw and clamps pitch to `[-89,89]` degrees |

The packed getter loads a controller record through global `0x80085a14`, stride
6, and returns `(buttons << 16) | packed_axes`. X occupies bits 15..8 and Y bits
7..0, each a signed byte. Menu callers also use this getter, so it is deliberately
left unwrapped.

Two full-body-checked wrappers scope injection to the first client's command
inside the player update. They always call the translated original. The command
wrapper saves the preceding view angles, calls the original builder, then
replaces view, movement and action fields before movement/collision/weapon code
runs. Neither host frame polling nor arbitrary pad reads consume mouse motion.

The shared mouse buffer accumulates deltas across host presentations and consumes
them once per eligible guest command. Held movement/actions survive subsequent
commands; wheel detents are bounded to 32 pending and delivered individually with
a fresh weapon-action edge. Mouse angles are **not multiplied by delta time**.
Yaw decreases with rightward mouse motion, and pitch increases with downward
motion. The original controller turns by `dt * 200` degrees times action input
and pitches by `dt * 100`, with `dt` at `0x80085940`; the speed modifier multiplies
the view time scale by 1.5. Actual host/guest update ratios have not been measured.

## Bindings and action IDs

The mutable binding table is `0x800779e0`, stride `0x44` (17 action IDs) per
controller. Its ROM default at `0x800657ac` is:

```text
1, 2, 3, 4, 1, 2, 3, 4, 13, 13, 15, 16, 14, 8, 7, 5, 6
```

Slots 0..12 use masks at `0x80065730` in this order: D-up/down/left/right,
C-up/down/left/right, L, R, A, B, Z. Thus both directional button groups move
and strafe, both shoulders jump, A advances weapons, B selects the previous
weapon, and Z fires. Start is handled separately as a pause edge.

Slots 13..16 are Y-positive, Y-negative, X-negative, X-positive: look down, look
up, turn left, turn right. Axes have a dead zone of `[-4,+4]`; outside it the
action magnitude is `((abs(axis)-4)*scale)^2`. Default X/Y scale is `1/60`, stored
per player at `0x80085a78` / `0x800a7d1c`. Movement actions clamp magnitude to 1;
look actions retain the squared magnitude. Digital buttons pass magnitude 1.

The function table at `0x80065764` contains exactly 18 handlers. Three checked
indirect calls use it: `0x80024fc8`, `0x8002506c`, `0x800250f0`. ROM name pointers
at `0x8006455c` identify the following actions:

| ID | ROM action | Handler | Output |
| --- | --- | --- | --- |
| 0 | empty | `0x80025154` | No action |
| 1 | walk forward | `0x800251e4` | Forward magnitude at `0x800765e8` |
| 2 | backpedal | `0x80025210` | Backward magnitude at `0x80077af0` |
| 3 | left step | `0x8002518c` | Left magnitude at `0x8007a428` |
| 4 | right step | `0x800251b8` | Right magnitude at `0x80087ae4` |
| 5 | left turn | `0x80025174` | Left turn at `0x800859ac` |
| 6 | right turn | `0x80025180` | Right turn at `0x8007a650` |
| 7 | look up | `0x8002515c` | Up look at `0x8008ab24` |
| 8 | look down | `0x80025168` | Down look at `0x80076544` |
| 9 | center view | `0x800252d8` | Flags OR `0x20` |
| 10 | button look | `0x800252a8` | Flags OR `0x80` |
| 11 | button strafe | `0x800252c0` | Flags OR `0x100` |
| 12 | speed | `0x800252f0` | Flags OR `0x40` |
| 13 | jump | `0x8002523c` | Vertical magnitude at `0x8008e7d8` |
| 14 | attack | `0x80025248` | Flags OR `0x1` |
| 15 | next weapon | `0x80025278` | Flags OR `0x4` |
| 16 | previous weapon | `0x80025290` | Flags OR `0x8` |
| 17 | button status | `0x80025308` | Flags OR `0x10` |

The dispatcher clears action magnitudes and flags (`0x80085918`) each update.
It additionally sets flag `0x80000000` when any digital controller button is
held (`0x80025100..0x80025118`); the adapter reproduces this marker for host
movement keys, jump, action keys and wheel selections.
Movement starts at 200 units times action magnitude. All three movement
components double with autorun XOR the speed modifier. Autorun defaults to 1
in the per-player setting at `0x800777a4`; its current-player copy is
`0x8008e440`. The adapter respects that setting, without changing stored bindings.

## Command layout and state gates

The client stride is `0x364`. The command begins at client `+0x100`:

| Command offset | Type / meaning |
| --- | --- |
| `+0x00`, `+0x04`, `+0x08` | Float pitch, yaw, roll |
| `+0x0c`, `+0x10`, `+0x14` | Signed 32-bit forward, side, up movement |
| `+0x18`, `+0x1c` | Float pitch-centering rate and timer |
| `+0x20`, `+0x24`, `+0x28` | Held, previous and newly pressed action flags |
| `+0x2c` | Previous packed controller word, written by dispatcher |

The adapter clears centering rate/timer and computes newly pressed actions as
`held & ~previous`. The original movement result copies its final angles back
to client `+0x100..0x108`, so the next mouse update starts from the engine's
accepted orientation.

Injection and capture require all of these conditions:

- Phase `0x80085988 == 1`, the gameplay loop. Start sets phase 2 and bypasses
  command construction; the wrapper clears pending mouse motion on this path.
- Mode `0x8008ab1c != 2`; mode 2 substitutes recorded demo controller input.
- Player count `0x8008d410 == 1` and intermission `0x8008e890 == 0`.
- Client-array pointer `0x80087b18` and entity-array pointer `0x800886ec` are
  aligned KSEG0 addresses with complete spans inside RDRAM.
- Entity zero is the world; entity one is at array base `+0x224`. Its `+0x68`
  client pointer must equal the first client. Entity flags `+0x194` must have
  active bit 1 and lack death bit `0x40`; movement flags `+0x1f8` must lack
  `0x2000`, which selects the alternate movement type 3. The adapter does not
  assign an unverified gameplay name to that alternate state.

`profile.toml` stores complete reviewed body hashes, data fingerprints, both
wrapper declarations and exact operand/call evidence. `generate.py` verifies
ROM identity, discovered boundaries, instruction ownership, decoded globals and
field offsets, call/delay pairs and translated-original coverage. It emits
`quake2_input_config.h` and `input-hooks.json` only under the selected build tree.
The action function table stays in `[discovery].function_tables`.

## Validation boundary

Static generation passes with 1,274 ready functions and 59 SDK hooks, no
unsupported functions. Both wrapper originals remain translated. Debug and
Release compile and link with the shared renderer and input transport.

The user reported that the adapter works great in gameplay. That confirms their
overall experience; individual transitions were not separately reported.
Remaining targeted checks include pause/resume, Esc/F4 recapture, focus loss,
death/respawn, level changes and controller fallback with capture released or
`RECOMP_QUAKE2_MOUSE=0`.
