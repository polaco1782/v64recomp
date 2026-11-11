# C11 GLES2 and compiled RSP graphics

GLES2 is the sole renderer in this fork. Graphics display lists run through
statically recompiled RSP programs by default and have no HLE fallback. All
runtime/generated graphics code is C11; Python performs generation. Audio and
JPEG retain their existing task implementations.

## Task execution

```text
Original CPU display lists -> OSTask -> snapshot RDRAM and descriptor
  -> wake RSP worker -> compiled microcode -> RDP words and DMA writes
  -> owner thread GLES2 draw/readback -> commit writes -> SP/DP completion
```

One task is in flight, matching the single SP. The worker owns private RAM,
scalar/vector registers, 48-bit accumulators and DMEM/IMEM. It never touches guest
CPU dispatch, `Runtime`, SDL, GL or completion callbacks. The owner task service
polls or waits at required task boundaries; shutdown drains and joins the worker.
Only actual DMA writes and completed native color readback enter live RAM.
SP and DP events remain separate; a task without `FULLSYNC` has no DP event.

Python emits constant-operand C11 basic blocks and per-image IMEM maps.
IMEM DMA binds these compiled entries only when the complete transferred range
matches verified original program words, including overlays. Runtime execution
never decodes unknown instructions or compiles mutable RAM. Unknown IMEM,
unsupported instructions, invalid DMA and instruction/output budgets fail explicitly.

Blocks group straight-line instructions and include a terminating branch's
delay slot. PC bookkeeping stays local and instruction budgets are charged once
per block. Every instruction also has a suffix entry for computed jumps into
the middle of a block. DMA/status writes and faults end blocks; an IMEM load
rebuilds the verified span map, truncating any block affected by a partial overlay.
Pending branch delays, incomplete spans and limits that expire inside a block
use one precompiled instruction at a time, preserving exact fault state. Normal
straight-line blocks are capped at 32 instructions, plus a branch delay slot.

This models completed graphics tasks rather than RSP cycle timing or resumable
hardware yield. Full-RDRAM snapshots add memory traffic; threading is not a speed
claim, and ARM performance has not been measured.

