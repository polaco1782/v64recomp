# Duke Nukem 64 US controls

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

## Steam Deck and dual-stick gamepads

The Deck launcher sets `RECOMP_DUKE_MOUSE=0` and `RECOMP_DUKE_GAMEPAD=1`.
The left stick walks forward/backward and strafes with proportional speed, from
gentle movement to the native full digital movement speed. The shared 12% radial
deadzone preserves diagonal direction. The right stick turns and looks up/down
through Duke's native analog/free-look path. Face buttons, triggers, bumpers and
D-pad C-buttons retain the game's selected action bindings. Menus, pause, maps,
demos, death and multiplayer use the original controller path.

The generic N64 layout alone puts looking on the left stick and digital walking
on the right because that is Duke's native control scheme. The dual-stick mode
separates these at the checked single-player command boundary. It temporarily
selects free-look and supplies right-stick axes, restoring the saved aim setting,
cached controller values and working controls after the original call.

At the first `mulscale9` call from `getinput` (return `0x800a2d04`), a scoped
wrapper adds left-stick travel to the native forward/side velocities before the
original sine-table rotation. Full stick supplies 80 velocity units; the native
90-unit component limits still apply when combined with digital movement. The
ROM retains world-space rotation, friction contributions and all player physics.
Other callers of the fixed-point helper are unchanged. Generation checks its
complete body and the caller/global operands alongside the existing evidence.

Set `RECOMP_DUKE_GAMEPAD=0` to restore the original N64 stick assignment. Debug
controller replay disables both live adapters and preserves original pad
semantics. Active mouse capture takes priority over dual-stick input.

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
.venv/Scripts/python.exe tools/analyze.py disassemble recomp_s0_800a2754 --game duke-us --metadata build/msvc/duke-us/debug/generated/duke-us/symbols.json --arch mips
.venv/Scripts/python.exe tools/analyze.py callbacks --game duke-us --metadata build/msvc/duke-us/debug/generated/duke-us/symbols.json --target 0x800a2754 --target 0x800a34e0 --before 10 --after 8
.venv/Scripts/python.exe tools/analyze.py disassemble recomp_s0_800998d4 --game duke-us --metadata build/msvc/duke-us/debug/generated/duke-us/symbols.json --arch mips
.venv/Scripts/python.exe tools/analyze.py disassemble recomp_s0_800a34e0 --game duke-us --metadata build/msvc/duke-us/debug/generated/duke-us/symbols.json --arch mips
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

The user confirmed the earlier desktop input and FOV changes work well in game.
The November 12 dual-stick change passes generation, Debug/Release builds and
716 focused ROM-backed checks in each mode. These exercise progressive forward,
backward and strafe commands, separate right-stick look, native rotation/friction,
saved controls, pause bypass and unrelated fixed-point calls. SDL virtual-stick
travel of 0/25/50/75/100% produces movement commands 0/384/1120/1824/2560 after
the shared radial deadzone.

Rendered checks reached the title/menu for 2,400 Debug and 3,300 Release
presentations; automated navigation did not establish gameplay movement. The
installed Deck Release passes a 180-presentation startup with a temporary save,
and the user's save checksum is unchanged. Physical Deck stick feel and gameplay
acceptance remain pending. Temporary probes/replays/captures were removed.
Broader progression, sustained audio and uncommon controller/camera transitions
remain unverified.
