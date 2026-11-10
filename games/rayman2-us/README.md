# Rayman 2: The Great Escape (US revision 0)

This fork uses the C11 GLES2 renderer and statically recompiled RSP graphics
programs directly. Existing presets/workflows need no renderer flags. See the
[graphics notes](../../docs/armv8-gles2.md) for this ROM's acceptance boundary.

This port targets the 32 MiB big-endian US revision 0 cartridge, header title
`Rayman 2`, country `0x45`, entry `0x80000400`:

- SHA-1: `50558356b059ad3fbaf5fe95380512b9dceaaf52`
- MD5: `03aa4d09fde77eed9b95be68e603d233`

Bring-up is in progress. Debug runs the original boot, DMA asset loading, audio
tasks, controller polling and scene scripts. A 1,200-presentation Release run
reaches the title and rendered language selection and exits cleanly. Rendered
gameplay is not yet verified. The checked callback inventory remains partial; unknown paths fail
explicitly rather than silently executing an unverified target.

## Keyboard, mouse and field of view

Both builds include these controls by default:

| Input | Action |
| --- | --- |
| WASD | Move; hold Shift to walk |
| Space / E | A: jump, swim or confirm |
| Left mouse button | B: shoot/charge |
| Mouse left/right | Turn the native third-person camera |
| Hold right mouse button + move mouse | Native C-up look mode, horizontal and vertical look |
| Ctrl | Z: strafe |
| F / down arrow | C-down: recenter camera |
| Q / R / left/right arrows | C-left / C-right camera controls |
| Up arrow | C-up look mode |
| Enter / Tab | Confirm / Start (pause) |
| Escape / F4 | Release mouse / recapture mouse |
| F10 | Quit |
| Keypad + / − | Widen / narrow horizontal FOV, original (~85.94°) through 140° |

Third-person mouse turning uses the game's original C-button camera actions;
vertical mouse look uses its native look mode while holding right click.
Any horizontal movement now reaches the next guest input update immediately,
without a movement threshold or queued turns. Turning uses the native digital
camera speed and stops requesting turns when the mouse stops; it is not a
proportional free-orbit camera.
Camera collision, pitch limits and scripted control restrictions remain under
the original game logic. WASD and arrow-key C-buttons remain available with the
mouse released. Mouse capture requires the active world camera, running scene
phase 9 and live controller mode; menus and the game's recorded demos release it.
Native gamepads remain available. Set `RECOMP_RAYMAN2_MOUSE=0` for the original
shared keyboard/gamepad mapping. Debug `--input` replay disables live overrides.
`RECOMP_MOUSE_SENSITIVITY` scales right-click analog look (default `0.08`, range
`0.001..10`). `RECOMP_MOUSE_INVERT_X=1` reverses horizontal mouse control in both
modes; `RECOMP_MOUSE_INVERT_Y=1` reverses look-mode pitch. These settings avoid
source edits when changing direction. Mouse deltas collected before a right-click
mode change are discarded so they cannot turn the other mode.

FOV starts at the exact original path every launch. Lowering to the minimum
restores that path. The override scales the tangent of the native horizontal
angle, preserving native zoom, and affects only the first active world camera
with perspective type 103. Separate menu cameras retain their original view.
F7 aspect expansion remains independent.

The ROM-bound wrapper at `0x8009989c` temporarily widens camera `+0x64` before
the original setup runs once, rebuilding focal length `+0x74`, projection
scales `+0x78/+0x7c`, clipping planes `+0x98..+0xd4` and the fixed GPU matrix.
Sphere visibility at `0x800894ac` reads those same planes; the plane transform
at `0x80089810` uses them too. The `guPerspective` call at `0x80099edc` takes
vertical radians converted to degrees and viewport aspect. The native source
angles `+0x64/+0x68` are restored afterward to avoid compounding script zoom.
World projection scales and CPU visibility therefore change together.

Camera initialization at `0x8002d5b0` establishes horizontal 1.5 radians and
vertical/horizontal ratio 0.75. Camera records at `0x800e5fc4` distinguish the
world camera from the separate menu camera at `0x800e5fc0`. Input is injected
after the original dispatcher at `0x8009ff70`, before the original deadzone and
button processing in `0x800a3bb0`. Full bodies and decoded operands are checked
by `controls_analysis.py` during generation; `controls-hooks.json` in the build
directory reports static verification and does not claim gameplay acceptance.

The pre-change bounded Debug desktop replay reached the rendered opening
sequence. Gameplay movement, camera direction/look-mode behavior, scene edges,
pause return and combined F7/FOV behavior need user testing with these changes.

From the repository root:

