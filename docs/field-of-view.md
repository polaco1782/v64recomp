# Finding and changing a game's field of view

This is the investigation procedure used for Hexen, GoldenEye, Duke Nukem 64,
Ocarina of Time and Quake 64.
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

The existing implementations illustrate different boundaries:

| Game | Projection boundary | CPU visibility handling |
| --- | --- | --- |
| Hexen US | `guPerspective`, restricted to the world return PC and matrix | Saves native CPU angle-to-screen/clip tables, rebuilds them for wider live views, restores saved bytes at the minimum |
| GoldenEye US | Gameplay calls to `viSetFovY` | Original setter updates VI projection and player perspective/scale together |
| Duke Nukem 64 US | World calls reaching `guPerspectiveF`, both directly and through `guPerspective` | Both CPU float and GPU fixed matrices change; widen the initial sector window and correct depth-derived world billboard sizes |
| Ocarina of Time US V1.2 | Gameplay view FOV after `Camera_Update`, before view matrices are built | Shared CPU/GPU view-projection path; preserve native camera FOV/zoom and exclude fixed backgrounds |
| Quake 64 US | Saved fixed `guFrustum` matrix selected at world render, scale X/Y diagonals only | Temporarily reduce right/up basis vectors during original four-plane construction; retain distances, sign bits and conservative coverage |

## 5. Choose conversion and scope

