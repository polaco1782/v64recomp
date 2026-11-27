# Doom 64 US

This port uses the US revision 0 cartridge (`NDME`), 8 MiB, SHA-1
`799a588d73da3fcce8031026a8187da92b91c817`.

## Build and run

From the repository root, import your cartridge and build the desktop game.

```sh
.venv/bin/python tools/import_rom.py --profile games/doom64-us/profile.toml \
  --rom '/path/to/Doom 64 (U) [!].z64'

tools/build.py doom64-us --gles --release
./build/doom64-us-gles-release/bin/doom64-us
```

`--debug` selects the diagnostic build. The ROM importer
writes `local/doom64.us.z64`. The first controller's 32 KiB Controller Pak
uses `local/saves/doom64-us.mpk`.

## Playing

Use **WASD** to move and strafe. Move the **mouse** left or right to turn and
up or down to look. Hold **Shift** to run. You can look 35° above or below
level at the original FOV. At 140° FOV the limit is about 16°. Weapons use
Doom’s original auto-aim.

**Left click** fires, **E** or **right click** uses a door or switch, and the
**mouse wheel** or **Q/R** changes weapons. **F** cycles the automap. **Tab** pauses.

In Doom’s menus, use **WASD** or the **arrow keys** to move the selection.
**Enter** or **left click** confirms, including saving or loading the selected
Controller Pak slot and activating a warp in Features. **Tab** or **right click**
backs out. **J** and **L** press C-Left and C-Right individually.
The mouse stays captured in these menus. **Escape** releases it and **F4** recaptures
it. A connected controller uses the game's button layout. Set
`RECOMP_DOOM64_MOUSE=0` to use the original desktop pad mapping instead.
`RECOMP_MOUSE_SENSITIVITY` sets degrees per mouse count (default 0.08).

**Options → Display → Brightness** starts at 100. The slider lifts dark
world lighting while preserving black at the start of level fades. Zero
restores the cartridge's original lighting. **Default Display** restores 100.

The **keypad + / −** keys adjust the horizontal field of view from the
original 90° to 140° in one-degree steps. The setting changes which parts
of the world the CPU considers visible as well as the rendered projection.

A new Controller Pak prompts for a game note on the first new game. Doom
recognizes that note on the next launch.

The 1,750-presentation replay starts with a fresh Pak. It opens New Game,
creates the note, chooses a difficulty and enters **Staging Area**. It moves
forward, collects the shotgun, fires, cycles the textured and line automaps,
and opens the pause menu. To reproduce it in Debug, use a new save filename
under the build directory.

```sh
./build/doom64-us-gles-debug/bin/doom64-us --frames 1750 \
  --input games/doom64-us/first-level.txt --save build/doom64-us-gles-debug/replay.mpk
```

Linux GLES Debug and Release builds reached the first level, saved and reloaded
a level 03 slot, and activated a Features warp. The Debug runs produced stereo
PCM at 22,047 Hz. The menu
checks used a temporary Pak and a debugger to unlock Features and trigger the
end-of-level intermission. Full campaign progression and sustained audio quality
still need playtesting.

Both builds also reached **Watch Your Step** (MAP17). Replaying captured RDP
commands at 1× and 4× render scale verified that edge coverage removes the
moving slivers at the doorway across several camera angles.

## Implementation

The recompiler starts at guest address `0x80000400`. The original program
clears BSS and creates its idle, main and ticker threads. The renderer accepts
F3DEX.NoN 1.00 for the world and L3DEX 1.00 for the line automap. The sound
system produces commands for the recompiled ABI1 microcode. The IWAD header is
at cartridge offset `0x63d10` and its directory contains 1,527 entries.

The US ROM supplies the callback addresses for menus, thinkers and the 797
object and weapon animation states. The public reverse-engineering project
helped identify the callback roles and state layout. The build reads the
original instructions, table records and assets from your cartridge.
[DOOM64-RE reference revision](https://github.com/Erick194/DOOM64-RE/tree/6931e678a0b2958be1b49598f2fe60712c6596e1)
