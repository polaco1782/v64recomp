# GoldenEye 007 US

Original startup, menus and playable Dam. CPU inventory is partial and remaining unsupported functions fail explicitly. Full-game progression remains unverified.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/goldeneye-us/profile.toml --rom /path/to/rom`.

```sh
cmake --workflow --preset goldeneye-us-release
./build/goldeneye-us-release/bin/goldeneye-us

cmake --workflow --preset goldeneye-us-debug
./build/goldeneye-us-debug/bin/goldeneye-us --help
```

`release/` contains the playable app and Release-only sources. `debug/` contains
the original trace/capture harness and inspection tools. Shared headers, adapters,
`generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

`profile.toml` contains the checked `discovery.jump_tables`. `generate.py` resolves
them from the fingerprinted, decompressed startup data before discovery checks
their transfer instructions and code targets.

Guest pthreads use the profile's entry-based names: `n64-main`, `n64-idle`,
`n64-scheduler`, `n64-sound` and `n64-controller`. `n64-sound` runs translated
sound code; `n64-audio` is the separate native DSP worker. See
[CPU profiling](../../docs/profiling.md) for thread counters and function samples.

After the first graphics task, both apps service VI/AI deadlines and worker
completions between presentations through `recomp_game_pace`. Generation checks
the empty idle loop and its `osCreateThread` argument setup before registering
the idle object with the scheduler. Synthetic-clock bootstrap keeps its bounded
drain.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).

Press F12 for the shared [GameShark menu](../../docs/cheats.md) in Debug or Release.

## Audio DMA cache recovery

The warning snapshots `warning-24048-60663945707700.json` and
`warning-24048-60664262225600.json` show all 64 sample DMA buffers in use and
`g_DmaState.firstFree == NULL`. Their frame counters are 13748 and 13751;
17 and 15 buffers respectively still belong to the frame two generations older.
The original `amDmaCallback` at `0x70002424` returns an unrelated buffer on a
cache miss when the free list is empty. Both failing ADPCM commands consume
that buffer at byte offset 1, interpreting sample payload as a predictor.
The ROM copies themselves match the buffers' recorded cartridge addresses.

