# Super Mario 64 US

SM64 US CPU instructions are converted from the ROM into C11 and compiled for
Linux x86-64. Rabbitizer decodes the MIPS instructions. The program runs compiled
C functions; native services supply the N64 OS, graphics, audio, input and saves.
Game-specific code, launch tools and profiles live in `games/sm64-us/`.
Run the commands below from the `v64recomp` checkout root.

The complete checked ordinary CPU inventory contains **4,378 functions and
301,116 instruction words**, including compiler-emitted unreachable instructions.
**45 OS entry points** bind to native handlers. Linked OS implementation bodies,
embedded RSP programs and data have separate fingerprinted classifications.
Conversion completes without unsupported instructions, unresolved transfers,
conflicting ownership or fail-on-entry game-function boundaries.

## Run

From a Linux desktop terminal with X11/Xwayland, Vulkan and SDL audio available:

```sh
.venv/bin/python games/sm64-us/run_sm64.py --render
```

The app starts at the original splash and Mario-face title screens. Press **S**
for Start. Use **arrows** and **A** to navigate file select. In gameplay, arrows
move, A jumps, X punches, Z crouches, and I/K/J/L control the camera. Escape or
window close exits. **F6** toggles between wireframe geometry and textured rendering;
HUD and other 2D rectangles keep their normal appearance.
Textured geometry uses UltraHLE's 15 wire colors, selected by the texture's
RDRAM source address modulo 15; untextured geometry uses white.
The original game controls asset loading and save behavior;
textures, models, scripts, sequences and sound samples come from the ROM.
RT64 renders at 4 times the original resolution with 4x MSAA and bilinear texture
filtering. These settings are selected in `backends/rt64.cpp`; the adapter does
not read an RT64 configuration file.
The save device defaults to `local/saves/sm64-us.eeprom`; `--save PATH`
selects another EEPROM file.

`--cheat-menu` enables the original hidden **SELECT STAGE** debug menu:

```sh
.venv/bin/python games/sm64-us/run_sm64.py --render --cheat-menu
```

Press **S** at the Mario-face title screen to open it. **A/X** select the
next/previous stage; **T/G** do the same, and **F/H** skip ten stages. Press **S**
to enter the selected stage. The original menu uses save file **D** and act **6**;
Exit Course returns to the menu. **Z + S + J + L** exits it and disables debug
level select for that run. The option requires live input and an unbounded run
(`--frames 0`, the default with `--render`).

`--dump-textures` exports original RGBA PNGs with stable texture hashes to
`local/textures/sm64-us/dump/`. Matching PNGs in `replacements/` load
automatically; F5 reloads the directory and toggles between original and
replacement textures during play. The separate
`games/sm64-us/remake_textures.py` tool can ask OpenAI to redraw them with more detail.
See [texture workflow](../../docs/textures.md) for commands, API setup and limitations.
`--dump-grid` exports every dumped texture into grids of up to sixteen tiles, with
at least 512×512 pixels per square tile (equivalent area for rectangles), layout
JSONs and prompts for an external image model. Add `--render` to collect textures
while playing and export on exit; without it, the launcher exports the existing
dump. The model redraws one complete PNG per grid, preserving dimensions and tile
positions. The texture tool's `--import-grid DIRECTORY --grid-json JSON` imports
the processed grids into replacements using the master manifest.

```sh
# Compile the complete result without starting the window.
.venv/bin/python games/sm64-us/run_sm64.py --render --build-only
# Keep task/RAM snapshots and a stereo WAV while playing.
.venv/bin/python games/sm64-us/run_sm64.py --render --capture
# Muted output still synthesizes sound and preserves AI timing.
.venv/bin/python games/sm64-us/run_sm64.py --render --mute
# A deterministic route selects file A and walks through the castle door.
.venv/bin/python games/sm64-us/run_sm64.py --render --mode interior --frames 180
```

