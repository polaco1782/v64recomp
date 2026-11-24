# Turok 3: Shadow of Oblivion

This port targets the North American retail Nintendo 64 cartridge, revision 0,
with an Expansion Pak and desktop RT64 or GLES2. The normalized 32 MiB ROM has SHA-1
`a2e0a28dc0d3a48c7c02cb9e4c4b38fe04b2d436`, internal title
`Turok 3: Shadow of O`, cartridge ID `NTKE` and entry `0x80000400`.

## Build and run

From the repository root, import your cartridge dump, then build and launch.

```sh
python3 tools/import_rom.py --profile games/turok3-us/profile.toml --rom "/path/to/Turok 3.z64"
python3 tools/build.py turok3-us --debug
./build/turok3-us-debug/bin/turok3-us --input games/turok3-us/new-game.txt --frames 2400 --capture-every 150
python3 tools/build.py turok3-us --release
./build/turok3-us-release/bin/turok3-us
```

Controller port 1 has a Controller Pak at `local/saves/turok3-us.mpk`.

## Runtime coverage

Debug and Release reached first-person play in **Chapter 1, The Breach**.
The 2,400-frame Debug replay passes the Controller Pak and Expansion Pak
screens, opens New Game, selects Danielle, and continues through the opening
scene. At frame 1,950 the chapter title and HUD were visible. Later frames
show the player turning and moving across the room with the tomahawk equipped.
The run exited normally after the first objective appeared. To stop at the
main menu, run the same replay with `--frames 905`.

An 800-frame desktop run recorded 275,264 stereo PCM frames at 22,047 Hz,
including 43,601 frames with different left and right samples. Later missions,
multiplayer and save-file round trips still need gameplay testing.

Generation writes `level-events.json` beside `symbols.json`. It lists both link
groups for every cartridge map, including the original row order, link keys,
destination maps and type/action selectors. The US cartridge has 2,685 link
records across 126 maps and uses 54 distinct type/action pairs. Of those
records, 2,634 target their own map and 51 target another map.

The original scene loader takes slot 5 from the cartridge directory and loads
slot 7 from the selected map. Its callback at `0x449ad4` decompresses the link
packet and pairs its two key arrays with two arrays of 24-byte event records.
The dispatcher at `0x269ab0` uses bytes 21 and 22 as the bank and action,
then compares the record's destination with the currently loaded map to choose
the current-map or remote-map function. The report records both function
addresses, including null slots. Generation checks every non-null address for
these selectors against the profile's dispatch table and CPU coverage.
Type 13/action 1, for example, selects a local actor-flag handler at
`0x26a2d4` in Unexpected Help and other maps. These counts describe events
stored on the cartridge. Gameplay verification covers the route above.

The profile also covers the derived actor state records for IDs 4 and 5 that
override the parent state bank.
Pickup handling searches bounded arrays by item ID; discovery records the caller,
record count, stride and slot for those functions in `symbols.json`.

## Desktop controls and field of view

Append `--gles` to either build workflow to select the alternative renderer.
A 2,400-presentation Linux Debug GLES2 replay completed and rendered the
Controller Pak prompt, title and opening cutscene. That run did not establish
first-person input, live FOV or later gameplay on GLES2.

WASD keeps the cartridge's C-button movement on foot and while climbing nets or ladders.
Move the mouse to look around, including relative to the wall while climbing.
Left-click fires, Space or right-click jumps, Q and R select
weapons, and F operates the weapon scope. Tab pauses while the mouse is captured.
In menus, Tab or S presses Start, Enter or A confirms, and E or X goes back.
Escape releases the mouse and F4 captures it again. Menus and physical
controllers use the standard N64 bindings. Set `RECOMP_TUROK_MOUSE=0` for those
bindings throughout, `RECOMP_MOUSE_SENSITIVITY` for degrees per count (default
0.08), or `RECOMP_MOUSE_INVERT_Y=1` to invert vertical look. Debug controller
replays retain the recorded pad values.

Keypad + and − adjust horizontal FOV in one-degree steps from the first on-foot
camera's original angle to 140°. The change preserves the guest's focal-length
ratio during area transitions and zoom. The world matrix, object tests, room
planes and bounds use the expanded view. The separate weapon projection keeps
its original framing.

The local ROM confirms the on-foot look at `0x002cf978`, world camera at
`0x0028ea30`, and their caller-specific input and projection boundaries.
The generator checks those function bodies and instructions against the profile
and records the results in `input-hooks.json`. In The Breach, both desktop Debug and Release
runs increased the horizontal FOV from 83.7055° to 123.7055° and
restored it with the keypad. The guest camera matrix returned to its original
values. Mouse motion changed yaw and pitch, and each of W, A, S and D moved
the first-person view. An isolated run of the generated guest routines also
checked 140° projection and bounded pitch. [Retail recompilation reference](https://github.com/n64hub/turok-3-recomp/blob/ec36469d9e5bd7440d48b7c0c8da47a083ed313c/us.toml)

## Cartridge layout

- IPL3 copies the first MiB starting at ROM `0x1000` to `0x80000400`. The
  SN64 startup stub maps physical RAM `0x000000–0x1fffff` at `0x00200000`
  with TLB entry 31. Its indirect call enters the engine at `0x0020aa50`.
- The original guest loop beginning at `0x0020aafc` copies another 512 KiB
  from cartridge address `0xb0101000` to `0x80100400`. Guest code clears
  BSS from `0x80114b60` to `0x80190000` and initializes the OS. The runtime
  retires the synthetic boot thread when the original main thread first calls
  the formatter at `0x00285f60`.
- At `0x0022561c` the guest requests a `0x4df50`-byte DMA from ROM
  `0x115760` to `0x80700000`. The map helper verifies these bytes before
  enabling executable dispatch at `0x00400000`. Asset region reservations
  begin at page `0x800`. The guest VM server receives page faults and the
  original DMA reader supplies the data.
- The engine uses four 12-byte mode records at `0x80104ec0`. Init
  (`0x002868a4`), retrace (`0x00286264`) and draw (`0x002854f8`)
  each call their own field after a null check. Mode 0 has no callbacks, mode
  3 performs the handoff, mode 2 draws the menu, and mode 1 draws the world.
  The 48 table bytes and nine callback entries are checked by generation.
- The original frame builder calls `0x002849bc` from `0x00285720` and queues
  the task through `0x00282e04`. Desktop boot has submitted graphics and
  received DP completions using two 284×222 VI buffers at `0x80500280` and
  `0x80554880`. The first verified colored screen shows the Controller Pak
  menu, its four controller icons and the animated background underneath
  “Checking Controller Pak...”.
- The graphics task builder at `0x002849bc` selects the same complete
  Acclaim F3DEX2 program and initialization data used by Turok 2. ROM
  `0x1114b0` contains the `0x1390`-byte text including its overlays,
  and ROM `0x1147f0` contains the `0x420`-byte data. Both games use the
  same fingerprint-checked renderer adapter in `games/turok2-us/`. Turok 3 also compiles
  the pager and controls from that directory against its own generated configuration.
- The audio builder at `0x00243ca8` selects ABI1. The boot program, text and
  initialized DMEM constants match Turok 2 and are checked at the first task.
  The original game submits audio through AI at the requested sample rate.

The archived [Drahsid/turok3 US layout](https://github.com/Drahsid/turok3/blob/1053f64/versions/turok3.us.yaml)
has the same retail SHA-1. The [n64hub ROM layout](https://github.com/n64hub/turok-3-recomp/blob/ec36469/syms/layout.toml)
identifies the second executable bank. Each address used by the port was
checked against the local US cartridge.
