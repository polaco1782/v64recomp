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

`main.c` contains the shared entry point and lifecycle for Debug and Release.
Headers, adapters, `generate.py`, `profile.toml`, inventories and maintained
replay inputs live beside it. Generated code stays under the selected build
directory; private ROMs and saves stay in `local/`.

`profile.toml` contains the checked `discovery.jump_tables`. `generate.py` resolves
them from the fingerprinted, decompressed startup data before discovery checks
their transfer instructions and code targets.

Guest pthreads use the profile's entry-based names: `n64-main`, `n64-idle`,
`n64-scheduler`, `n64-sound` and `n64-controller`. `n64-sound` runs translated
sound code; `n64-audio` is the separate native DSP worker. See
[CPU profiling](../../docs/profiling.md) for thread counters and function samples.

After the first graphics task, the common frame loop services VI/AI deadlines and worker
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
| Mouse in the mission menus | Move the target cursor |
| Left / right mouse in menus | Confirm / back |
| Scroll up / down | Previous / next weapon |
| E or R / Q | Use or reload / change weapon |
| Tab | Start / watch menu |
| Arrows / Enter | Menu direction / confirm |
| Esc / F4 | Toggle mouse capture / recapture after release or focus loss |
| F10 | Quit |

Mouse capture stays active in the front-end menus so mouse movement controls the
original target cursor. Arrows and gamepads remain available alongside the mouse.
Esc releases the mouse; F4 recaptures it. Capture releases during watch transitions
and when the window loses focus. Tank, multiplayer, death and scripted-camera
states fall back to the original controller path. Release mouse capture to use
the dual-stick gamepad controls below. Original aiming restrictions still apply.

Set `RECOMP_GOLDENEYE_MOUSE=0` to disable mouse controls and retain the shared
keyboard mapping. `RECOMP_GOLDENEYE_GAMEPAD=0` also restores legacy stick mapping.
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

The front-end cursor adapter wraps `frontUpdateControlStickPosition` at
`0x7F009B84`. In the title/menu stage it consumes mouse motion once, moving the
original cursor by half a menu pixel per mouse count before the original routine
adds keyboard/gamepad movement and clamps it to the viewport. Menu movement is
independent of gameplay look sensitivity and pitch inversion. The wrapper's full
body fingerprint and cursor globals (`0x8002A908`, `0x8002A90C`) are verified during
generation. Left/right clicks use the original confirm/back button paths.

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

The menu mouse change was confirmed working through user playtesting on
2026-11-02. Debug and Release builds passed; a bounded Debug desktop smoke run
also rendered startup and exited successfully.

Debug retains optional `RECOMP_GOLDENEYE_INPUT_TRACE` CSV output and
`RECOMP_GOLDENEYE_MOUSE_REPLAY` input for future investigations. Replay rows are
`eligible_update dx dy move_x move_y active` with increasing update indices.
The standalone investigation checks, fixtures and scratch output were removed
after recording their findings here.

## Field of view

Use keypad `+` / `-` to adjust FOV live, one horizontal degree per keydown;
holding a key repeats. The range is the exact original view (60 degrees
vertically, about 75.2 horizontally at 4:3) through 140 degrees. The original
view is the default each launch. Lowering to the minimum restores the original
FOV path. No FOV environment variable is read. The current angle is printed to
the console when it changes.

This works in Debug and Release independently of mouse capture. Native weapon
zoom retains its optical magnification ratio. The override applies to active
single-player gameplay; menus, watch, death, locked controls, scripted cameras
and multiplayer retain original behavior. F7 widescreen is independent of FOV.

The original `viSetFovY` body remains translated and updates VI projection and
player camera scaling together. Full bodies, caller/delay pairs, player offsets
and the native 60-degree literal are checked against the ROM by `generate.py`
and `input_analysis.py`; generation emits `fov-hooks.json` and `input-hooks.json`.
See the [investigation guide](../../docs/field-of-view.md).