Use the shared keypad `+` / `-` controls in both Debug and Release. Bind the
game's adjustment callback with `recomp_renderer_set_fov_hook`. The renderer
delivers one-degree keydown steps (including repeat) on the owner thread, with
guest execution parked. It ignores these shortcuts when keyboard input is
captured by the shared menu or Debug inspector, or the window lacks focus.
The shared `fov_control` service clamps horizontal degrees at a documented
reference aspect between the exact original angle and 140. Start at the original
each launch and preserve its exact guest path when lowering to the minimum.
There is no FOV environment lookup or startup parser.

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
4. Implement game math/wrappers in parent-folder `fov.c`/`fov.h` (or the
   game's combined control adapter, as in OoT). Initialize shared `RecompFov`
   state with the checked native vertical angle/aspect in both apps. Bind the
   adjustment callback after creating the renderer. Recompute projection,
   zoom, sprite and visibility scalars together; keep exact original behavior
   at the minimum. If CPU tables are cached, rebuild them live too.
5. Add the adapter to `RECOMP_GAME_SOURCES`. Regenerate the game's MSVC browse project using
   `tools/msvc_solution.py`'s `write_project` interface. Do not edit generated C.

A hash binds reviewed semantics to bytes; it does not establish those semantics
by itself. A report must keep `runtime_verified=false` when it only checks ROM
evidence. Look at the existing game modules for the current schemas and APIs.

## 7. Validate and hand off

Build Debug and Release unless the user requests source-only work. Verify launch
and lowering to the minimum preserve original projection bytes. Exercise keypad
clamping through 140 degrees, scope exclusions, repeated updates and zoom
ratios where applicable. ROM evidence alone cannot establish portal traversal
or edge visibility in a live level.

For Duke on Windows:

```powershell
.\tools\build_msvc_project.ps1 -Game duke-us -Configuration Debug
.\tools\build_msvc_project.ps1 -Game duke-us -Configuration Release
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
projection matrix, about 0.54 at 110 degrees. Earlier isolated runs verified
argument scaling for both world callers, unchanged anchors and HUD exclusion.
The user subsequently confirmed the corrected FOV and input
work well in game. Broader camera and portal cases remain unverified.

## Ocarina of Time investigation record

The NTSC 1.2 ROM has SHA-1 `41b3bdc48d98c48529219919015a1af22f5057c2`.
Its `prepare.py` resolves compressed `.code` and overlays before analysis;
`.code` starts at VRAM `0x800116e0`, analysis ROM offset `0x02000000`.
Original cartridge offsets must not be substituted into this prepared mapping.
The [zeldaret reference at the reviewed revision](https://github.com/zeldaret/oot/tree/14759abf9abb4295de142cdc1452998630ea70fb)
supplied camera/view names and layout hypotheses. Local disassembly verified
the following bodies, call sites and operands. Full fingerprints are in
`games/oot-us-v12/profile.toml` under `controls`; the combined adapter emits
`controls-hooks.json` and `oot_controls_config.h` through `generate.py`.

| Evidence | Meaning and next trace |
| --- | --- |
| `Play_Init` at `0x8009ae40` installs `0x8009d1b8` in `GameState.main` | Identifies live `Play_Main`, rather than assuming every game state is gameplay |
| `Play_Main` calls `Play_Update` at `0x8009d1d8` | Return PC `0x8009d1e0` scopes play-state discovery and live input refresh |
| `Camera_Update` at `0x80048c4c` begins by using `a1` as `Camera*` | `Vec3s` return uses a hidden output pointer in `a0`; treating `a0` as the camera corrupts state |
| Dispatch at `0x80049070` calls through `0x800ef6c0` | Full 71-entry table is fingerprinted; index 2 is `Camera_Normal1` at `0x80039054` |
| `Camera_Normal1` calls `Camera_BGCheck` at `0x800398a8` | Mouse orbit reuses the original helper at `0x800362fc`, with camera/from/to arguments and scratch registers |
| `Camera_Update` writes view FOV at `0x800493fc` | Adjust `PlayState.view.fovy` after the original update; keep `Camera.fov` native so zoom does not compound |
| `View_ApplyPerspective` at `0x80092620` loads view FOV at `0x800926f4` | It feeds `guPerspective` at `0x8009271c`; hooking the earlier view scalar avoids altering menus and unrelated SDK callers |
| Normal-camera parameter at `0x800ec808` is `(60, tag 7)` | Verifies the usual native vertical FOV of 60 degrees for tangent scaling |
| Settings table at `0x800ef4b0`, 66 entries | Its default-mode function IDs identify normal-family 3D settings; fixed/prerendered settings stay excluded |

Trace `Play_Draw` and actor projection/culling as well as SDK projection calls.
The original world view builds the shared `viewProjectionMtxF`; actor culling
uses coordinates projected through that matrix. World billboards follow the 3D
pipeline. This differs from Duke's independent depth-derived 2D tile sizes and
does not call for the same sprite scaling patch. Live room/edge visibility is
still an acceptance check, not a consequence established by a static report.

The adapter requires the normal game mode, health, active main camera and live
play-state fields, excluding pause, message, game-over, transition and cutscene
states. FOV uses the normal-family settings whitelist. Orbit is narrower: only
normal mode dispatched to `Camera_Normal1`, yielding to native Z-targeting.
Check the exact RAM offsets in the profile; for example `PauseContext.state` is
at `PlayState + 0x10934`, not the pause-context base `+0x10760`.

ROM fingerprints establish the reviewed boundaries; movement, aiming, collision
geometry and complete camera transitions require gameplay acceptance. Follow the
game's README for the build command and current validation limits.

## Hexen live visibility update

The original visibility initializer at `0x803bba90` allocates a temporary
4096-entry tangent table, builds fixed-point screen positions and a clip angle,
then frees the temporary buffer. Its fixed +/-2 tangent branches at
`0x803bbbf4`/`0x803bbc00` truncate coverage beyond `atan(2)`, about 63.4 degrees
on each side. Merely raising the FOV setting to 140 would leave missing edges.

The live adapter lets the original initializer run normally and saves its
finished 4096 words at `0x80144bf8` plus clip angle at `0x80144bdc`. The profile
checks the table-address LUI/ADDIU, clip-address LUI/SW, fixed-point screen center
(`0x47200000`, 40960) and upper bound (`0x47a00080`, 81921), as well as the full
initializer body. No additional ROM entry or instruction patch is needed.

Above the native conservative span, compute a focal scale from the selected
half-angle plus one degree, sample angles `(i - 2048 + 0.5) * 2*pi / 8192`, and
rebuild `ceil(center - tan(angle) * focal)` clamped to `[-1, 81921]`. The first
negative entry defines the original binary-angle clip formula
`(index << 19) - 0x40000000`. The screen bounds replace the fixed +/-2 branches,
so 140-degree views retain coverage. Host math can differ slightly from the
guest's trigonometric approximations; angular overscan keeps clipping
conservative. Returning to the minimum copies the exact saved native words back.

Key changes update only these CPU tables and projection scalars, with the guest
parked. They never re-enter the guest allocator. The ROM operands and an
analytical table model were reviewed during implementation. This extension was
not rebuilt at the user's request; live
geometry/edge visibility and combined F7 behavior still need acceptance.

## Quake 64 investigation record

US revision 0 has SHA-1 `25a7fe0342490d3c23328040bb34f8748995537a`.
Its `.main` maps VRAM `0x80000400`, ROM `0x1000`, size `0x77080`.
The SDK inspector found `guFrustum` at `0x800574f0`, not `guPerspective`.
The [pinned PC engine reference](https://github.com/id-Software/Quake/tree/bf4ac424ce754894ac8f1dae6a3981954bc9852d/WinQuake)
provided structural hypotheses only; the N64 layout and addresses came from
local disassembly. See the game's controller guide and complete `profile.toml`
for fingerprints and decoded operands.

| Evidence | Interpretation |
| --- | --- |
| `0x80028584` calls `guFrustum` | Left/right `-4/+4`, bottom/top `-3/+3`, near 4, far 4096, scale 1; original horizontal FOV is 90 degrees |
| `0x80028574` constructs `0x80078160` | Fixed world projection is initialized separately from per-frame view/culling work |
| `0x80027740`/`0x80027744` emit a matrix load | GPU task reads that global matrix later; restoring it at the end of a host wrapper would restore too early |
| `0x80028378` calls camera setup, plane setup and world drawing | Scope the chosen projection at this render boundary, preserving native views when gameplay is inactive |
| `0x80027378` constructs `forward +/- right` and `forward +/- up` planes | CPU clipping does not use the GPU matrix; widening only the matrix would clip newly visible geometry |
| `0x800274ac` calls the plane sign-bit helper | Let the original recalculate distances/types/sign bits, rather than editing plane normals afterward |
| `0x800260f0` loops over four planes | Confirms the CPU visibility consumer and exact plane span |
| `0x8002db24` emits Vtx/triangle geometry for effects | Effects follow the 3D projection; no independent Duke-style screen-size correction was identified |

Save the original 64 matrix bytes when the verified world `guFrustum` call
returns. For selected horizontal angle `h`, scale only fixed-point diagonal
entries 0 and 5 by `1/tan(h/2)`; keep the native depth terms. At minimum,
restore the original bytes exactly. Keep the selected matrix resident until a
later render boundary chooses another view, so the submitted RSP task sees it.

For CPU culling, temporarily scale right by `1/tan(h/2 + 1 degree)` and up by
`1/max(1, tan(atan(0.75*tan(h/2)) + 1 degree))`. The original vertical planes
already cover a conservative 90-degree span, wider than the native 73.74-degree
vertical projection; never narrow that span. Run the original plane constructor
once, then restore all six basis-component words. Rendering and billboard
orientation must see the original camera basis afterward.

The same game-owned adapter injects mouse/WASD after the controller dispatcher
at `0x800469d8`, restricted to gameplay return `0x8003b528`. This avoids changing
menu pad reads or replacing physics. See
[Quake's controller guide](../games/quake-us/controller.md) for command fields,
state gates, native speeds and weapon-edge behavior.

Do not treat `0x8006caa0` as a gameplay-mode flag. It is the difficulty selector,
initialized to 1 (Normal), with menu labels Easy/Normal/Hard/Nightmare. The first
adapter incorrectly required zero, disabling capture, injection and FOV on
Normal and higher difficulties. The gate was removed; difficulty is independent
of gameplay eligibility. Its address and original default are retained in the
ROM evidence as `skill` and `default_skill`.

Generation retained four translated originals with 1,402 ready functions and 58
SDK hooks. This is static evidence only: no standalone tests were added, and
MSVC Debug compiles and links. Release has not been rebuilt and the game has not
been launched. Gameplay and combined F7 acceptance remain.