```sh
.venv/bin/python tools/import_rom.py --profile games/rayman2-us/profile.toml \
  --rom '/path/to/Rayman 2.z64'
cmake --workflow --preset rayman2-us-debug
./build/rayman2-us-debug/bin/rayman2-us --frames 360
```


## Verified loading model

| Section | Cartridge bytes, end exclusive | Guest address |
| --- | --- | --- |
| Boot | `0x1000..0x1dcc0` | `0x80000400` |
| Main | `0x1dcc0..0xc5bf0` | `0x80025c50` |
| Support | `0xc5bf0..0xd0a20` | `0x800f64a0` |

The entry clears BSS `0x8001d0c0..0x80025c50`, then jumps to `0x80000450`.
Its main thread at `0x800004c0` creates the game and pre-NMI threads. The game
thread at `0x80000854` uses the original PI DMA path to load main and support.
The cartridge's disabled compressed-load branch remains translated; no analysis
image preparation or guest interpreter is needed for the observed startup path.

Main CPU text ends at `0x800c6d98`, followed by alignment and initialized data.
Runtime executable visibility tracks through `0x800c6da0`, while discovery also
maps the data containing checked tables. The original asset loader uses
`0x800ca140` as an eight-byte scratch buffer. Including that buffer in runtime
code tracking incorrectly unloaded the main section after a legitimate asset DMA.

Synthetic boot-thread storage is retired at mainproc's first call to
`0x80002690`, before the game's heap initialization can reuse upper RDRAM.
The original main thread has priority zero but continues game work when a flag
is set, so it is not configured as a permanently idle thread.

## Checked discovery

`profile.toml` contains all mappings, bootstrap parameters and dispatch tables.
Shared discovery traces callback arguments through translated consumers. It
recovered `0x8000c118` from `0x8000c0c0`, which passes it as `a0` to the formatter
at `0x80013f00`; that routine invokes it through `s4`. This verified entry and its
caller/consumer evidence are retained in the checked `startup-entries.json`.
`startup-entries.json` is deliberately partial and ROM-bound. Its callback roots
come from these producers:

- `osCreateThread` sites at `0x800004a0`, `0x8000064c`, `0x80000688`,
  `0x8008f9f4`, `0x8008fa24` and `0x8009f59c`.
- Audio constructor `0x800ae4fc` passes `0x800af120` through `0x80004a10` and
  `0x80006890` to `0x8000dc44`. That callback returns `0x800aef7c`, stored in the
  audio object at `+0x30` for later indirect invocation.
- `0x8009e1f0` installs the three callbacks `0x8009e230`, `0x8009e27c` and
  `0x8009e2dc` at globals `0x800c9440..0x800c9448`.
- `0x80082f2c` calls the command-handler table at `0x800c8b3c`. Slot zero is
  null; the remaining 29 entries are separate callback bodies.
- Audio objects install methods through `0x8000fb20` at offsets `+4` and
  `+8`; the sample decoder's `+0x28` method is installed at `0x8000d750`.
  `0x800ad4cc` installs the three original default audio callbacks. Audio effect
  constructors `0x800b1d8c`, `0x800b21bc`, `0x800b2530` and `0x800b259c`
  install methods at object `+0x14` / `+0x1c`; `0x800b16bc` and `0x800b358c`
  install the audio service callbacks at `0x800ca168` / `0x800ca1c0`.
- `0x800f964c` constructs its complete 40-slot support method table in BSS
  `0x801012ec..0x8010138c`; the verified methods lie in
  `0x800f99ec..0x800f9f90`. The consumer is the indirect call at `0x800f997c`.
- Lifecycle calls at `0x8002a190` and `0x8002a2c0` select fields 4 and 5 of
  eleven six-pointer records at `0x800c6de0`. The loop bounds are eleven.
- Resource methods at `0x800b5014`, `0x800b5080` and `0x800b50f4` use fields
  2, 5 and 3 of six-pointer records at `0x800ca1d0`. Type 0 and unused types
  contain null callbacks; the type range 1..11 is checked by `0x800ad8d0`.
- The mutable scene callbacks at `0x800c6db0..0x800c6dbc` initially contain
  `0x80029658` and three copies of the original empty callback `0x80026370`.
  `0x800263c8` replaces the last two with `0x80027718` / `0x80027b98`;
  `0x80026a8c` invokes them. These are roots rather than an immutable dispatch table.
- `0x80035f4c` constructs thirteen object callbacks (`0x80035578..0x80035d40`)
  in a stack record. `0x800adf44` copies that record to `0x800c95ec..0x800c961c`;
  `0x800ac41c` invokes its first twelve methods to obtain object parameters.
