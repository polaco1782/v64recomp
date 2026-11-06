# Turok US v1.2 keyboard, mouse and FOV

The desktop adapter is enabled by default in Debug and Release.

| Control | Action |
| --- | --- |
| W / S | Native forward / backward movement |
| A / D | Native strafe left / right |
| Mouse | Turn and look vertically |
| Left mouse | Fire, using the native weapon's held/press behavior |
| Space / right mouse | Jump; hold to retain the native jump behavior, or swim up |
| Wheel down / Q | Next weapon |
| Wheel up / R | Previous weapon |
| F | Automap; hold F and use WASD to scroll it |
| Tab | Native Start/pause |
| Enter | Native A/menu confirm |
| Escape / F4 | Release / recapture mouse |
| Keypad + / - | Adjust horizontal FOV from the original approximately 60.8° to 140° |

The original controller movement, acceleration, collision, jump, swimming,
climbing, weapon and recoil code still runs. The adapter supplies digital actions
through the original control-query ABI. Native run/walk settings remain in use.
Mouse motion updates the player's yaw and persistent pitch in radians, with
vertical limits of -78° and +90°. The native climbing camera owns its head angles
while climbing; mouse angle injection yields to it. Ducking remains automatic in
low passages, as in the original game.

Menus, pause, warps, training overlays, attract demos, dead players and cinematic
cameras use native input.
When capture is released, the previous controller remap is retained: arrows/left
stick move forward/back and turn, I/K/right-stick vertical look, J/L/right-stick
horizontal strafe. Debug controller replay disables mouse injection and retains
original N64 pad semantics.

`RECOMP_TUROK_MOUSE=0` disables the desktop adapter. The shared
`RECOMP_MOUSE_SENSITIVITY` setting defaults to `0.08` degrees per count, accepting
`0.001..10`; `RECOMP_MOUSE_INVERT_Y=1` reverses vertical mouse motion.
Keypad FOV adjustment remains independent of capture and these mouse settings.

Each launch starts at the exact original FOV. Keypad adjustments use the shared
FOV control and preserve native zoom/water distortion with tangent scaling.
Lowering to the minimum takes the unmodified projection/visibility path.
RT64's F7 aspect expansion remains a separate setting.

## Retail evidence and timing

All addresses target the normalized US v1.2 ROM SHA-1
`c7ed00dee20f4235823bcf50d32fa5d6862d6fce`. The program is RNC-decoded before
analysis; `.main` maps analysis offset `0x800000` to VRAM `0x80001300`.
Complete reviewed bodies and decoded instruction operands are recorded under
`controls` in `profile.toml`, with six original-preserving wrappers at top level.
`generate.py` imports `controls_analysis.py` to check their fingerprints,
metadata ownership, translated coverage and call/delay pairs. It emits
`turok_controls_config.h` and `controls-hooks.json` in the chosen build output.
The report describes static evidence and keeps `runtime_verified=false`.

The [pinned source reference](https://github.com/Abhisheksinha1506/Turok-N64-Source-code/tree/f6ac62a9b9fc799bdb023dddaa440480814f9af6/src/PR/tengine)
supplied semantic hypotheses from `control.c`, `tcontrol.c`, `tmove.c`,
`camera.c` and `tengine.c`. Retail addresses, fields and function bodies were
checked independently; this reference is not a build dependency.

- `CEngineApp__UpdateGAME` at `0x8000a044` scopes one game update. Its call to
  `ProcessPlayerController` at `0x8000aa54` latches one mouse/keyboard command.
  SDL polling runs on the owner thread through `recomp_on_host`, before the
  original controller processing. Motion/wheel detents are consumed once;
  held input and edges remain consistent through movement and later weapon AI.
- `CTMove__UpdateTurokInstance` at `0x80052c6c`, return `0x8000aae0`, receives
  movement `0x80128cc0`, engine `0x800f9fb0` and controls `0x80128aa0`.
  Native action queries call `CTControl__Action` at `0x80051fb8`; checked query
  return PCs select the host action. Calls outside the live game update retain
  their original results, including menu/pause queries.
- The engine player pointer is at `+0x810c`; health is signed 16-bit at actor
  `+0x1dc`, and yaw is float at actor `+0x64`. The movement pitch/target are
  `+0x70/+0x68`. The pitch `NormalizeRotation` call at `0x80053818` is before
  the engine copies the angle and before later recoil/head-bob additions.
  The hook changes this result before camera, weapon and movement consumers.
- `CCamera__Update` at `0x80037f3c` builds the world camera. Its
  `guPerspectiveF` call at `0x80038684` passes FOV/aspect as `a2/a3` bits.
  Original vertical FOV is 47.5°, with a 320/240 reference aspect. The hook
  requires the normal eye camera, live player and exact matrix/norm arguments.
- The float projection reaches `engine + 0x1f794` through the multiplies at
  `0x800386a8/0x800386bc`; `0x800386d8` converts it for the GPU.
  `CEngineApp__GetScreenCoordinates` at `0x8000ae64` consumes the same float
  matrix. World particles use oriented 3D vertices with that projection.
- CPU clipping uses a separate five-corner volume. Camera stack locals
  `+0xb0/+0xac` hold FOV/aspect; `0x80038a1c..0x80038b08` derives the corners
  and animation radius. At the projection hook, the saved CPU angle is widened
  to cover the actual adjusted horizontal projection plus two degrees, retaining
  any larger native overscan and limiting the span to 178°. The original code
  then transforms the corners (`0x80038b18`), builds the volume
  (`0x80038b74`) and encloses its X/Z bounds (`0x80038b88`). Recursive visibility,
  allocations and clipping tests remain native.

## Validation boundary

ROM evidence, metadata and source generation were checked during implementation.
Debug/Release builds and runtime tests were deliberately left to the user.
Mouse direction/sensitivity, sustained/press fire, bow release, swimming,
climbing, pause/capture return, edge visibility, zoom, FOV reset and combined F7
behavior still need gameplay acceptance.
