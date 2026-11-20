# The Legend of Zelda: Majora's Mask (US)

Import the 32 MiB NTSC-U cartridge with game ID `NZSE` and SHA-1 `d6133ace5afaa0882cf214cf88daba39e266c078`. The desktop build uses RT64 and an 8 MiB Expansion Pak configuration.

```sh
tools/import_rom.py --profile games/mm-us/profile.toml --rom "/path/to/Majora's Mask.z64"
tools/build.py mm-us --release --rt64
./build/mm-us-release/bin/mm-us
```

For input replay, frame captures and audio recording, build with `--debug` and run `./build/mm-us-debug/bin/mm-us --help`. Flash saves use `local/saves/mm-us.flash` by default.

To exercise file creation with a fresh save, run the Debug replay from the repository root. Choose an unused save filename.

```sh
./build/mm-us-debug/bin/mm-us --frames 3200 --input games/mm-us/new-file.txt --save build/mm-us-debug/new-file.flash
```

The game loads its main executable and overlays from its DMA table at ROM offset `0x1a500`. Generation checks the decompressed main executable and 617 overlays against their SHA-256 fingerprints. At runtime the original loader performs DMA, Yaz0 decompression and MIPS relocations. The host checks each image before enabling its compiled functions. The entry inventory uses the NTSC-U ELF function starts from [Zelda64RecompSyms](https://github.com/Zelda64Recomp/Zelda64RecompSyms/tree/5815f864c3bc75a582ca9c73496ab455264fdde9), checked against the supplied cartridge. Its symbol file uses the name `mm.us.rev1` for this image.

RT64 processes the game's F3DZEX and S2DEX graphics tasks. The audio backend checks the MM NEAD audio program, and JPEG tasks use the game's checked photo decoder. Desktop validation covered the title, file creation and reload, opening cutscene, movement in the first forest, and pause and resume.

## Desktop controls

WASD moves Link with the N64 analog stick. Hold Shift to walk slowly. Move the mouse to orbit the ordinary third-person camera. Use the left mouse button for the sword, the right mouse button for Z-targeting, Ctrl for the shield, and E or Space for the A action. Q, R and F activate the left, down and right C buttons respectively. Shift+F activates C-Up.

Tab presses Start. In menus, use the arrow keys to move, Enter to confirm and E to go back. Escape releases the mouse and F4 recaptures it. Set `RECOMP_MM_MOUSE=0` for the original desktop keyboard mapping, or adjust `RECOMP_MOUSE_SENSITIVITY` in degrees per mouse count (default `0.08`). A connected controller keeps the original N64 controls.

Keypad + and − change the ordinary gameplay camera’s horizontal field of view in one-degree steps. The minimum restores the original projection, about 75.2° horizontally at 4:3 with a 60° vertical lens. The maximum is 140°. Scripted cameras and menus use their authored views.

The orbit uses the cartridge’s camera collision test to keep the eye on the player’s side of walls. The ordinary world view supplies both the rendered projection and the projection diagonals that MM uses for actor culling. A stationary forest test widened the horizontal view from 75.2° to 109.2° and restored it in Debug and Release.
