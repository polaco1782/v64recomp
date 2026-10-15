# v64recomp

Native N64 recompilation for Linux. ROM instructions are translated to C and
compiled for the host; shared native services supply scheduling, input, saves,
audio, and RT64 Vulkan graphics. See [how it works](docs/recompilation.md).

## Screenshots

Captured from the native builds.

| Super Mario 64 | The Legend of Zelda: Ocarina of Time | GoldenEye 007 |
| --- | --- | --- |
| ![Super Mario 64 in Castle Grounds](docs/screenshots/super-mario-64.png) | ![The Legend of Zelda: Ocarina of Time title screen](docs/screenshots/ocarina-of-time.png) | ![GoldenEye 007 gameplay](docs/screenshots/goldeneye-007.png) |

## Build and play

Requirements: Python 3.11+, C/C++ compiler, CMake 3.20+, Ninja, OpenSSL 3, SDL2,
X11/Xwayland, Vulkan, and the populated RT64 checkout in `third_party/rt64/`.
Python generates CPU sources during the build. Built games launch directly.

Run these commands from the checkout root to install the build tools:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
```

Provide your own ROM files and import each game once with
[`tools/import_rom.py`](tools/import_rom.py). The tool accepts big-endian
(`.z64`), byte-swapped (`.v64`) and word-swapped (`.n64`) ROMs. It normalizes the
bytes, verifies the selected profile's exact ROM size and SHA-1, then copies the
result into `local/` with the required filename. It rejects a mismatched ROM or
an existing destination containing different bytes.

```sh
# Super Mario 64 US
.venv/bin/python tools/import_rom.py \
  --profile games/sm64-us/full.toml --rom '/path/to/Super Mario 64.v64'

# Ocarina of Time US V1.2
.venv/bin/python tools/import_rom.py \
  --profile games/oot-us-v12/profile.toml --rom '/path/to/Ocarina of Time.n64'