The GLES2 pipeline consumes RDP edge/coefficient triangles, fill/texture
rectangles, TMEM palettes and RGBA/IA/I/CI textures. It retains combiner shaders,
depth/blending, framebuffer surfaces and RGBA16/RGBA32 native readback.
Depth storage follows the physical N64 Z image across color-buffer swaps and
RSP task boundaries. GLES3/OES_depth24 contexts use 24-bit depth; other GLES2
contexts retain the legal 16-bit fallback. S/T/W are
interpolated in screen space and divided in the fragment shader, with bounded
coordinates for zero/negative W. Rectangles have zero shade coefficients.
Read-only decals use a per-triangle depth bias derived from the RDP's normalized
integer Z gradients, replacing the fixed GL polygon offset. This keeps coplanar
overlays such as SM64's castle doorway trim visible across camera angles and
render scales. It approximates the RDP's depth/dz comparison; GLES2 does not
provide the stored per-pixel RDP dz value. Depth-writing geometry retains its
original Z values. See the primary
[decal comparison](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/depth_test.h)
and [gradient normalization](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/rdp_renderer.cpp)
references.
RDP depth comparison and update are independent: GLES2 update-only passes keep
the depth test enabled with `GL_ALWAYS`. Disabling the GL test would also
discard their depth writes, allowing later objects to show through walls.
CPU-drawn loading images use the VI format and profile-bounded dimensions.
LOADTILE latches sampling bounds as well as transferring pixels; raw video can
therefore draw through its upload tile without a separate SETTILESIZE. See the
[libdragon tile API](https://libdragon.dev/ref/rdpq_8h.html).
Coverage, blending, LOD and VI framing remain approximations.

## Checked programs and acceptance

Only the exact ROM revisions pinned by the game profiles are covered.

| Game / program | Inspected boundary |
| --- | --- |
| GoldenEye US / Fast3D fifo 2.0G | Debug and Release menu-to-Dam replay, movement, turning, firing and Release watch. |
| Perfect Dark US v1.1 / F3DPD | Debug file menus and Carrington Institute room with input and nonzero PCM. VI framing needs broader review. |
| Quake US / F3DEX.NoN + F3DLP.Rej 1.21 | Debug and Release new-game transition, first-level movement/turning/firing. Shared Z-image storage and depth precision corrected; broader geometry acceptance remains pending. |
| Quake II US / F3DLP.Rej + F3DEX.NoN 1.23 | Intro and menu program switch. |
| Duke Nukem 64 US / Fast3D 2.0H | Textured main menu after correcting rectangle shade. |
| Duke Zero Hour US / F3DEX2.NoN 2.08 | Original intro logos. |
| Nightmare Creatures US / F3DEX2 2.06 | Animated opening scene. |
| Banjo-Kazooie US / F3DEX 1.21 | Nintendo logo and animated waterfall intro. |
| Rayman 2 US / F3DEX.NoN 1.23 | CPU intro, resolution transitions and rendered first scene with character/HUD. |
| SM64 US / Fast3D 2.0D | Native bootstrap/full program pinned; regression results below. |
| 1080 JU revision 0 / F3DEX 1.23 | Native bootstrap/full program pinned; a separate CPU scene mismatch limits later progression. |
| Shadow Man US / F3DEX2 2.08 | Native bootstrap/full program pinned; regression results below. |

The remaining ports need their own checked programs and rendered validation,
including sprite/background and line variants. Bad Apple submits raw RDP commands directly, and Media Player keeps its native
SDL interface; neither uses a display-list HLE frontend. A compiled microcode program does not prove
that every emitted RDP effect or CPU game path works.

## Generation and build

Keep all graphics declarations in the game's `profile.toml`.
`[[graphics.microcodes]]` pins task `name`, `code` and `data` spans with
`rom_offset`, `size` and `sha256`. `[graphics.rsp].images` lists the original
bootstrap first, then complete program text including IMEM overlays, with the
same bounds/fingerprint fields. Compressed images use a verified named `image`
and `offset` supplied by the game's preparation helper.

`generate.py` imports `append_graphics_config` and `generated_sources` from
`tools/graphics_microcode.py`. After writing `game_config.h`, it calls
`append_graphics_config(profile, output, images)`. The shared driver tracks
`game_rsp.c` in its source manifest/cache; CMake compiles it with the game.
ROM-derived C stays private in the selected build directory. Task code/data are
reauthenticated on every submission; IMEM DMA separately checks loaded code.

The single `main.c` binds `game_graphics_microcodes` and any non-default native
framebuffer inventory. There are no backend-selection preprocessor guards or
renderer cache options. Existing presets/workflows use GLES2 directly:

```sh
cmake --preset goldeneye-us-release
cmake --workflow --preset goldeneye-us-release
./build/goldeneye-us-release/bin/goldeneye-us
```

For a separate tree:

```sh
cmake -S . -B build/goldeneye-release -G Ninja \
  -DRECOMP_GAME=goldeneye-us -DCMAKE_BUILD_TYPE=Release
cmake --build build/goldeneye-release -j4
```

## Image quality

Desktop defaults are scale 4, FXAA, bilinear magnification and trilinear mipmap
filtering for power-of-two textures. ARM64 defaults are scale 1 and FXAA off.
A 320x240 image renders internally at 1280x960 on desktop; GoldenEye's 440x330
menus render at 1760x1320. The window starts at 1280x960 and can be resized.

- `RECOMP_GLES2_SCALE=1..4` selects internal scale.
- `RECOMP_GLES2_AA=0` disables FXAA; `1` enables it.
- `RECOMP_GLES2_MIPMAPS=0` disables mipmaps; `1` enables them.
- `RECOMP_GLES2_GAMMA=0.5..2.5` selects the presentation color exponent; default
  1.2, with 1 giving neutral colors.

Copy-mode HUD glyphs retain point sampling. Complete repeating tiles use GPU
wrapping for filtering across seams. Mip selection uses unwrapped derivatives
through GLES2 extensions or optional GLSL ES 3.00 on a GLES3 context. Strict
GLES2 contexts retain the GLSL ES 1.00 sampling path. NPOT textures use bilinear
filtering for GLES2 legality. No GLES3 API calls are required.

RAM framebuffer sizes remain native. Higher-resolution surfaces are downsampled
before CPU readback, without FXAA or display color correction. The former HLE
geometry interpolation and its SM64 vertex hooks were removed with the frontend.
Texture replacement/dumping and renderer-specific inspection/cheat UI are also
absent; no unsupported texture-pack options are advertised.

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
1,200-presentation native-RSP startup checks; SM64 also passes a strict GLES2 2.0
context check. All twelve game integrations and both native video/player targets
build in Debug and Release. The normal `sm64-us-debug` CMake workflow passes.
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
cmake -S . -B build/pi3 -DCMAKE_BUILD_TYPE=Release \
  -DRECOMP_GAME=sm64-us -DCMAKE_C_FLAGS=-mcpu=cortex-a53
cmake --build build/pi3 -j2
```

`cmake/toolchains/linux-aarch64.cmake` supports cross builds with a target sysroot
containing SDL2, GLES2, OpenSSL and POSIX threads. Cross compilation cannot certify
hardware behavior.