Interactive timing follows a monotonic clock. The serialized renderer uses this
clock for pacing and disables the additional compositor wait. Scripted checks use an injected
clock and accelerated startup; their window opens when Mario becomes controllable.
`--frames` accepts 60..600 in scripted checks. For `interior`, it counts frames
inside the lobby. `--jobs` controls renderer build parallelism.

## Generate and inspect

```sh
.venv/bin/python games/sm64-us/run_sm64.py --report-only
.venv/bin/python games/sm64-us/run_sm64.py --build-only
```

The result is under `build/sm64-us/rom/full/`: `functions.c`, `generated.h`,
`game_config.h`, `symbols.json`, `coverage.json`, `full_boundaries.json` and the
native executable. The renderer executable is
`build/renderer/sm64_native_full_rt64`. Identical successful generations
preserve source timestamps, avoiding unnecessary recompilation of the large C file.

`games/sm64-us/full.toml` is the single game profile. `code-inventory.json` holds
ROM fingerprints, CPU entry addresses and code/data/native-OS/RSP ranges. These
are persistent conversion inputs. CPU text boundaries and missing entries were
cross-checked against a byte-matching ELF/map during development. Builds and
runtime need the ROM and profile/inventory; no reference C, ELF or map is read.

Ordinary indirect calls accept any registered function in a loaded code section.
An unknown or unloaded address fails explicitly. Computed local jumps retain
checked jump tables. Hooks preserve the N64 register/stack ABI, guest memory
layouts and completion events. Calls and floating-point helpers update `ctx->pc`
for diagnostics and thread snapshots. Branches retain scheduler checkpoints
without updating it; compiled C calls and labels control execution.

Set `LOG_EMISSION = True` near the top of `tools/emit.py` to trace translation
and C emission. It defaults to `False`, keeping the emitter silent. The trace
goes to stderr and includes progress, function/section addresses, verified native
OS/hardware hook bindings, direct and indirect call sites, memory helper lowering,
scheduler checkpoints and unsupported boundaries. This also applies to the full runner. These messages describe code generation;
they do not trace execution of the generated program.

## Captures and diagnostics

Bounded runs and `--capture` produce task traces, RAM/list snapshots,
`audio.wav` and audio statistics. Bounded rendered runs also export window and
native framebuffer PNGs. Normal interactive play does not write continuous
RAM/task/WAV captures. `--playback` enables paced audio in a headless check.

Runtime failures print a native C backtrace and automatically save timestamped
`crash-*.json` and `crash-*-rdram.bin` files in the run output directory. Reports
include the failing address/range, level and input, guest thread registers,
loaded sections, the latest SP task and the last 16 audio commands. Thread PCs
are labeled as the last tracked call/FP location. Branches do not update them.
Native debug symbols let the inspector resolve C source lines and nearby ROM
instructions without adding instruction tracking to the generated program:

```sh
# Inspect the newest report from the default full build, or pass a report path.
.venv/bin/python tools/inspect_crash.py
```

Keep the matching executable and generated source when inspecting an older
report. Reports currently cover controlled `recomp_fail` errors; host faults
outside those checks still use the debugger or system core dump.

## Setup and limits

Python 3.11+, Rabbitizer, a C/C++ compiler, CMake/Ninja, OpenSSL, SDL2 and the
RT64 sources in `third_party/rt64/` are required. Create the Python environment with:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python tools/import_rom.py \
  --profile games/sm64-us/full.toml --rom '/path/to/Super Mario 64 (U).v64'
```

Only the verified SM64 US ROM is accepted: SHA-1
`9bef1128717f958171a4afac3ed78ee2bb4e86ce`. The importer normalizes common N64
byte orders. ROMs and generated game code remain local and ignored by Git.

Complete CPU conversion does not establish complete gameplay compatibility.
Broader course/ending/prolonged-play checks remain. Graphics are serialized;
audio accepts the verified SM64 ABI1 microcode. Audio rate changes and some
exceptional floating-point operands still require explicit support. See
[docs/audio.md](../../docs/audio.md), [docs/fpu.md](../../docs/fpu.md) and [PLAN.md](../../PLAN.md).
