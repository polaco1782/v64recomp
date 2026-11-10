# v64recomp

Native N64 game recompilation in C11. Python translates the original MIPS CPU
code and checked RSP graphics programs into C; a shared runtime provides the
console services, audio and GLES2 rendering. Users supply their own verified ROMs.

GLES2 and the compiled RSP worker are the only graphics path in this fork.
There is no renderer-selection flag or display-list HLE fallback. Audio and JPEG
tasks retain their existing native implementations. Support is being extended in
stages; see [game status](docs/status.md) and the [graphics notes](docs/armv8-gles2.md)
for the exact rendered milestones and remaining limits.

## Build and run

Clone the repository normally, install Python 3.11+, a C11 compiler, CMake 3.25+,
Ninja, OpenSSL 3 and SDL2, and install `requirements.txt` in a Python environment.
Linux also needs a GLES2-capable driver and X11/Xwayland or Wayland. Windows uses
the pinned [vcpkg dependencies](vcpkg.json); see [Windows setup](docs/windows.md).
No C++ compiler, renderer submodule or shader compiler is required.

Import the pinned ROM into ignored `local/`:

```sh
python3 tools/import_rom.py --profile games/goldeneye-us/profile.toml --rom /path/to/goldeneye.z64
cmake --preset goldeneye-us-release
cmake --workflow --preset goldeneye-us-release
./build/goldeneye-us-release/bin/goldeneye-us
```

All game presets and workflows use the same default graphics path. A separate
build directory is also supported:

```sh
cmake -S . -B build/goldeneye-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DRECOMP_GAME=goldeneye-us
cmake --build build/goldeneye-release -j4
./build/goldeneye-release/bin/goldeneye-us
```

Select another game with its preset or `RECOMP_GAME`; each game keeps one
`profile.toml`, `generate.py` and `main.c`. Configure Debug in a separate directory.
`-DRECOMP_OPTIMIZE_DEBUG=ON` optimizes game/runtime code while preserving Debug
checks and diagnostics. `-DRECOMP_GAME=none` builds shared libraries only.

For a bounded Debug check with private saves:

```sh
./build/goldeneye-us-debug/bin/goldeneye-us --frames 600 --mute \
  --save /tmp/goldeneye.eeprom --crash-dir /tmp/goldeneye-crashes \
  --capture-every 300
```

Run `--help` for supported options. Debug adds shared controller replays, WAV
recording, call traces and frame captures. Release retains runtime correctness
checks and the shared crash reporter. The [debugging guide](docs/debugging.md)
explains logs and crash evidence; the [profiling guide](docs/profiling.md) covers
CPU sampling. Builds require the exact ROM revision declared by the game profile.

## Graphics and input

Original CPU code builds display lists. Each SP graphics task snapshots its RAM
and descriptor and wakes a worker running statically recompiled C11 microcode.
The owner thread renders the resulting RDP words with GLES2, commits DMA/readback
writes and posts SP/DP completion. [Architecture and acceptance](docs/armv8-gles2.md)
describe the implementation and its limits.

Desktop image quality defaults to 4x internal resolution, FXAA, bilinear
magnification and mipmap filtering where supported. ARM64 defaults to native
resolution with FXAA off. Override the preferences at launch:

```sh
RECOMP_GLES2_SCALE=4 RECOMP_GLES2_AA=1 ./build/goldeneye-us-release/bin/goldeneye-us
```

`RECOMP_GLES2_MIPMAPS=0` disables mipmaps. `RECOMP_GLES2_GAMMA=1` selects a neutral
color curve; the default exponent is 1.2. Higher resolution smooths edges without
adding texture detail. Rendering keeps the game's original update rate. The
former HLE geometry interpolation, texture-pack integration and renderer-specific
inspection/cheat UI are absent from this fork.

| Control | Action |
| --- | --- |
| Arrow keys | N64 analog stick by default. |
| A / X / Z | N64 A / B / Z. |
| S | N64 Start. |
| I / K / J / L | N64 C-buttons. |
| C / V | N64 L / R. |
| F7 | Toggle supported native expanded aspect. |
| Keypad + / - | Adjust FOV in games with a checked FOV adapter. |
| Escape / F4 | Release or recapture the mouse in supported mouse-control games. |
| F10 | Quit. |

Supported games retain their ROM-checked WASD/mouse and gamepad camera adapters;
consult each game's README for its controls and FOV behavior. Save files remain
private under `local/saves/`. Temporary validation saves do not replace them.

## Current graphics stage

Checked compiled graphics programs exist for SM64, GoldenEye, 1080, Shadow Man,
Quake, Quake II, Duke Nukem 64, Duke Zero Hour, Nightmare Creatures, Rayman 2,
Perfect Dark and Banjo-Kazooie. GoldenEye has bounded Dam gameplay evidence and
Perfect Dark reaches the Carrington Institute room. Other scenes and remaining
ports have narrower acceptance boundaries; [status](docs/status.md) is authoritative.

Zelda, Conker, Hexen, Turok, RE2 and TWINE still need checked graphics programs.
Their CPU port sources remain, but a missing graphics integration fails during
configuration. Native video demo integration is tracked separately. Full campaigns,
listening quality and ARM/Pi hardware performance remain unverified.

## Repository layout

- `games/<game>/`: game lifecycle, checked profile, generation and adapters.
- `runtime/` and `include/`: shared CPU, memory, OS, I/O, scheduler and RSP services.
- `backends/`: C11 GLES2 RDP renderer, input, audio and JPEG adapters.
- `tools/`: Python discovery, static recompilation, packaging and diagnostics.
- `third_party/`: independently licensed audio/JPEG, fiber and font dependencies.
- `build/`: generated private C, executables and build caches.
- `local/`: user ROMs, saves and crash reports, excluded from version control.

See [adding a ROM](docs/adding-a-rom.md), [recompilation](docs/recompilation.md),
[threading](docs/threading.md), [audio](docs/audio.md), [ROM packaging](docs/rom-packaging.md)
and [Steam Deck packaging](docs/steamdeck.md). `tools/format.sh` formats project C;
`python3 tools/msvc_solution.py` refreshes the browsable Visual Studio projects.

## License

The project uses GNU GPL version 2; see [LICENSE](LICENSE) and
[third-party notices](THIRD_PARTY_NOTICES.md). The license does not grant rights
to game ROMs, extracted assets or third-party content. No commercial ROM is bundled.