```

SM64 uses `games/sm64-us/full.toml`; the other supported games use
`games/<game>/profile.toml`, where `<game>` is `goldeneye-us`, `shadowman-us`,
`quake-us`, `duke-us`, `nightmare-us`, or `oot-us-v12`.
The imported, normalized files have these filenames and MD5 checksums:

| Filename | MD5 |
| --- | --- |
| `duke.us.z64` | `c7f1a43764a26da2e43f2a36a5f76e4c` |
| `goldeneye.us.z64` | `22ff6c18eb028792ef350554b1e1fa68` |
| `nightmare.us.z64` | `4116cf435db315a2481af8d1e9352feb` |
| `oot.us.v12.z64` | `57a9719ad547c516342e1a15d5c28c3d` |
| `quake.us.z64` | `097605021951024c3ecb2d502c0c2a9f` |
| `shadowman.us.z64` | `b457298b87b85bbf950f24867daa9475` |
| `sm64.us.z64` | `20b854b239203baf6c961b850a4a51a2` |

After importing a ROM, build and run its game:

```sh
cmake --preset sm64-us-release
cmake --build --preset sm64-us-release -j 4
cmake --build --preset sm64-us-release --target run
# Or launch the executable directly, from any working directory:
./build/sm64-us-release/bin/sm64-us
```

Replace `sm64-us` with `goldeneye-us`, `shadowman-us`, `quake-us`, `duke-us`,
`nightmare-us`, or `oot-us-v12`. ROMs, saves, texture packs and crash dumps stay
in ignored `local/`.

The equivalent explicit CMake configuration is:

```sh
cmake -S . -B build/quake-release -G Ninja -DRECOMP_GAME=quake-us -DCMAKE_BUILD_TYPE=Release
cmake --build build/quake-release -j 4
cmake --build build/quake-release --target run
```

CMake selects one game per build tree and regenerates CPU sources when the ROM,
profiles, inventories or generator change. Unchanged generation is cached. Missing
ROMs or generation failures stop configuration explicitly. The generation log is
in the build directory. CPU sources are split into smaller files for parallel
compilation. Use `-DRECOMP_GAME=none -DRECOMP_BUILD_RT64=OFF` to build shared libraries.

## Release and debug

`games/<game>/release/` holds the playable app, audio profiles and required game
adapters, such as paging, graphics extensions and overlay loading. Both modes
use these adapters and the same checked ROM metadata in the game's root.
Release retains correctness checks and crash reporting. It writes no routine
reports, WAVs, screenshots, RAM captures or call traces during normal play.

`games/<game>/debug/` holds diagnostic applications, replay inputs and inspection
tools. Debug includes full symbols, unoptimized game code, call tracing, captures,
gameplay probes, RT64's F1 inspector and F6 wireframe.
It retains the existing diagnostic startup behavior and checks.

```sh
cmake --preset quake-us-debug
cmake --build --preset quake-us-debug -j 4
./build/quake-us-debug/bin/quake-us --frames 300 --capture-every 60
./build/quake-us-debug/bin/quake-us --trace-calls
cmake --build --preset quake-us-debug --target report
```

All executables accept `--help`, `--rom FILE`, `--data DIR`, `--save FILE`,
`--crash-dir DIR`, `--frames N` (0 runs until close), and `--mute`.
Debug additionally supports `--input FILE`, `--wav FILE`, `--audio-trace FILE`,
`--capture-every N`, and `--trace-calls`. Per-game availability is listed in
[the debug workflow](docs/debugging.md). For the CMake run target, pass arguments
with `-DRECOMP_RUN_ARGS='--frames;300;--mute'` at configuration.

VS Code's **Debug: <game>** configurations configure/build the debug preset and
launch the native executable. Breakpoints work in game adapters, the shared
runtime, backends and `build/<game>-debug/generated/<game>/cpu-*.c`. Persistent
translation fixes belong in the generator or checked metadata.

## Controls and data

Arrow keys move the stick; A/X/Z are N64 A/B/Z; S is Start; Q/E are L/R;
I/J/K/L are C buttons. Escape or window close exits. F5 reloads texture packs
and toggles original/replacement textures. Packs default to
`local/textures/<game>/replacements`; `RECOMP_TEXTURE_REPLACEMENTS` overrides this
path (an empty value disables packs). Rendering uses 4x resolution, 4x MSAA,
widescreen expansion and texture LOD scaling. This RT64 revision has no game
ray-tracing renderer.

Release failures routed through `recomp_fail` write a uniquely named JSON report
and RDRAM snapshot to `local/crashes/<game>/`. Reports include native stack
addresses, guest thread registers/last tracked PCs and the latest task. Keep the
executable and its generated sources when investigating a crash:

```sh
.venv/bin/python tools/inspect_crash.py
```

Host faults outside these controlled checks require GDB or a host core dump.
Save paths preserve the existing SM64/GoldenEye EEPROM and OOT SRAM locations.
Game support remains at the milestones in [status](docs/status.md); selecting
Release does not establish full-game correctness for partial ports.

## Layout

- `games/<game>/release/`: playable app and required game adapters/generation inputs.
- `games/<game>/debug/`: trace apps, captures/replays and inspection tools.
- `games/<game>/*.toml`, `*.json`: shared pinned ROM profiles and checked inventories.
- `runtime/`, `include/`, `backends/`: shared runtime and rendering/audio services.
- `cmake/`, `CMakePresets.json`: game selection, generation, build and run integration.
- `tools/`: shared ROM import, discovery, translation and crash inspection tools.
- `third_party/`: RT64 and the audio DSP dependency.
- `local/`: private ROMs, saves, packs, renderer data and crash dumps.
- `build/`: disposable generated sources, binaries and debug output.

See [audio](docs/audio.md), [floating point](docs/fpu.md),
[threading](docs/threading.md), [native hooks](docs/native-hooks.md),
[debugging](docs/debugging.md), and [textures](docs/textures.md).

## Legal and third-party software

This repository does not include or distribute ROM images or copyrighted game
assets. Users must provide their own ROM files. The project uses free and
open-source software; each third-party component remains subject to its own
license.

Graphics are provided by [RT64](https://github.com/rt64/rt64), an open-source
N64 graphics renderer used by this project for Vulkan rendering.
