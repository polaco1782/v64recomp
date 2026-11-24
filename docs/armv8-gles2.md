# OpenGL ES 3.0 and compiled RSP graphics

Graphics display lists run through
the statically recompiled RSP program checked for the selected game. All
core runtime and generated graphics code are C11; Python performs generation.
The desktop texture reader and editor also use C11. The optional C11 cheat menu uses the C bindings for Dear ImGui. Audio uses its own recompiled RSP program.

Native builds require OpenGL ES 3.0 or later. Browser builds use
WebGL 2 and the same GLSL ES 3.00 shaders.

The backend lives in `backends/gles2/`: the renderer adapter services graphics
tasks; the RDP decoder batches draw commands; context, framebuffers, controls,
TMEM and combiner shaders live in separate modules. The editor, pack reader
and wireframe are separate from game code.

## Task execution

The guest owner executes compiled graphics, audio and JPEG microcode directly
against live RDRAM. A graphics task hands its RDP command buffer to the GLES
render thread and posts SP completion. The buffer stays owned by that task
until the render thread finishes. One draw can be in flight.

Before handing off a draw, the owner captures the 4 KiB pages referenced by
texture and palette DMA and 8-bit color-image imports. Draws reuse the same
staging allocation. A readback updates that private view immediately, so later
commands in the same list can sample the result. The owner publishes the exact
framebuffer byte ranges and posts DP completion for lists containing FULLSYNC.
Raw DPC submissions keep the caller parked and lend live RAM through readback.

The guest can run and submit audio while a draw is in flight. The next graphics
task, presentation, capture, input using the GL context, and teardown collect
outstanding work before reusing shared state. SDL window events and presentation
stay on the application thread. The render thread has exclusive use of the GL
context during a draw. Set `RECOMP_GLES2_RENDER_THREAD=0` for synchronous GLES
drawing when comparing output or timings.

On two captured Ocarina of Time tasks, the render handoff copied 196 KiB and
184 KiB out of 8 MiB. Median submission times were 33 and 27 microseconds over
101 repetitions on a native Linux Radeon GPU at scale 1. Waiting immediately
for completion took 3.73 and 3.03 ms including submission. Synchronous draws
took 3.64 and 2.99 ms. The handoff frees the guest owner during drawing.
Both tasks produced identical pixels and RDRAM writes with the guest memory
overwritten after submission.

Python emits constant-operand C11 basic blocks and per-image IMEM maps.
IMEM DMA binds bytes from installed RDRAM to compiled entries after matching
the transferred span against verified program words, including overlays.
SP DMA reads from unpopulated RDRAM return zeroes in DMEM and IMEM. In IMEM
each zero-filled word replaces its previous translation with a NOP. Unknown
code from installed RDRAM, unsupported instructions, out-of-range DMA writes
and instruction/output budgets still fail explicitly.

Blocks group straight-line instructions and include a terminating branch's
delay slot. The generated code computes the next PC and charges the instruction
budget once per block. Every instruction also has a suffix entry for computed jumps into
the middle of a block. DMA/status writes and faults end blocks; an IMEM load
rebuilds the verified span map, truncating any block affected by a partial overlay.
Pending branch delays, incomplete spans and limits that expire inside a block
use one precompiled instruction at a time, preserving exact fault state. Normal
straight-line blocks are capped at 32 instructions, plus a branch delay slot.

