# Duke Nukem 64 US mouse/keyboard adapter

The adapter targets US revision 0, cartridge SHA-1
`98d6778004becc672eba0a5e887f6e3f3d1b5c15`. It is enabled by default in Debug
and Release and uses the shared renderer's relative mouse/capture transport.

## Controls

| Input | Action |
| --- | --- |
| W / S | Native forward / backward movement |
| A / D | Native strafe left / right |
| Mouse | Turn and free-look |
| Left mouse | Fire |
| Space | Jump |
| Ctrl / right mouse | Crouch |
| E | Open / use |
| Wheel down / Q | Next weapon |
| Wheel up / R | Previous weapon |
| F | Use selected inventory item |
| Tab | Start / pause menu |
| Arrows / Enter / E | Menu navigation / N64 A / N64 B |
| Esc / F4 | Release capture / recapture |
| F10 | Quit |

WASD uses the original digital movement speed. Inventory selection and other
unmapped native controls remain available with capture released and a controller.
While captured, mouse/keyboard own gameplay input and preserve Start from the
shared controller state. Focus loss and overlays release capture automatically.

Set `RECOMP_DUKE_MOUSE=0` to restore the ordinary keyboard/controller mapping.
`RECOMP_MOUSE_SENSITIVITY` defaults to `0.08` degrees per mouse count, range
`0.001..10`; `RECOMP_MOUSE_INVERT_Y=1` reverses vertical look. These apply at launch.
Keypad `+` / `-` changes FOV independently of mouse capture.

Capture requires game state 2, no inactive/demo flag, one local player, no pause
or overhead map, positive player health, no fist/exit sequence and no remote
camera owner. These state gates prevent capture in menus, demos and death.
Debug `--input` controller replay disables the adapter.

## Native command boundary

`getinput` at `0x800a2754` builds `loc` at `0x802aac18`. The gameplay ticker at
`0x800998d4` copies it to `sync[0]` (`0x802a5600`) before calling `processinput`
at `0x800a34e0`. The command is twelve bytes:

| Offset | Guest format | Meaning |
| --- | --- | --- |
| +0 | Signed byte | Analog angular velocity |
| +1 | Signed byte | Analog vertical look |
| +2 / +4 | Signed halfword | World-space X / Y movement |
| +8 | Word | Native action bits |

The command hook is restricted to player zero and return PC `0x80099b40`.
Immediately before that guest update it polls SDL on the host owner thread with
`recomp_on_host`, including updates serviced during pacing. It consumes each
mouse delta once; held WASD/actions survive subsequent consumes. Inactive input
clears pending motion, wheel detents and fractional angles.

The adapter updates the native 2048-unit yaw at player `+0x282`. Vertical look is
stored at `+0x220` as a horizon value. The camera interprets its angle as
`atan((horizon - 100) / 128)`; the native range `[-28,228]` corresponds to
approximately `[-45,45]` degrees. Fractional motion is retained between updates.

For command construction it temporarily supplies a simple 16-word control map
at `0x801c93d0`, digital WASD/actions and zero analog-stick input. The original
`getinput` body runs once and performs its own sine-table rotation, movement,
friction addition, button mapping and clock update. The adapter restores the
selected map, cached pad/sticks, working control values and aim-mode setting
afterwards. It does not move player coordinates or replace collision/physics.

The process hook is restricted to return PC `0x80099ba0` and an adapted command
from that update. It temporarily selects native free-look, clears centering and
zeroes analog vertical input to avoid controller snapping/drift. The original
player processing still handles movement, collision, weapons, hard landing,
recoil and other effects. Aim mode is restored after the original returns.
Wheel detents receive a release update between cycles so adjacent detents form
separate native weapon-selection edges.

## How the boundary was found

The semantic reference is [NBlood/Duke64-RE at the reviewed revision](https://github.com/NBlood/Duke64-RE/tree/7ec429c4ba37245da5a5671a019e911b22ff318c).
Its `src/player1.c` defines `getinput`, `BUTTON` and `processinput`;
`include/duke3d.h` describes the input/control-map fields, and `src/game3.c`
shows the ticker copying `loc` to `sync` before player processing. These supplied
hypotheses; local disassembly verified bodies, arguments, globals and offsets.
The reference's `processinput` span differs by padding from local discovery;
the profile uses the actual discovered ROM span and full SHA-256.

Repeat the investigation with maintained tools, using this build's verified
metadata rather than an arbitrary external map:

```powershell
.venv/Scripts/python.exe tools/disassemble_function.py recomp_s0_800a2754 --game duke-us --metadata build/msvc/duke-us/debug/generated/duke-us/symbols.json --arch mips
.venv/Scripts/python.exe tools/inspect_callbacks.py --game duke-us --metadata build/msvc/duke-us/debug/generated/duke-us/symbols.json --target 0x800a2754 --target 0x800a34e0 --before 10 --after 8
.venv/Scripts/python.exe tools/disassemble_function.py recomp_s0_800998d4 --game duke-us --metadata build/msvc/duke-us/debug/generated/duke-us/symbols.json --arch mips
.venv/Scripts/python.exe tools/disassemble_function.py recomp_s0_800a34e0 --game duke-us --metadata build/msvc/duke-us/debug/generated/duke-us/symbols.json --arch mips
```

Check the call delay slots and the copy at `0x80099b44..0x80099b74`. Follow the
2048-unit yaw through `getinput`'s sine-table accesses and the horizon through
`processinput`'s free-look and clamp paths. Decode signed low-half addresses:
the player count is `0x800ff624`, not `0x8010f624`.

`profile.toml` contains full function fingerprints and checked operands under
`input_adapter`; `generate.py` runs the shared `tools/input_evidence.py` verifier,
emits `duke_input_config.h` and writes `input-hooks.json` in the selected build.
These establish static evidence and translated coverage, not gameplay acceptance.

## Gameplay acceptance

The user subsequently confirmed that the input and
FOV changes work well in game. Broader level progression, sustained audio and
less common controller/camera transitions remain unverified.
