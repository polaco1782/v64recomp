# Carmageddon 64 (US revision 0)

The native desktop target uses the original game's menus, controller polling,
music engine and recompiled graphics and audio microcode. The US retail ROM
has cartridge ID `NCDE`, size 16,777,216 bytes and SHA-1
`dc7495093fb9a668b0c851b87c037a4cdf2ddc65`.

## Build and run

From the repository root with the project's Python environment installed:

```sh
.venv/bin/python tools/import_rom.py --profile games/carmageddon64-us/profile.toml \
    --rom '/path/to/Carmageddon 64 (U) [!].z64'
python3 tools/build.py carmageddon64-us --release
./build/carmageddon64-us-gles-release/bin/carmageddon64-us
```

Use `--debug` with the build helper for the Debug executable. The shared
keyboard mapping uses **S** for Start, **A** for N64 A, **X** for N64 B
and the arrow keys for the analog stick. Player one's Controller Pak uses
`local/saves/carmageddon64-us.mpk` by default.

## Cartridge implementation

The original entry at `0x80001000` clears BSS from `0x800a5180` up to
`0x8022bf00` in 4 MiB RDRAM. Thread 102 takes over from the IPL and starts
thread 103, which enters the game at `0x80015db0`. The profile describes
the resident image, local switches, ten front-end screen handlers and 45 Mus
sequence handlers. The entry inventory ties embedded front-end script callbacks
to their operand words and the interpreter call sites for each argument count.

The game submits F3DEX.NoN fifo 2.05 graphics tasks and ASP audio tasks.
The graphics image includes its IMEM overlays. The desktop replay produced
stereo PCM at 22,047 Hz through the original audio task path.

The SDK lookup used the repository's pinned n64sym signatures. Native bindings
match complete bodies in this ROM. The raw PI routine at `0x8007b9f8` accepts
KSEG-tagged cartridge offsets, including those used to load race assets.
[Upstream signature catalogue](https://github.com/shygoo/n64sym/tree/ccf4600f3389f1a84bde23339225cf372fdf7712).

## Reproduce Quick Start

```sh
python3 tools/build.py carmageddon64-us --debug
./build/carmageddon64-us-gles-debug/bin/carmageddon64-us --frames 3200 \
    --input games/carmageddon64-us/quick-start.txt --capture-every 120 \
    --save build/carmageddon64-us-gles-debug/replay.mpk
```

Use an unused save filename to reproduce the Controller Pak prompt. The replay
skips the publisher screens, moves down to **Quick Start**, confirms
the Controller Pak prompt and briefing, then accelerates and steers in the city.

On Linux GLES, Debug and Release both completed 3,200 presentations. Debug
recorded 1,083,456 stereo frames at 22,047 Hz, including nonzero samples during
the race. The final one-second status sample counted 4,549 RSP task submissions
and 3,107 controller reads. Release accepted the same route through the shared
X11 keyboard driver in 53.5 seconds. Its captures at 32, 42 and 50 seconds show
the car, other racers, changing speed and collisions. A second Release replay
without screenshots delivered 1,175,104 stereo frames to PipeWire with zero
audio underruns or queue resets. Quick Start wrote the
32 KiB Controller Pak and a later launch reopened it. Campaign save/reload,
full races and multiplayer remain unverified.
