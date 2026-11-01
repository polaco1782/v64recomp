# Finding and changing a game's field of view

This is the investigation procedure used for Hexen, GoldenEye and Duke Nukem 64.
It is intended for an agent extending another game in this repository. Run the
commands from the repository root, substitute the game ID and use metadata from
that exact ROM. On Linux, replace `.venv/Scripts/python.exe` with
`.venv/bin/python`.

The deliverable is an optional, default-preserving camera setting with checked
ROM evidence, build integration and honest validation notes. A wider matrix
alone is insufficient: CPU visibility, portal traversal, projection helpers and
weapon zoom may depend on the original FOV.

## 1. Establish the existing implementation

Read `AGENTS.md`, `SKILL.md`, the game's README, `profile.toml`, `generate.py`,
both `app.c` entry points and any existing graphics/input adapters. Preserve the
working tree and private ROM/save files. Do not assume two games use the same
addresses, engine or ABI.

Generate analysis metadata through the game's normal preparation path:

```powershell
git -c diff.ignoreSubmodules=all status --short
.venv/Scripts/python.exe tools/generate_game.py --game duke-us --output build/duke-fov-analysis --report-only
```

Use a dedicated output directory. Read `symbols.json` and `coverage.json`.
Confirm the ROM identity, section VRAM/ROM mappings, function ownership and
translation status. For compressed games, these tools prepare the analysis
image; original cartridge offsets may differ from analysis offsets. A previous
game's output or an external symbol map is not a substitute.

## 2. Find semantic candidates

Search public primary sources for the exact game: decompilations, disassemblies,
SDK source and engine source. Pin the reference revision. Names and comments
help interpret a routine, but every address and operand used by the adapter
must be checked against the local ROM. A reference containing `GLOBAL_ASM`
declarations supplies candidates rather than a C implementation of those bodies.

For a libultra renderer, start with the maintained SDK inspector:

```powershell
.venv/Scripts/python.exe tools/inspect_sdk_symbols.py --game duke-us --metadata build/duke-fov-analysis/symbols.json --symbol guPerspective --symbol guPerspectiveF --symbol guFrustum --symbol guFrustumF --output build/duke-fov-sdk
```

Read `report.txt` and its caller evidence. These are identification candidates;
CRC resemblance does not authorize a replacement. A custom renderer may instead
construct a matrix directly, use a focal-length scalar, or build screen-angle
tables. If there is no SDK match, follow the world draw path and matrix producers
with the same disassembly/caller tools. Do not select a similarly named function
from an unrelated ROM.

## 3. Trace callers and arguments, including delay slots

Disassemble the candidate and its caller using verified instruction ownership:

```powershell
.venv/Scripts/python.exe tools/disassemble_function.py recomp_s0_800c3130 --game duke-us --metadata build/duke-fov-analysis/symbols.json --arch mips
.venv/Scripts/python.exe tools/inspect_callbacks.py --game duke-us --metadata build/duke-fov-analysis/symbols.json --target 0x800c3130 --target 0x800c3360 --before 20 --after 8
.venv/Scripts/python.exe tools/disassemble_function.py recomp_s0_80063bb8 --game duke-us --metadata build/duke-fov-analysis/symbols.json --arch mips
```

Work backwards from each `jal` through argument construction and forwards from
the result through its consumers. The instruction after `jal` executes before
the callee; its argument stores must be included. A normal return address is
`call_pc + 8`. The tools print bytes and mapped offsets; use those to record the
evidence in the profile.

Determine all of the following before editing behavior:

- Which calls build the world camera, menu/logo camera, weapon view or another
  player's viewport? How does the matrix reach the RSP display list?
- Is the FOV vertical or horizontal? What aspect ratio and angle units are used?
  Float `60.0` is `0x42700000`; its appearance is a clue, not proof of meaning.
- What is the guest calling convention at this particular boundary?
  `guPerspective[F](matrix, norm, fovy, aspect, near, far, scale)` carries the
  third/fourth float arguments as bits in `a2`/`a3` under the observed o32 ABI;
  later arguments are on the guest stack. A one-float game setter can instead
  receive its argument in `f12`, as GoldenEye does.
- Does a wrapper call another wrapped function? Avoid transforming the same
  value twice. Track the parent return address if the inner SDK call is shared.

Decode address operands correctly. `lui t6, 0x8010; lh t6, 0xf624(t6)` loads
from `0x800ff624`, because the load offset is signed. It does **not** load from
`0x8010f624`. An `ori` low half is unsigned. Bind the instruction words and
decode them in generation so an incorrect transcribed address fails early.

## 4. Follow the CPU side of the camera

Trace the projection result and the world render path until visibility decisions
are understood. Search reference names such as perspective, camera scale,
angle-to-screen, clip angle, `drawrooms`, `scansector` and portal traversal.
Disassemble the actual consumers. Determine whether the CPU uses:

- the same float projection or combined view/projection matrix as the GPU;
- camera scale/reciprocal values derived from FOV;
- angle-to-screen or tangent lookup tables;
- a separate angular sector/portal window.

Keep CPU and GPU inputs consistent. Preserve original allocation, clipping,
rounding, visited lists and recursive traversal. For a sector scanner, widen the
initial camera window; do not expand every recursively clipped portal window.
Preserve its native pitch allowance and angle-wrap conventions. Check limits
before increasing table ranges or assuming an angular span can grow arbitrarily.

Check world sprites separately from polygon geometry. A billboard can project
its anchor with the matrix while deriving its screen size from depth and a
hard-coded focal scale. Changing the matrix then moves its anchor correctly but
leaves it too large relative to the widened world. Trace the size arguments to
the tile/billboard draw call and apply the focal-length ratio
`tan(stock_vertical/2) / tan(adjusted_vertical/2)` only for world callers.
Verify both width and height and retain the projected anchor. The same tile
routine may draw weapons, HUD and menus; those calls must keep their own scale.

For a matrix in BSS, the callback inspector's mapped-code target search is not
a RAM-reference search. Search generated Debug instruction comments for the
address's low-half operand, then verify the preceding `lui`, register data flow
and containing function with the disassembler. For example, Duke's matrix at
`0x8026ec70` has signed low half `-0x1390`:

```powershell
rg -n -C 4 -- '-0x1390' build/msvc/duke-us/debug/generated/duke-us
.venv/Scripts/python.exe tools/disassemble_function.py recomp_s0_8006cb30 --game duke-us --metadata build/msvc/duke-us/debug/generated/duke-us/symbols.json --arch mips
```

Many unrelated addresses can share that low half. In this consumer,
`0x8006cb5c`/`0x8006cb60` construct the full matrix pointer; loads beginning at
`0x8006cb6c` feed CPU multiply/add calculations. Generated comments are a useful
index, while the ROM-bound disassembler verifies the owning instructions.

The three existing implementations illustrate different boundaries:

| Game | Projection boundary | CPU visibility handling |
| --- | --- | --- |
| Hexen US | `guPerspective`, restricted to the world return PC and matrix | Temporarily supplies a wider conservative scalar to the original angle-to-screen initializer, then restores it |
| GoldenEye US | Gameplay calls to `viSetFovY` | Original setter updates VI projection and player perspective/scale together |
| Duke Nukem 64 US | World calls reaching `guPerspectiveF`, both directly and through `guPerspective` | Both CPU float and GPU fixed matrices change; widen the initial sector window and correct depth-derived world billboard sizes |

## 5. Choose conversion and scope

State the setting's convention explicitly. These ports use horizontal degrees
at a documented reference aspect, accept `60..120`, and keep the exact original
path when the environment variable is unset, empty or `0`.

For a fixed horizontal setting `h` and aspect `a`, the vertical angle is:

```text
v = 2 * atan(tan(h / 2) / a)
```

Use radians inside trigonometric functions. If the game supplies native zoom,
preserve its optical magnification with a tangent scale rather than overwriting
every incoming angle with the same value:

```text
scale = tan(requested_horizontal / 2) / (reference_aspect * tan(stock_vertical / 2))
adjusted_vertical = 2 * atan(tan(incoming_native_vertical / 2) * scale)
```

Keep native zoom fields unchanged so later updates cannot compound the override.
Scope hooks with verified return addresses, matrix identities or camera state.
Decide how multiplayer, menus, watch screens, death and scripted cameras should
behave; verify any state globals used for that decision. RT64 F7 aspect expansion
is separate from this reference-angle conversion and needs combined testing.

## 6. Store evidence and integrate the setting

Keep all game-specific work under `games/<game>/`:

1. Declare full-body `function_wrappers` with section, VRAM, size, SHA-256 and
   handler. Wrappers retain the translated original and execute it once.
2. Put reviewed functions, call/delay pairs, operands and reference revision in
   the existing complete `profile.toml`; do not add a second profile chain.
3. Add a game-owned verifier called from `generate.py` preparation and reporting.
   Check full bodies, decoded operands, metadata ownership/boundaries and
   translated coverage. Emit constants into a generated header and a
   `fov-hooks.json` report under the selected build directory.
4. Implement parsing/math/wrappers in parent-folder `fov.c`/`fov.h`. Parse once
   at startup in both apps. Reject malformed, nonfinite and out-of-range values.
   With the setting disabled, pass straight through to the original.
5. Add `fov.c` to `RECOMP_GAME_SOURCES`; use `RECOMP_GAME_DEBUG_SOURCES` for any
   maintained focused check. Regenerate the game's MSVC browse project using
   `tools/msvc_solution.py`'s `write_project` interface. Do not edit generated C.

A hash binds reviewed semantics to bytes; it does not establish those semantics
by itself. A report must keep `runtime_verified=false` when it only checks ROM
evidence. Look at the existing game modules for the current schemas and APIs.

