# Ocarina of Time US V1.2 controls

WASD/mouse controls capture automatically during live gameplay. Escape releases
the mouse; F4 recaptures it after release or focus loss. Dialog, pause,
transitions, death and cutscenes release capture and discard pending motion.
Use the normal controller/keyboard bindings outside gameplay (Enter is A).

| Input | Game action |
| --- | --- |
| W / A / S / D | Native analog stick; movement follows the camera |
| Shift + WASD | Half-strength stick for walking |
| Mouse motion | Orbit the standard third-person follow camera |
| Left mouse | B: sword |
| E or Space | A: contextual action |
| Right mouse | Z: target / recenter |
| Ctrl | R: shield |
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

Settings are read at startup:

```powershell
$env:RECOMP_MOUSE_SENSITIVITY = '0.08' # degrees per mouse count; 0.001..10
$env:RECOMP_MOUSE_INVERT_Y = '1'       # optional inverted vertical orbit
$env:RECOMP_OOT_MOUSE = '0'            # optional original controller mapping
```

Debug input replay (`--input`) disables this adapter. FOV works
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

In-game movement, collision geometry, aiming and complete camera transitions
remain for gameplay acceptance.