- `0x80045440` dispatches the script node's type byte at `+7` through the
  41-pointer table `0x800c7238..0x800c72dc`. This supplies the original missing
  handler `0x800450d4`. Its primitive getter `0x8004c9b4` indexes 330 eight-byte
  function/flag records at `0x800c7bc0..0x800c8610`; only the function fields are
  registered, including the next observed target `0x8005fcbc`.
  The related getters use 74 function/flag records at `0x800c7340`, 174 at
  `0x800c7590`, 19 function pointers at `0x800c7b00`, and 28 at `0x800c7b50`.
  Padding and flag fields are excluded. The profile also pins each primitive's
  switch table to its original unsigned bounds check and complete table hash.
- `0x80045264` and `0x80045230` use the getters at `0x8004c8d4` /
  `0x8004c8e8` to select two callback fields from four sixteen-byte records at
  `0x800c7300`. The remaining fields are type tags and padding, not code.
- Scene setup `0x80029428` calls `0x8002883c` with `0x800e3f28` to install
  the two update methods `0x800721b8` / `0x80073400`. The dispatch at
  `0x80028ac8` selects them using the object's type returned by `0x80073010`.
- Field read/write dispatch at `0x800444cc` / `0x80044930` uses the two method
  fields in 24 twelve-byte records at `0x800c8610..0x800c8730`. Type tags are
  excluded. Lifecycle construction at `0x8003fd18` / `0x8003fd4c` uses fields
  2 / 0 of the eleven-record lifecycle table; `0x8003fe7c` uses cleanup field 4.
- `0x80029dc8` installs the ten-method object parameter interfaces beginning at
  `0x80029830` and `0x80029d1c` through `0x800adfb4` / `0x800ae1ec`.
  `0x800acdf4` reads those dynamically allocated 72-byte method records. The
  same constructor registers the callback `0x800348e0` through `0x800ae324`.
- Property access at `0x800612f0` / `0x800611e8` selects read/write methods
  from eleven three-word records at `0x800c87f0..0x800c8874`. The conversion
  calls at `0x80061310` / `0x800611dc` use five records at
  `0x800c8874..0x800c88b0`. The first word in each record is a type tag.

Ordinary SDK calls use full-body fingerprints in `tools/symbols.json`.
Runtime loaded-section checks and registered indirect-call lookup remain enabled.

```sh
.venv/bin/python tools/generate_game.py --game rayman2-us \
  --output build/rayman2-us-report --report-only
```

Debug supports `--trace-calls`, `--input FILE`, `--wav FILE` and
`--capture-every N`. Replays use `frame buttons stick_x stick_y` text rows.
The shared crash reporter writes failure JSON and word-swapped RDRAM under
`local/crashes/rayman2-us/`.

For a failure, collect evidence before rebuilding:

```sh
.venv/bin/python tools/diagnose_rom.py --game rayman2-us \
  --metadata build/rayman2-us-debug/generated/rayman2-us/symbols.json \
  --crash local/crashes/rayman2-us/crash-ID.json \
  --output build/rayman2-us-report/crash-ID
```

See [the model debugging workflow](../../docs/rom-diagnostics.md).

## Public research references

Research checked on 2026-10-19 found these useful starting points; none is a
build dependency or a substitute for this ROM's checked metadata:

- [Raymap](https://github.com/byvar/raymap), inspected at revision
  `2df138379acaa5c9d74ae275c6690ba8e2e3e52e`, includes N64 data loading and
  [ROM script parsing](https://github.com/byvar/raymap/blob/2df138379acaa5c9d74ae275c6690ba8e2e3e52e/Assets/Scripts/OpenSpace/ROM/Structs/AI/ScriptNodeArray.cs).
  It distinguishes packed cartridge nodes from expanded script nodes and
  separates node kinds, parameter types and primitive-name tables. The expanded
  node reader's parameter/indent/type layout agrees with this ROM's dispatch
  reads at offsets 0, 6 and 7. Its name-table lengths differ from our verified
  native tables; numeric names and IDs have not been imported.
- An indexed README for
  [another N64 recompilation attempt](https://github.com/danielgomesvieira2000/rayman-2-the-great-escape-recomp)
  targets the same cartridge SHA-1 and reports the same three segment addresses
  and graphics microcode identifier. The repository and detailed findings could
  not be retrieved, so this is cached corroboration, not a verified symbol map.
  Its reported thread-state bug prompted a local check: our scheduler already
  writes libultra's guest state bits 1/2/4/8, separately from host thread enums.
- [ACP_Ray2](https://github.com/raytools/ACP_Ray2) documents PC engine types and
  functions. It can supply conceptual hints, but PC addresses and layouts are
  not assumed to match the N64 cartridge.
