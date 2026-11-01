# Duke Nukem 64 US

Original menus and first level with RT64 graphics, ABI1 audio and live input. Full-game progression remains unverified.

Single-player WASD/mouse controls are enabled by default. See the
[controller guide](controller.md) for bindings, settings, ROM evidence and
validation. `RECOMP_DUKE_MOUSE=0` restores the ordinary controller path.

From the project root, import the pinned ROM using
`tools/import_rom.py --profile games/duke-us/profile.toml --rom /path/to/rom`.

```sh
cmake --workflow --preset duke-us-release
./build/duke-us-release/bin/duke-us

cmake --workflow --preset duke-us-debug
./build/duke-us-debug/bin/duke-us --help
```

## Field of view

`RECOMP_DUKE_FOV` optionally sets the single-player world camera's horizontal
FOV at the native 4:3 aspect. The range is `60..120` degrees, including fractional
values. Unset, empty or `0` retains the exact original projections (60 degrees
vertically, approximately 75.2 horizontally) and visibility window.

```powershell
$env:RECOMP_DUKE_FOV = '100'
.\build\msvc\duke-us\release\bin\Release\duke-us.exe
```

The setting applies at launch in both Debug and Release. Restore the original
with `$env:RECOMP_DUKE_FOV = '0'`. Multiplayer keeps its original camera;
orthographic HUD/menu calls retain their original behavior. RT64 F7 aspect
expansion is separate and can widen the displayed view further.

The world setup at `0x80063bb8` builds both a float CPU projection and a fixed
GPU projection. `fov.c` wraps the original `guPerspectiveF` at `0x800c3130` for
the direct world call and the world call through `guPerspective`. The latter
checks the saved parent return address to avoid changing unrelated SDK calls.
The original routines still build the matrices and perspective normalization.

Duke also scans visible sectors using an angular window. The wrapper at
`scansector` (`0x8005962c`) widens only its initial call from `drawrooms_`, keeping
the native pitch allowance. The scan window is never narrowed; recursive
portal clipping, visited lists and sorting remain guest code.
Full bodies, calls, delay slots and decoded operands are checked against this
exact ROM by `fov_analysis.py`; generation writes `fov-hooks.json`.

World billboard sprites/effects also receive the projection's focal-length
ratio. Their native depth-based size calculation bypasses the projection matrix;
without this correction, enemies retain their stock screen size while the world
widens. The fix is scoped to the world calls to `rendertileworld`, preserving
weapon/HUD/menu tile calls. It is covered at FOV 110 by focused checks, and the
user subsequently confirmed that the corrected FOV and input work well in game.

See [the repeatable FOV investigation guide](../../docs/field-of-view.md) for
the tools, commands, pinned public reference and step-by-step Duke findings.

On 2026-10-31, Debug/Release builds and focused Debug checks passed. Those checks
execute the actual translated float/fixed projection math, comparing native
matrix bytes, requested horizontal angles, repeated updates and scope exclusions.
Visibility-window checks use a synthetic observer. To rerun without opening a
game window:

```powershell
$env:RECOMP_DUKE_CHECK_FOV = '1'
.\build\msvc\duke-us\debug\bin\Debug\duke-us.exe --frames 1
Remove-Item Env:RECOMP_DUKE_CHECK_FOV
```

Broader edge visibility, extreme pitched views, portal transitions and combined
widescreen behavior remain unverified beyond the user's gameplay test.

`release/` contains the playable app and Release-only sources. `debug/` contains
the original trace/capture harness and inspection tools. Shared headers, adapters,
`generate.py`, `profile.toml` and inventories live in this parent directory. Generated C stays under the selected
build directory, and private ROM/save data stays in `local/`.

See [build/run options](../../README.md), [debugging](../../docs/debugging.md),
and [support status](../../docs/status.md).
