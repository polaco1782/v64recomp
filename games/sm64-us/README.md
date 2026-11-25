# Super Mario 64 US

Working gameplay baseline, with Castle Grounds movement/jumping and the castle lobby transition previously validated. Broader courses, ending and prolonged runs still need validation.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/sm64-us/profile.toml --rom /path/to/rom`.

```sh
python3 tools/build.py sm64-us --release
./build/sm64-us-gles-release/bin/sm64-us

python3 tools/build.py sm64-us --debug
./build/sm64-us-gles-debug/bin/sm64-us --help
```

Shared renderer statistics distinguish displayed frames from host presentations;
Debug uses the common tracing, replay and capture options.

SM64 uses its statically recompiled RSP audio microcode with either renderer.
[Audio comparison and results](../../docs/audio.md#sm64-us-compiled-audio)

The live loop uses the shared wall-clock pacer to service VI/AI, timers and task
completion between display ticks. The profile's verified priority-zero idle
thread lets the scheduler wait for those events without running its busy loop.
This replaces a full-frame sleep and a game-specific queue-state predicate.

On 2026-11-09, the Debug startup check and Debian 12 Release build passed, and
the user confirmed steady 60 FPS while moving inside the castle on Steam Deck.
The earlier build also held 60 FPS after restarting, so the originally reported
30 FPS drop was not reproduced in that comparison. The pacing change improves
event servicing and host presentation cadence; it does not establish the cause
of that intermittent drop or broader course performance.

Configure either mode with `-DRECOMP_EMBED_ROM=ON` to include ROM content in the
executable and launch without the external ROM. SM64's complete checked inventory
lets packaging erase ordinary CPU text, native OS ranges and the unused IPL3
boot block while retaining data and RSP programs. See
[ROM packaging](../../docs/rom-packaging.md) for exact scope, validation, reports
and comparison with an original ROM through `--rom`.

`main.c` contains the shared entry point and lifecycle for Debug and Release.
Headers, adapters, `generate.py`, `profile.toml`, inventories and maintained
replay inputs live beside it. Generated code stays under the selected build
directory; private ROMs and saves stay in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).

Word shifts retain the fix for MSVC optimized sign extension that affected
packed physics parameters. The standalone physics regression harness and
scripted validation frontend have been removed; Debug now uses the common
frame loop, replay and capture options.

## Desktop controls

Move the mouse to orbit Mario horizontally or tilt the view up and down. The
camera follows Mario as he moves and pulls in when level geometry blocks the
view. WASD moves relative to the view, and Shift + WASD walks. The arrow keys
also move. Space jumps, left click punches or kicks, and Ctrl or right click
crouches. Tab sends Start. In menus, Enter sends A.

Escape releases the mouse and restores the native view. F4 recaptures the
mouse. For native camera commands while captured, Q/R send C-left/right, E
sends C-down, and F sends C-up. Cutscenes, pause, C-up look and cannon aiming
use the game's camera. A gamepad remains available.

`RECOMP_MOUSE_SENSITIVITY` sets degrees per mouse count (default 0.08).
`RECOMP_SM64_MOUSE=0` disables capture and enables the original I/J/K/L
C-button aliases. Debug controller replays use the native camera.

Keypad + / − adjusts horizontal field of view from the original up to 140°
at the 4:3 reference aspect. At minimum the original path is unchanged. The
US ROM's checked world camera callback updates the same graph-node FoV used by
projection and object culling, without overwriting the original game's zoom
state or applying to cutscenes. The browser retains its existing controls.

## Browser preview

See [WebAssembly build and validation](../../docs/webassembly.md) for the experimental Emscripten/GLES2 target.