`audio_dma.c` wraps the fingerprinted original callback in both configurations.
On a full-cache miss it returns the oldest buffer at least two frames old to
the free list, then lets the translated callback perform its normal ROM DMA.
Cache hits and current/previous-frame buffers remain intact. The original
cleanup at `0x70002610` sweeps before incrementing the frame counter, explaining
the retained older generation; `amMain` at `0x70001f7c` waits for the preceding
task's completion. These paths were checked against the generated code and
the [GoldenEye audio-manager reference](https://github.com/n64decomp/007/blob/master/src/audi.c).
This recovery does not expand the pool: exhaustion entirely within the protected
generations still follows the original fallback and retains the HLE warning.

The Release executable rebuilt successfully on 2026-10-30. In-game validation
is left to the user at their request; further testing was waived.

## Mouse and keyboard

Single-player on-foot gameplay uses mouse-look and WASD by default. Mouse motion
changes view angles directly; WASD uses the game's original movement and collision.
The adapter temporarily uses control style 1.2 inside input processing and restores
the selected style afterwards. Settings and saves retain the selected control style.

| Control | Action |
| --- | --- |
| Mouse / WASD | Look / move and strafe |
| Left / right mouse | Fire / original aim mode |
| Scroll up / down | Previous / next weapon |
| E or R / Q | Use or reload / change weapon |
| Tab | Start / watch menu |
| Arrows / Enter | Menu direction / confirm |
| Esc / F4 | Toggle mouse capture / recapture after release or focus loss |
| F10 | Quit |

Capture releases in menus, during watch transitions and when the window loses
focus. Tank, multiplayer, death and scripted-camera states fall back to the
original controller path. A gamepad remains available; release mouse capture to
use its original look/movement controls. Original aiming restrictions still apply.

Set `RECOMP_GOLDENEYE_MOUSE=0` to restore the legacy keyboard/controller mapping.
`RECOMP_MOUSE_SENSITIVITY` sets degrees per mouse count (default `0.08`, range
`0.001..10`); `RECOMP_MOUSE_INVERT_Y=1` inverts vertical movement. These environment
settings apply at launch in Debug and Release.

### Input implementation and validation

`input.c` adapts `bondviewProcessInput` at `0x7F081974`, scoped to its call from
`MoveBond` with return PC `0x7F084788`. It consumes mouse motion once per eligible
guest update and rebuilds orientation with the original angle helper before
movement. Wheel detents are queued and mapped to the original A / A+Z weapon
cycling actions. The shared transport and original-preserving wrappers are
described in [native hooks](../../docs/native-hooks.md).

`input_analysis.py` is a required generation dependency. It verifies function
fingerprints, call sites, player offsets and state globals declared in
`profile.toml`, and writes `input-hooks.json` into the selected build directory.
The semantic reference is [n64decomp/007](https://github.com/n64decomp/007/tree/c4356466796c697dfd298010b9bed261f9ed8c6a),
checked against this profile's exact ROM rather than assuming retail addresses.

Windows validation on 2026-10-30 included Debug/Release builds and a rendered Dam
replay with 510 adapted updates. It verified angular displacement, movement,
angle limits, control-style restoration and watch/death fallback. Mouse movement
was also confirmed through user playtesting. The later recapture and wheel
changes were built in both configurations; gameplay testing was left to the user.
Tank, multiplayer and full-game progression remain unverified.

Debug retains optional `RECOMP_GOLDENEYE_INPUT_TRACE` CSV output and
`RECOMP_GOLDENEYE_MOUSE_REPLAY` input for future investigations. Replay rows are
`eligible_update dx dy move_x move_y active` with increasing update indices.
The standalone investigation checks, fixtures and scratch output were removed
after recording their findings here.

## Field of view

Set `RECOMP_GOLDENEYE_FOV` before launching to choose a horizontal FOV in degrees
at 4:3 (`60..120`, including fractional values). Unset, empty or `0` preserves
the original camera, normally 60 degrees vertically (about 75.2 horizontally).
The setting works in Debug and Release independently of mouse controls.

```powershell
$env:RECOMP_GOLDENEYE_FOV = '100'
.\build\msvc\goldeneye-us\release\bin\Release\goldeneye-us.exe
```

Restore the original with `$env:RECOMP_GOLDENEYE_FOV = '0'`. Changes require a
restart. Native weapon zoom scales with the chosen FOV, preserving its optical
magnification ratio. The override applies to active single-player gameplay;
menus, watch, death, locked controls, scripted cameras and multiplayer retain
their original FOV. RT64's F7 aspect expansion can widen the displayed view
further than the 4:3 reference angle.

`fov.c` wraps the fingerprinted `viSetFovY` at `0x70004544`, scoped to the
viewport, zoom and level-render calls returning to `0x7f086fcc`, `0x7f07e730`
and `0x7f0be510`. The original setter updates both the VI projection and the
player's perspective/scale values used by CPU culling and screen calculations.
The native player FOV/zoom fields remain unchanged, so repeated updates cannot
compound the override. The conversion scales the tangent of each incoming
vertical half-angle rather than replacing zoomed views with a fixed angle.

The [pinned camera reference](https://github.com/n64decomp/007/blob/c4356466796c697dfd298010b9bed261f9ed8c6a/src/game/bondview.c)
guided identification; full ROM bodies, call/delay pairs, player offsets and
the original 60-degree literal are verified independently by `generate.py`
and `input_analysis.py` against `profile.toml`. Generation writes
`fov-hooks.json` alongside `input-hooks.json`.

On 2026-10-31, ROM/translation checks, Debug/Release builds and the synthetic
Debug check passed. To rerun the wrapper checks without opening a game window:

```powershell
$env:RECOMP_GOLDENEYE_CHECK_FOV = '1'
.\build\msvc\goldeneye-us\debug\bin\Debug\goldeneye-us.exe --frames 1
Remove-Item Env:RECOMP_GOLDENEYE_CHECK_FOV
```

These checks cover defaults, range validation, zoom ratios, repeat updates,
call scope and state gates; they do not execute the paged camera routines.
The user confirmed that the FOV works well in game. Broader aiming, zoom
transitions and combined widescreen behavior remain unverified.
