# Ocarina of Time US V1.2 controls

WASD/mouse controls capture automatically during live gameplay. Escape releases
the mouse; F4 recaptures it after release or focus loss. Dialog, pause,
transitions, death and cutscenes release capture and discard pending motion.
Outside gameplay, WASD navigates menus and Enter is A. Arrow keys remain
C-buttons when mouse capture is released, including while playing the ocarina.

| Input | Game action |
| --- | --- |
| W / A / S / D | Native analog stick; movement follows the camera |
| Shift + WASD | Half-strength stick for walking |
| Mouse motion | Orbit the standard third-person follow camera |
| Left mouse | B: sword |
| E or Space | A: contextual action |
| Right mouse | Z: target / recenter |
| Ctrl | R: shield |
| Up / Down / Left / Right arrows | C-up / C-down / C-left / C-right, including ocarina notes |
| Q / R / F | C-left / C-down / C-right item |
| Shift + F | C-up |
| Tab | Start |
| Keypad + / - | Increase / decrease horizontal FOV; original minimum, 140 maximum |

The original camera solver chooses the target and distance. Mouse orbit keeps
its heading until capture ends or the camera changes mode, and calls the game's
background collision helper for the new eye position. Z-targeting takes over
the camera. Mouse orbit is limited to the standard `Camera_Normal1` follow
handler: first-person aiming, special follow handlers, fixed backgrounds and
scripted cameras retain native behavior. WASD supplies native stick input in
first-person views; mouse aiming in those modes is not implemented.

## Steam Deck and gamepad camera

The Steam Deck package enables native dual-stick controls. Use Steam Input's
Gamepad template or the bundled v64recomp - N64 template; keep both sticks as
gamepad axes. On a desktop, enable this mode with `RECOMP_OOT_GAMEPAD_CAMERA=1`.

| Input | Game action |
| --- | --- |
| Left stick | Analog movement and menu navigation |
| Right stick | Position the standard third-person follow camera |
| Top-left D-pad | C-up / C-down / C-left / C-right, including ocarina notes |
| A / X | A: contextual action / B: sword |
| Left trigger | Z: target / recenter |
| Right trigger or bumper | R: shield |
| Menu | Start |

Both sticks use a 12% radial deadzone with continuous travel. Full right-stick
deflection turns at 120 degrees/second horizontally or 90 vertically. Camera
speed follows elapsed time, independently of input polling and interpolation.
The existing follow-camera limits, collision handling and Z-targeting takeover
also apply to the stick. Dialog, pause, cutscenes and special cameras keep their
native behavior; first-person aiming still uses the left stick.

This mode disables mouse capture and mouse action remapping so the gamepad's
movement and buttons reach the game intact. Set `RECOMP_OOT_GAMEPAD_CAMERA=0`
to restore the original gamepad mapping. Keyboard controls in gamepad mode use
the original layout (arrows move, I/K/J/L are C-buttons). Desktop WASD/mouse
controls remain the default when this mode is not enabled.

Settings are read at startup:

```powershell
$env:RECOMP_MOUSE_SENSITIVITY = '0.08' # degrees per mouse count; 0.001..10
$env:RECOMP_MOUSE_INVERT_Y = '1'       # optional inverted vertical orbit
$env:RECOMP_OOT_MOUSE = '0'            # optional original controller mapping
$env:RECOMP_OOT_GAMEPAD_CAMERA = '1'  # dual-stick/D-pad mode, default in Deck packages
```

Debug input replay (`--input`) disables mouse and gamepad camera overrides. FOV works
independently of mouse capture; see the [FOV setting](README.md#field-of-view).

## Evidence and verification

The [zeldaret reference](https://github.com/zeldaret/oot/tree/14759abf9abb4295de142cdc1452998630ea70fb)
provided names and layout hypotheses. Addresses, operands, full function bodies
and table fingerprints were checked against this exact NTSC 1.2 ROM. The
single `profile.toml` records the evidence; `controls_analysis.py` verifies it
during generation and emits `controls-hooks.json` in the selected build.
The report describes static evidence, not gameplay acceptance.

`Play_Update` at `0x8009b60c` establishes the live play state and refreshes input
at its verified `Play_Main` caller. `Camera_Update` at `0x80048c4c` uses a hidden
structure-return pointer in `a0`, with `Camera*` in `a1`. Its verified dispatch
calls `Camera_Normal1` at `0x80039054`; only that normal-mode path receives orbit.
The adapter calls `Camera_BGCheck` at `0x800362fc` with a scratch register context
so collision cannot clobber the original handler's return registers.

Debug and Release builds pass. SDL virtual-controller checks cover analog
movement/camera separation, all four D-pad C-buttons, deadzones, action buttons,
legacy mapping and disconnects. Native camera-hook checks cover elapsed-time
rotation, centering, stall limits, collision-call register preservation,
Z-targeting, dialog, special modes and replay isolation.

These checks do not establish physical Steam Deck gameplay acceptance. In-game
movement, collision geometry, aiming and complete camera transitions remain
for gameplay acceptance.