The setter is at `0x70004544`, scoped to viewport, zoom and level-render returns
`0x7f086fcc`, `0x7f07e730` and `0x7f0be510`. Native player FOV/zoom fields remain
unchanged, preventing compounding across updates. The
[pinned camera reference](https://github.com/n64decomp/007/blob/c4356466796c697dfd298010b9bed261f9ed8c6a/src/game/bondview.c)
guided identification; all runtime addresses and operands were checked against
this ROM independently.

Earlier FOV versions passed ROM/translation checks and the user's gameplay test.
Linux Debug/Release builds also passed on 2026-11-03. Full-range keypad adjustment
and broader aiming/zoom transitions remain unverified.

## Dual-stick gamepad and widescreen

Single-player on-foot controls use the left stick for movement/strafe and the
right stick for horizontal/vertical look. LT fires; RT or RB aims; A changes
weapons, X uses/reloads and Start opens the watch. Menus retain native navigation.
Steam Deck packages enable these controls and disable mouse capture by default.
On PC, release mouse capture with Escape before using the gamepad. Set
`RECOMP_GOLDENEYE_GAMEPAD=0` to use the saved native controller style instead.

The adapter temporarily selects native 2.2 Galore (index 5) inside the verified
`MoveBond` input call, routing primary axes to look and the checked secondary
`joyGetStickX/Y` reads to movement. The original functions still execute once.
Primary R becomes secondary Z for native aiming. The selected control style is
restored after the call, without changing the save or exposing another player.
Watch, death, tank, scripted cameras and multiplayer retain the native path;
controller/mouse replays do not inherit physical gamepad look.

GoldenEye's backgrounds are CPU-projected RDP triangles. Independent RT64
projection expansion left these clipped to the old central region. F7 now uses
the native player/VI aspect setters (`0x7f09b224`, `0x70004590`) at the four
ROM-checked viewport call sites. Both CPU visibility/scale and GPU projection
receive the same native aspect, scaled for the window and the watch's existing
4:3/16:9 choice. Native top/bottom letterboxing and zoom are retained. RT64 then
stretches the completed anamorphic framebuffer at scanout, with projection
expansion disabled. Other games retain their existing RT64 aspect behavior.
Menus, watch and excluded cameras retain original presentation. The HUD follows
the native framebuffer layout, so it stretches horizontally in wide gameplay.

ROM fingerprints, call/delay words and field operands pass generation checks.
Linux Debug/Release and the Debian 12 Steam Deck build pass. A live Dam virtual
gamepad test verified right-stick turning/pitch without movement and left-stick
strafing/walking without turning, with control-style restoration. Widescreen
screenshots at 16:10 and 16:9 show background coverage at both edges, and F7
reset/watch return were exercised. Broader level/portal transitions,
weapon zoom and physical Deck control acceptance remain to be tested.

## Performance investigation

A confirmed 25-second slow-Dam capture on 2026-11-03 measured 45.7% of one
logical CPU in `n64-main` and 37.8% in the owner, with about 19,000 voluntary
switches/second on each. GPU busy samples averaged 38.2%, with median 8% and
brief 100% spikes. The workload is serialized between guest and owner, so low
average utilization does not establish spare execution time. The earlier
33.9% total-CPU capture had an unconfirmed scene and is not the comparison.

`runtime.checkpoint_interval = 512` batches ordinary guest backedges instead of
using the shared default 64. Explicit polling-loop yields and priority
preemption retain their immediate paths; the VI clock and 60 Hz presentation
deadline stay unchanged. This reduces repeated context serialization and
device polling in GoldenEye's CPU render/visibility work.

Equivalent desktop Release title-sequence runs at seconds 25..40 measured
CPU consumption of 0.193 versus 0.149 logical cores (about 23% lower), and
6,538 versus 1,055 scheduler dispatches/second (about 84% fewer). Display
callbacks remained about 60/second. These are callback/CPU measurements, not
unique game-frame or compositor FPS. Debug/Release and Deck builds pass;
the user still observed drops below 40 FPS on Deck after this change.

A second confirmed 25-second Dam capture measured 46.7% of one logical CPU in
`n64-main`, 26.8% in the owner, and roughly 4,500/5,100 voluntary switches/second
respectively. GPU busy averaged 28.4% (median 5%). The Deck's gamescope FPS limit,
dynamic refresh and reported display refresh were all 60. The two captures did
not lock the camera to identical coordinates, so they establish reduced
scheduler overhead and a remaining bottleneck, not a gameplay FPS gain.

In the second CPU sample, `mips_fp_binary` accounted for 17.63% of user CPU
samples directly. The background portal routine at `0x7f0b7f84` accounted for
13.12% including its callees; those costs overlap. The exact-US-address
[background reference](https://github.com/n64decomp/007/blob/c4356466796c697dfd298010b9bed261f9ed8c6a/src/game/bg.c)
identifies portal traversal. The
[collision reference](https://github.com/n64decomp/007/blob/c4356466796c697dfd298010b9bed261f9ed8c6a/src/game/stan.c)
identifies other sampled routines `0x7f0b0e24` (`stanTestLineUnobstructed`) and
`0x7f0b18b8` (`stanTestVolume`). These observations support a CPU geometry and
visibility bottleneck; low aggregate CPU/GPU utilization does not rule it out.
The checked ROM remains the US image declared in `profile.toml`.

Release enables link-time optimization for GoldenEye and its runtime when the
toolchain supports it. Clang also specializes the binary FP helper at translated
call sites, where the operation, format and register indices are constants.
Strict floating-point flags, rounding, exceptions and host-state restoration
remain enabled. Debug and other games retain the ordinary helper. This targets
arithmetic overhead without changing portal visibility or collision behavior.
A temporary differential check matched 1,520,000 arithmetic/conversion/compare
cases and 647,970 traps with Clang 22 and the Debian Clang 14 Deck compiler,
including all rounding modes and MXCSR/x87/errno restoration. A 40-million-call
normal-operand arithmetic microbenchmark with Clang 14 took 1.053 seconds before
and 0.729 seconds after specialization on the desktop CPU. This is not a Deck
FPS measurement; the title sequence changed little. The Deck itself passed
600 frames with its embedded pack and the same 1,520,000-case differential check,
preserving the player's save byte-for-byte. Its single arithmetic benchmark
used 1.459 versus 1.306 seconds of CPU time (about 10% lower). The specialized
build is installed, but the remaining slow Dam view still requires gameplay
comparison.

See [frame timing capture](../../docs/profiling.md#frame-pacing-and-stutter).