The GLES pipeline consumes RDP edge/coefficient triangles, fill/texture
rectangles, TMEM palettes and RGBA/IA/I/CI textures. It retains combiner shaders,
depth/blending, framebuffer surfaces and RGBA16/RGBA32 native readback.
Depth storage follows the physical N64 Z image across color-buffer swaps and
RSP task boundaries. Native GLES3 and WebGL 2 both use 24-bit depth. S/T/W are
interpolated in screen space and divided in the fragment shader, with bounded
coordinates for zero/negative W. Rectangles have zero shade coefficients.
Read-only decals use a per-triangle depth bias derived from the RDP's normalized
integer Z gradients, replacing the fixed GL polygon offset. This keeps coplanar
overlays such as SM64's castle doorway trim visible across camera angles and
render scales. It approximates the RDP's depth/dz comparison; the depth buffer does not
provide the stored per-pixel RDP dz value. Depth-writing geometry retains its
original Z values. See the primary
[decal comparison](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/depth_test.h)
and [gradient normalization](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/rdp_renderer.cpp)
references.
RDP depth comparison and update are independent: GLES update-only passes keep
the depth test enabled with `GL_ALWAYS`. Disabling the GL test would also
discard their depth writes, allowing later objects to show through walls.
One- and two-cycle rectangles honor primitive Z and the independent compare
and write flags. Primitive Z uses the lower 15 bits of the command's Z field,
so Hexen's `0xffff` far-clear rectangle maps below 1.0. An unforced blender
selecting framebuffer memory in its final cycle preserves the existing color
while the sky mask updates depth.
Alpha-based coverage discards fragments below one of eight estimated samples.
This is independent of threshold comparison, which takes the first combiner
cycle's alpha in two-cycle mode. Partially transparent sprites such as Hexen's
first-person wand can therefore survive the coverage test.
CPU-drawn loading images use the VI format and profile-bounded dimensions.
LOADTILE latches sampling bounds as well as transferring pixels; raw video can
therefore draw through its upload tile without a separate SETTILESIZE. See the
[libdragon tile API](https://libdragon.dev/ref/rdpq_8h.html).
An unclamped masked axis decodes the full supported wrap period, even when
SETTILESIZE is missing or still describes the previous tile.
Coverage, blending, LOD and VI framing remain approximations.

## Checked programs and acceptance

Only the exact ROM revisions pinned by the game profiles are covered.

| Game / program | Inspected boundary |
| --- | --- |
| GoldenEye US / Fast3D fifo 2.0G | Debug and Release menu-to-Dam replay, movement, turning, firing and Release watch. |
| Perfect Dark US v1.1 / F3DPD | Debug file menus and Carrington Institute room with input and nonzero PCM. VI framing needs broader review. |
| Quake US / F3DEX.NoN + F3DLP.Rej 1.21 | Debug and Release new-game transition, first-level movement/turning/firing. Shared Z-image storage and depth precision corrected; broader geometry acceptance remains pending. |
| Turok US v1.2 / F3DLX.NoN, F3DEX.NoN and L3DEX 1.00 | Debug replay through title, first-person gameplay, automap and pause. |
| Quake II US / F3DLP.Rej + F3DEX.NoN 1.23 | Intro and menu program switch. |
| Doom 64 US / F3DEX.NoN and L3DEX 1.00 | Staging Area, shotgun pickup, textured and line automaps, and pause in a 1,750-presentation Debug replay. |
| Duke Nukem 64 US / Fast3D 2.0H | Textured main menu after correcting rectangle shade. |
| Duke Zero Hour US / F3DEX2.NoN 2.08 | Original intro logos. |
| Nightmare Creatures US / F3DEX2 2.06 | Animated opening scene. |
| Banjo-Kazooie US / F3DEX 1.21 | Nintendo logo and animated waterfall intro. |
| Rayman 2 US / F3DEX.NoN 1.23 | CPU intro, resolution transitions and rendered first scene with character/HUD. |
| SM64 US / Fast3D 2.0D | Native bootstrap/full program pinned; regression results below. |
| 1080 JU revision 0 / F3DEX 1.23 | Native bootstrap/full program pinned; a separate CPU scene mismatch limits later progression. |
| Conker US / F3DEXBG.NoN | 1,200-presentation Debug and Release startup runs, including the 64×64 CI8 intro target. |
| Resident Evil 2 US v1.1 / F3DEX.Rej and S2DEX | Opening logos, movie and title menu with RAM scanout. |
| Ocarina of Time US v1.2 / F3DZEX2 and S2DEX2 | First-slot Temple of Time exterior, including prerendered background strips. |
| Shadow Man US / F3DEX2 2.08 | Native bootstrap/full program pinned; regression results below. |

Additional builds and current limitations are tracked in the
[per-ROM coverage matrix](desktop-coverage.md), including compile-only
variants and scene-specific limits. Bad Apple submits raw RDP commands directly,
and Media Player keeps its native SDL interface; neither uses a display-list
HLE frontend. A compiled microcode program does not prove that every emitted
RDP effect or CPU game path works.

## Generation and build

Keep all graphics declarations in the game's `profile.toml`.
`[[graphics.microcodes]]` pins task `name`, `code` and `data` spans with
`rom_offset`, `size` and `sha256`. `[graphics.rsp].images` lists the original
bootstrap first, then complete program text including IMEM overlays, with the
same bounds/fingerprint fields. An optional `data.task_size` records a larger
DMA transfer when mutable parameters follow the authenticated data prefix. Compressed images use a verified named `image`
and `offset` supplied by the game's preparation helper.

For compressed images the game generator exposes `rsp_images(rom, profile)` for audio and graphics,
or `graphics_images(rom, profile)` for graphics alone.
The shared generator authenticates each image and writes `game_rsp.c` for the
GLES backend. The generated program registers its microcode and framebuffer inventory
at renderer creation. Task submission verifies the code and data in guest RAM,
and IMEM DMA verifies each loaded code span.

```sh
python3 tools/build.py goldeneye-us --release --gles
./build/goldeneye-us-gles-release/bin/goldeneye-us
```

## Image quality

Desktop defaults are scale 4, FXAA, bilinear magnification and trilinear mipmap
filtering. ARM64 defaults are scale 1 and FXAA off.
A 320x240 image renders internally at 1280x960 on desktop; GoldenEye's 440x330
menus render at 1760x1320. The window starts at 1280x960 and can be resized.

- `RECOMP_GLES2_SCALE=1..4` selects internal scale.
- `RECOMP_GLES2_AA=0` disables FXAA; `1` enables it.
- `RECOMP_GLES2_MIPMAPS=0` disables mipmaps; `1` enables them.
- By default, presentation and captures follow the game's VI gamma-enable bit,
  using an exponent of `1/2.2` when enabled and `1` when disabled.
  Set `RECOMP_GLES2_GAMMA=0.5..2.5` to force a fixed exponent in either mode.

### Experimental lighting and reflections

| Launch option | Toggle | Effect |
| --- | --- | --- |
| `--ssao` | F3 | Screen-space ambient occlusion darkens creases and contacts between nearby surfaces. |
| `--ssr` | F9 | Screen-space reflections trace visible geometry on upward-facing surfaces. |
| `--torch-lights` | F2 | Quake and Ocarina of Time flames add localized light to nearby visible surfaces. |
| `--torch-shadows` | F1 | Start torch lighting with scene and player shadows. F1 toggles both kinds of shadow. |

Launch Quake with `./build/quake-us-gles-release/bin/quake-us --ssao --ssr --torch-shadows`
to compare the effects during gameplay. On WebGL use
`?ssao=1&ssr=1&torch-shadows=1`. Each option starts its effect enabled.
Launching with any effect enables the depth attachment and the F3/F9 controls.
Launching with either torch option enables F1 and F2 in a supported game. To see reflections
in Conker's opening sequence, launch `./build/conker-us-gles-release/bin/conker-us --ssr`
and watch the wooden floor beneath the N64 logo.

Quake uses the positions of static `flame` and `flame2` models in single-player,
with a warm color and a radius chosen for the model size. Castle of the Damned
has both types. Ocarina of Time reads the position, color and radius from each
active `Obj_Syokudai` torch or `En_Light` flame. The light follows the game's
flicker and goes out when the game extinguishes it. The renderer projects up to
eight nearby sources with the game's camera and viewport.

With shadows enabled, the shader takes up to 24 depth samples between a lit
surface and each contributing flame. A blocker reduces that flame's contribution.
The game's original lighting stays visible in shadow. Scene geometry uses
visible depth, so a wall or prop can lose its shadow when it leaves the screen.

The player also blocks light using a small set of rounded body segments.
Link's segments follow the animated head, torso, arms and legs. Quake fits a
standing head, torso and legs to the local player's collision hull and heading.
This lets a torch behind the first-person camera cast a shadow onto the floor
ahead. Use F1 with either torch launch option to see the difference.

The renderer records visible triangle depth alongside the color buffer. SSAO
samples six screen directions. Its normal estimate checks which neighboring
depths follow the same plane, and its bias accounts for RDP depth precision
at triangle boundaries. SSR traces up to 32 steps. Both estimate
surface normals and assume a 60-degree vertical field of view. Silhouettes
and screen edges can produce gaps. Presentation and screenshot captures use
the same effects. Guest framebuffer readback retains the original color.
With all effect options omitted, the renderer skips the extra attachment and pass.

Copy-mode HUD glyphs retain point sampling. Complete repeating tiles use GPU
wrapping for filtering across seams. The shared renderer uses GLSL ES 3.00
texture gradients computed before wrapping and supports mipmaps for non-power-of-two textures, including replacement images.

RAM framebuffer sizes remain native. Higher-resolution surfaces are downsampled
before CPU readback, without FXAA or display color correction. The former HLE
geometry interpolation and its SM64 vertex hooks were removed with the frontend.
Desktop Debug and Release builds with texture support enable the F8 thumbnail
editor without a pack. See [GLES texture packs](textures.md#gles3-texture-packs-and-editor)
for formats and limits. F6 toggles mesh wireframe in Debug and Release. F12 opens the shared
[GameShark cheats menu](cheats.md).

F7 expands the view without stretch only when a native camera/visibility hook
accepts the new aspect (GoldenEye and Banjo in supported scenes). With no
hook, use original framing. Once a compiled RSP has clipped geometry, the
GLES backend cannot recover the missing sides by reshaping the framebuffer.

## Validation and limits

Compiled GoldenEye completed a 5,100-presentation
Debug Dam replay at 4x scale with movement, turning, firing and nonzero stereo
PCM. Release completed all 38 keyboard events, showed the watch, returned to play
and exited normally. The final graphics-only build also completes that replay
with all 38 events delivered and visible Dam gameplay. Eight other Debug startup
runs reached 1,200 presentations.
Perfect Dark's maintained 3,600-presentation replay completed 3,261 graphics and
1,769 audio tasks and rendered the Carrington Institute room. These are bounded
scenes, not full campaigns or listening-quality acceptance.

After removing the graphics HLE frontend, SM64, 1080 and Shadow Man complete
1,200-presentation native-RSP startup checks; the earlier GLES2 backend also passed a strict 2.0
context check for SM64. All twelve game integrations and both native video/player targets
build in Debug and Release. The SM64 US Debug workflow passes.
Media Player decodes a temporary synthetic MP3; Bad Apple completes 180 raw-RDP
frames in both modes, with inspected grayscale animation after correcting KSEG
address normalization and LOADTILE sampling bounds. The final 5,100-presentation
GoldenEye Debug Dam replay also passes after that texture correction. Longer video acceptance
remains unverified.

The 2026-11-10 SM64 doorway correction passes close-up Debug camera sweeps and
normal Debug/Release builds; the user confirmed that the flicker appears fixed.
The close-up used a temporary debugger position change after entering Castle
Grounds, leaving game code and the user's save unchanged. Sanitizer checks on
482 captured RDP triangles preserve all decoded geometry and cover zero,
negative and saturating depth gradients. Temporary diagnostics are removed.

Quake's new-game switch is pinned to both original programs. A 3,300-presentation
Debug replay and a 55-second Release desktop replay reach the first level and
exit normally. Its world task inherits RDP state from an earlier setup task.
Depth buffers now follow the physical Z image, with clears retained across task
and color-buffer changes; supported contexts also use 24-bit depth. The captured
world task emits 203 triangles with valid normalized depths of 0.981356 to
0.993719, where 16-bit linear depth has limited separation. Inspected gameplay
includes the corridor, shotgun and HUD. These checks do not establish that all
reported flicker or later levels are fixed.

Temporary C11 AddressSanitizer/UBSan checks passed for a captured GoldenEye task,
immutable worker snapshots while caller RAM changes, busy-queue rejection,
worker reuse/shutdown, accumulator saturation, carry flags, aliased vectors,
reciprocal edges, odd transfers, transpose loads/stores, scalar branch delays,
JR alignment, DMEM wrap and changed-IMEM rejection. Leak detection was disabled
for the host environment. Task-created diagnostics are removed after validation.

C11 macro specialization puts add/subtract and comparison bodies directly in
their vector handlers, removing the second call in both build modes. Small
shared helpers use `static inline`.
In an interleaved, CPU-pinned `-O2 -fPIC` benchmark, a captured
2,004,161-instruction GoldenEye task measured 17.99 ms before and 15.62 ms after
(medians of four 31-trial samples per version). RDP output and resulting RAM were
byte-identical. This measures that CPU task only, not game FPS or ARM performance.

The 2026-11-10 GoldenEye Release profile sampled a 20-second Dam sequence at
199 Hz on a Ryzen 7 5700X, with scale 4 and the default desktop quality settings.
The sequence includes the end of the intro, then movement/turning/firing.
Process CPU was 71.4% of one logical core: the RSP worker used 45.1% and the owner
23.8%. Of 2,745 samples, 30.5% were in the generated RSP loop itself and 24.1%
in vector helpers. Source attribution placed 14.2% of all samples in the loop's
dispatch, PC advancement and checks; the full loop self cost also includes scalar
instructions. This motivated the basic-block compiler described above.
Approximately aligned timing windows averaged 2.74 ms wall and
2.37 ms owner CPU per completed RDP/GLES2/readback service. There were no dropped
samples or timer overruns. This does not measure GPU execution, steady-scene
FPS or ARM performance. See the [profiling workflow](profiling.md).

The basic-block compiler matches the previous dispatcher byte for byte on 180
temporary synthetic cases and three captured GoldenEye/Quake tasks, comparing
registers, PC/fault state, DMEM/IMEM, RAM, DMA writes and ordered RDP output.
All 183 comparisons also pass AddressSanitizer/UBSan. Coverage includes
mid-block entries, branches in delay slots, exact instruction limits, IMEM wrap,
partial/overlapping overlays and changed-code rejection. These compiler checks
do not replace gameplay acceptance, which remains with the user.

On the same Ryzen 7 5700X, an interleaved, CPU-pinned comparison with the presets'
`-O3` generated C measured these captured tasks (medians of four samples of 31
timed runs per version, after two warmups):

| Captured task | Instructions | Previous dispatcher | Basic blocks | Less RSP CPU time |
| --- | ---: | ---: | ---: | ---: |
| GoldenEye US | 2,004,161 | 17.45 ms | 8.98 ms | 48.6% |
| Quake US | 126,153 | 1.029 ms | 0.608 ms | 40.9% |
| Quake US | 78,095 | 0.204 ms | 0.139 ms | 31.9% |

Timing includes RSP task initialization and IMEM verification, excluding the
worker's RAM snapshot and rendering. The `-O2` comparison also improves these
tasks by 32–44%. Larger generated block bodies trade code size for fewer
dispatches. These results measure x86-64 task CPU time; game FPS and ARM
performance still need measurement.

Vector behavior was checked against the primary
[ares RSP implementation](https://github.com/ares-emulator/ares/tree/master/ares/n64/rsp);
perspective bounds and rectangle inputs against
[paraLLEl-RDP](https://github.com/Themaister/parallel-rdp). These are consultation
references, not build dependencies. The implementation is C11.

## ARM foundation

`runtime/fpu.c` retains x86 behavior and Linux/AArch64 C/libm handling for guest
rounding/exceptions and FPCR isolation. Initial ARM support is 64-bit Linux.
No Pi hardware gameplay or performance acceptance has been established.

```sh
cmake -S . -B build/pi4 -DCMAKE_BUILD_TYPE=Release \
  -DRECOMP_GAME=sm64-us -DCMAKE_C_FLAGS=-mcpu=cortex-a72
cmake --build build/pi4 -j2
```

`cmake/toolchains/linux-aarch64.cmake` supports cross builds with a target sysroot
containing SDL2, GLES3, OpenSSL, libcurl and POSIX threads. Cross compilation cannot certify
hardware behavior.
