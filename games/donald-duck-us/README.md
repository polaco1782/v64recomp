# Disney's Donald Duck: Goin' Quackers

This target uses the US revision 0 cartridge (NDQE), a 20 MiB big-endian
image with SHA-1 `e31a7567d408c890065029ba537f830765b17d94`.

## Build and run

From the repository root, import the cartridge with its local path.

```sh
.venv/bin/python tools/import_rom.py --profile games/donald-duck-us/profile.toml \
    --rom "/path/to/Disney's Donald Duck - Goin' Quackers (U) [!].z64"
python3 tools/build.py donald-duck-us --release
./build/donald-duck-us-gles-release/bin/donald-duck-us
```

The default Controller Pak image is `local/saves/donald-duck-us.mpk`.
Use `--save FILE` to play with a different image. The game uses the
[shared controller and keyboard mappings](../../README.md#controls-and-data).
On the keyboard, use the arrow keys to move, **A** to confirm or jump, **X**
to activate a teleporter, and **S** for Start.

## Desktop validation

On Linux GLES, the Debug replay starts with a fresh Controller Pak, selects
English, writes a named slot, plays the opening cutscene, and exercises movement
and jumping in Gyro's lab. Use an unused save filename for a fresh run.

```sh
python3 tools/build.py donald-duck-us --debug
RECOMP_LIVE_PATCH_DIR=off ./build/donald-duck-us-gles-debug/bin/donald-duck-us \
    --frames 8800 --input games/donald-duck-us/opening.txt \
    --save /tmp/donald-duck-opening.mpk --capture-every 120
```

Debug completed all 8,800 presentations without live repair and recorded
2,708,480 stereo PCM frames at 22,047 Hz. Both builds reopened the 32 KiB
Controller Pak and confirmed the loaded slot in the game's menu.

Use the same Controller Pak to replay the route into Forest Edge.

```sh
RECOMP_LIVE_PATCH_DIR=off ./build/donald-duck-us-gles-debug/bin/donald-duck-us \
    --frames 4320 --input games/donald-duck-us/forest-edge.txt \
    --save /tmp/donald-duck-opening.mpk --capture-every 120
```

Debug completed the 4,320-presentation replay without live repair. Both builds
used the lab's forest teleporter, crossed the bridge in Duckie Mountain, and
entered Forest Edge from the nearest pad on the left. Captures show movement
and jumping along the opening trail. The Debug run with captures recorded
1,390,080 stereo PCM frames, 34 queue underruns and zero queue resets. A separate
capture-free Release run sent the replay's input through X11 and completed in
72.2 seconds. It delivered 1,525,120 stereo PCM frames to PipeWire with zero
underruns or queue resets. Later stages and campaign completion remain unverified.

## ROM layout

The entry at `0x80000400` clears `[0x8001d1f0, 0x80036d80)` before
switching stacks and calling the SDK bootstrap. The original game thread at
`0x8000086c` loads the main program with PI DMA. Its call to `0x80000b30`
loads the support segment. Only the verified executable spans participate in
runtime section activation. Initialized data remains available for guest loads
and discovery of checked jump tables.

| Region | Cartridge bytes | Guest start | CPU text end |
| --- | --- | --- | --- |
| Boot | `[0x1000, 0x1ddf0)` | `0x80000400` | `0x80017e30` |
| Main | `[0x1ddf0, 0xc0b10)` | `0x80026d80` | `0x800bf520` |
| Support | `[0xc0b10, 0xcc000)` | `0x800f2900` | `0x800fd020` |

The host supplies an 8 MiB Expansion Pak configuration. The resident graphics
microcode is F3DEX.NoN 1.23. The audio tasks use the cartridge's ABI1 ASP
microcode. The initialized code and data of both microcodes match Rayman 2 US
byte for byte at their respective offsets. Generation verifies their
fingerprints before recompiling the RSP programs.

The public N64Recomp port provided section and SDK symbol leads. The load
instructions, task binaries, native SDK bodies, and callback sites used here
were checked against the local cartridge. [Reference revision 321c636](https://github.com/n64hub/donald-duck-goin-quackers-recomp/tree/321c636d287204cc2c0d4fa3f2d416634b5cc3bd)
