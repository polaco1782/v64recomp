# v64recomp

Native N64 recompilation for Linux. ROM CPU instructions are translated from MIPS
to C11 and compiled for the host. Shared native services supply scheduling, OS,
input, saves and audio; RT64 supplies Vulkan graphics.

Super Mario 64 US is the working gameplay baseline. GoldenEye US reaches the
menus and playable Dam; its CPU inventory and broader gameplay validation remain
in progress. Shadow Man US runs original startup, graphics, audio and its
Controller Pak screen; title/gameplay work remains in progress. See
[SM64](games/sm64-us/README.md), [GoldenEye](games/goldeneye-us/README.md),
[Shadow Man](games/shadowman-us/README.md) and [the roadmap](PLAN.md).

## Setup

Requirements: Python 3.11+, a C/C++ compiler, CMake, Ninja, OpenSSL 3, SDL2, X11
or Xwayland, Vulkan and the populated RT64 checkout in `third_party/rt64/`.
Run commands from this checkout's root:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
# Optional texture tools:
.venv/bin/python -m pip install -r requirements-textures.txt
```

Import each supported ROM once. ROMs, saves and texture packs stay in `local/`.

```sh
.venv/bin/python tools/import_rom.py \
  --profile games/sm64-us/full.toml --rom '/path/to/Super Mario 64 (U).v64'
.venv/bin/python games/goldeneye-us/run_goldeneye.py \
  --rom '/path/to/007 - GoldenEye.v64' --report-only
.venv/bin/python games/shadowman-us/run_shadowman.py \
  --rom '/path/to/Shadow Man (U) [!].z64'
```

## Build and run

Each launcher regenerates its CPU sources and builds the game with the shared
runtime and RT64. Build output stays in `build/`; the RT64 source tree stays clean.

```sh
.venv/bin/python games/sm64-us/run_sm64.py --render --build-only
.venv/bin/python games/goldeneye-us/run_goldeneye.py --render --build-only
.venv/bin/python games/shadowman-us/run_shadowman.py --render --build-only

.venv/bin/python games/sm64-us/run_sm64.py --render
.venv/bin/python games/goldeneye-us/run_goldeneye.py --render
.venv/bin/python games/shadowman-us/run_shadowman.py --render --frames 0
```

The executables are `build/renderer/sm64_native_full_rt64` and
`build/renderer/goldeneye_native_rt64`, plus `build/renderer/shadowman_native_rt64`.
Add `--jobs N` to control build parallelism. SM64 and GoldenEye accept
`--cheat-menu` for their session-only debug/unlock menus.
Press **F1** while rendering to toggle RT64's ImGui debug inspector. It provides
graphics settings, workload debugging and performance statistics. Keyboard input
captured by the inspector is withheld from the game's controller. Graphics
settings changes apply to the current session.
Rendering defaults include 4x resolution, 4x MSAA, widescreen expansion to the
window's aspect ratio and texture LOD scaling. The bundled RT64 revision lacks
the ray-tracing renderer implementation, so ray tracing is unavailable.

To build just the shared runtime and audio:

```sh
cmake -S . -B build/host -G Ninja
cmake --build build/host
```

## Layout

- `games/<game>/`: applications, profiles, inventories, launchers, game-specific
  audio recognition, graphics extensions and capture inspection.
- `runtime/` and `include/`: shared native services and interfaces.
- `backends/`: shared RT64, audio and texture adapters.
- `tools/`: shared ROM import, discovery, translation, builds and texture tools.
- `third_party/`: RT64, minicoro and the audio DSP dependency.
- `local/`: ignored ROMs, saves, texture packs and driver caches.
- `build/`: disposable generated code, compiled output and captures.

[Audio](docs/audio.md), [floating point](docs/fpu.md) and
[texture workflow](docs/textures.md) document the shared services.
Common OS, memory and string routines use a [shared native hook catalog](docs/native-hooks.md)
automatically, with explicit game-profile overrides for unusual variants.