## 7. Validate and hand off

Build Debug and Release. Verify unset/empty/zero preserves original projection
bytes. Exercise range endpoints, scope exclusions, repeated updates and zoom
ratios where applicable. Prefer actual translated projection math when it can
run in isolation. Clearly label observer-based visibility checks as synthetic;
they cannot establish portal traversal or edge visibility in a live level.

For Duke on Windows:

```powershell
.\tools\build_msvc_project.ps1 -Game duke-us -Configuration Debug
.\tools\build_msvc_project.ps1 -Game duke-us -Configuration Release
$env:RECOMP_DUKE_CHECK_FOV = '1'
.\build\msvc\duke-us\debug\bin\Debug\duke-us.exe --frames 1
Remove-Item Env:RECOMP_DUKE_CHECK_FOV
git -c diff.ignoreSubmodules=all diff --check
```

In-game acceptance should cover level geometry/sprites at both screen edges,
pitch up/down, turning across angle wrap, portal/room transitions, aiming and
weapon zoom, pause/menu return and the original-FOV reset. Include combined
widescreen behavior when relevant. If the user is doing gameplay testing, hand
off the rebuilt executable and record that acceptance gap without claiming it
passed. Do not replace or interrupt a game the user is already running.

Record findings and limits in the game's README and support status. Link the
pinned sources used, remove task-created scratch scanners/downloads once their
findings are preserved, and keep ROMs, saves and generated output ignored.

## Duke investigation record

The ROM is US revision 0, SHA-1 `98d6778004becc672eba0a5e887f6e3f3d1b5c15`.
The mapping is `.main`: VRAM `0x8004b400`, ROM offset `0x1000`, size `0xbf880`.
The [NBlood reference at the reviewed revision](https://github.com/NBlood/Duke64-RE/tree/7ec429c4ba37245da5a5671a019e911b22ff318c)
supplied SDK symbols and the `drawrooms_`/`scansector` names; the relevant game
bodies are mostly assembly declarations. Local disassembly supplied the camera
data flow and operands. Full fingerprints and instructions are in
`games/duke-us/profile.toml` under `fov_adapter`.

| Evidence | Meaning and next trace |
| --- | --- |
| SDK inspector finds `0x800c3130` / `0x800c3360` | Candidate `guPerspectiveF` / `guPerspective`; disassemble bodies and callers |
| `0x80063cc8` calls F, `0x80063d20` calls fixed | Same camera setup builds a float and a fixed projection; both receive native vertical 60 and aspect bits `0x3faaaaab` |
| `0x800c3394` calls F inside fixed | A single F hook covers both paths; saved `ra` at stack `+0x24` identifies the fixed routine's world parent (`0x80063d28`) |
| `0x80063f44` calls `guMtxCatF` | Camera combines its float view and projection into a CPU matrix; changing only the fixed matrix would miss this path |
| `0x8006cb5c`/`0x8006cb60` construct `0x8026ec70` | CPU consumer at `0x8006cb30` loads and multiplies its matrix elements, confirming the float matrix is used outside the RSP |
| `0x8006cf00` calls `rendertileworld` (`0x8006cf8c`) | World sprite sizes in `a2`/`a3` come from depth, bypassing the focal scale; compensate at return `0x8006cf08` |
| `0x800659d8` calls the same tile routine | World effects need the same correction; other callers include HUD/weapon/menu tiles and are excluded |
| `0x8005bf9c` calls camera setup | Per-player renderer establishes the camera before world rendering |
| `drawrooms_` at `0x80059dec` | Computes initial yaw window using 35 degrees plus native pitch allowance for one/two players; other split-screen layouts use a different base |
| `0x80059fe0` calls `scansector` at `0x8005962c` | Initial bounds are float arguments `f12`/`f14`; scope the widening to return `0x80059fe8` |
| Scanner recursively calls itself at `0x80059c84` | Leave recursive portal bounds unchanged |
| `LUI 0x8010` / `LH 0xf624` in drawrooms | Player count is at `0x800ff624`; restrict the optional override to one player |

The chosen CPU padding is `max(0, requested_horizontal/2 + 1 - 35)` degrees on
either side of the original initial window. It preserves native pitch overscan
and never narrows visibility. This is a conservative camera-window adjustment;
live tilted views, complex portal scenes and wider rendering workloads require
gameplay acceptance. Follow the README for current build/test results.

The first FOV change omitted the separate billboard size path. The user reported
larger-looking aliens at FOV 110. Disassembly showed the depth-derived sizes
above; the correction now applies the same focal ratio as the actual translated
projection matrix, about 0.54 at 110 degrees. Debug checks cover that angle,
both world callers, unchanged anchors and HUD exclusion. They establish the
argument scaling; the user subsequently confirmed the corrected FOV and input
work well in game. Broader camera and portal cases remain unverified.
