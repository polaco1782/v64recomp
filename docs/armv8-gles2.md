# ARMv8 and Fast3D GLES2

Target: SM64 US on the development machine first, then Raspberry Pi 3 running
64-bit Linux at the original game frame rate. RT64 remains the default;
the GLES2 backend is experimental and opt-in. Pi hardware is not ready for testing.

## Architecture

The game keeps its original graphics functions and display-list generation,
including scene geometry, HUD and frame setup. The shared Fast3D parser reads
the resulting commands from guest RAM and translates them into GLES2 draws.
There are no game graphics function hooks, native scene packets or model-mesh
conversion cache.

The frontend resolves segments and nested list calls with RAM, stack and
command-budget bounds. The task adapter verifies the original graphics microcode
against the pinned ROM, preserves SP/DP completion ordering and presents the
VI-selected framebuffer. Unsupported commands/tasks fail explicitly.

Implemented: matrices, triangles, lighting, fog, viewport/scissor, culling,
depth/blending, TMEM loads/palettes, RGBA/IA/I/CI textures, tile addressing,
texture caching, combiner shaders, fill/texture rectangles, framebuffer surfaces
and CPU-visible RGBA5551 readback. Rendering uses SDL GLES2 entry points/shaders,
with EGL requested on Linux/X11. Raw RDP DMA and RT64 extensions are unsupported.
Other games require verified microcode/command coverage.

Desktop rendering defaults to 960x720 (3x native resolution), bilinear texture
filtering and linear presentation scaling. Copy-mode HUD glyphs retain point
sampling. ARM64 defaults to 320x240 to limit GPU cost. Set `RECOMP_GLES2_SCALE`
to `1`, `2`, `3` or `4` before launch to choose 320x240 through 1280x960.
For example, in PowerShell:

```powershell
$env:RECOMP_GLES2_SCALE = "4"
.\build\sm64-gles2-release\bin\sm64-us.exe
```

Framebuffers read by the game remain 320x240; higher-resolution surfaces are
downsampled on the GPU before RAM readback. Captures retain the render resolution.
Higher scales increase GPU work and framebuffer memory use.

A mild color curve (RGB exponent 1.2) darkens midtones while preserving black,
white and alpha. It applies once to the composed display image. Set
`RECOMP_GLES2_GAMMA=1` for neutral colors, or a value from `0.5` to `2.5` to
adjust the curve; larger values darken the image. This is a visual preference,
not a change to texture decoding or an emulation accuracy claim.

Color correction runs in the presentation pass, rather than in every material
shader. Native framebuffer readback retains the ungraded game image. Texture
cache keys cover the active tile and its palette, so unrelated TMEM loads do
not cause redundant texture uploads.

Set `RECOMP_GLES2_FPS=60` for experimental geometry interpolation, or `30` for
the default native rendering. Physics, input sampling, audio and display-list
generation still run at their original rates. The renderer draws a halfway
pose between matching triangles, presents it on the first display tick, then
presents the native pose on the next tick. This adds about one display interval
of visual latency; it is not a 60 Hz game-logic patch or image cross-fade.

Vertex-load slots are paired only when the full command topology and vertex
count match. Normals/colors are not identity keys. Shared cache vertices use one
intermediate position, including geometry crossing the clipping planes. Any
failed correspondence or large discontinuity discards the whole midpoint instead
of mixing poses within one mesh. Orthographic HUD elements stay native.

SM64's animated title head has an explicit identity adapter: two fingerprinted
Goddard vertex-conversion wrappers run the original routines, then associate
packed vertices with their logical game vertex objects. GLES2 can match these
vertices despite changing batches and triangle counts. Unannotated vertices in
these frames stay native; missing history, ambiguous transforms and large jumps
reject the whole midpoint. The adapter writes no guest RAM and is enabled only
with `RECOMP_GLES2_FPS=60`. See the [game notes](../games/sm64-us/README.md).

A desktop title-screen check at 4x scale accepted midpoint candidates for all
301 sampled graphics tasks (150 through 450), with approximately 30 graphics
tasks and 60 presentations per second. Three captured midpoint poses showed no
obvious tearing. This covers the idle head animation, not every interactive
deformation. General object/transform matching is still missing; gameplay can
still fall back to native poses. The option remains experimental and disabled
by default. Presentation counts alone do not establish smooth 60 FPS.

Native framebuffer readback remains unchanged. Extra GPU draws and depth/color
surfaces increase cost; Pi performance is unverified. The renderer's `interpolated_frames` counter is distinct from window presentations. The revised 120-frame Castle Grounds movement replay
produced 85 midpoint presentations across 238 display ticks at 4x scale, with
zero audio underruns and byte-identical final guest RAM and native image compared
to native rendering, including after adding the title-head adapter. This is
partial gameplay interpolation, not a sustained 60 FPS claim.

## Desktop build and run

Use a separate directory with the existing desktop dependency configuration:

```sh
cmake -S . -B build/sm64-gles2-release -DCMAKE_BUILD_TYPE=Release \
  -DRECOMP_GAME=sm64-us -DRECOMP_RENDERER=gles2 -DRECOMP_BUILD_RT64=OFF
cmake --build build/sm64-gles2-release --target sm64-us -j4
./build/sm64-gles2-release/bin/sm64-us
```

On Windows, use Developer PowerShell, the existing vcpkg toolchain/installed
directory as needed, and the `.exe` suffix. Import the pinned ROM into `local/`
using `tools/import_rom.py`. No external renderer sources/assets are build inputs.

Configure Debug in a separate tree with the same renderer flags, then run:

```sh
./build/sm64-gles2-debug/bin/sm64-us --frames 60 --mute
```

For interactive Debug play, configure with `-DRECOMP_OPTIMIZE_DEBUG=ON` to
optimize translated game code, runtime services and GLES2 parsing while retaining
Debug diagnostics and checks. Use `OFF` when unoptimized source stepping is
needed. The shared timing report separates game graphics jobs from presentation
rate; a 60 Hz presentation rate does not imply 60 game updates per second.

Live SM64 runs service guest VI/AI/task deadlines between presentations using
the shared pacing helper and the profile's verified idle thread. Windows sleeps
use high-resolution waitable timers where supported; coarse timer wakeups can
otherwise miss retraces and starve audio even when rendering takes only 1-2 ms.

The original DL path has passed the Castle Grounds movement replay on an AMD
Radeon RX 7800 XT exposing OpenGL ES 3.2, using GLES2 APIs/shaders. Startup/logo
and Mario face were also inspected. Bounded desktop checks do not establish
every course/effect, sustained performance or strict GLES2 hardware behavior.
Muted PCM checks do not establish listening quality. EGL/Linux presentation and
Pi input/audio/performance remain untested.

The parser uses checked RAM spans for direct texture/vertex reads, batches across
matrix/vertex/list updates that preserve raster state, and copies completed color
images into RAM with one destination range check. The bounded 60-frame Castle
Grounds replay measured 31.1 to 6.9 ms per graphics task in Debug after these
changes; the final image is unchanged. A separate 600-presentation Release
startup/Mario-face run averaged 1.7 ms across 282 graphics tasks. These are
different workloads, not a Debug/Release comparison or a Pi performance claim.
The existing `RECOMP_TIMING` framework retains overall graphics and calling-thread
CPU measurements.

## ARM foundation

`runtime/fpu.c` preserves x86 SSE behavior and adds Linux AArch64 strict C/libm
arithmetic, guest rounding/exception handling and FPCR flush/default-NaN isolation.
Guest register/pointer widths remain unchanged. Initial ARM support is
Linux/AArch64, not 32-bit ARM or bare metal. The CPU sampler requires x86-64 Linux.

Full ARM game/backend builds and hardware behavior remain unverified. When the
Pi is ready, install SDL2/GLES2, OpenSSL 3 and POSIX thread development dependencies
and configure a separate game build:

```sh
cmake -S . -B build/pi3-gles2 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS=-mcpu=cortex-a53 -DRECOMP_GAME=sm64-us \
  -DRECOMP_RENDERER=gles2 -DRECOMP_BUILD_RT64=OFF
cmake --build build/pi3-gles2 --target sm64-us -j2
```

`cmake/toolchains/linux-aarch64.cmake` selects GNU AArch64 tools for cross builds;
supply an ARM sysroot containing target dependencies. Cross compilation cannot
certify hardware behavior.

Next: extend parser/material correctness and desktop replay coverage, measure
frame costs, then validate on Pi hardware when available.

Command references:
[SM64 PC renderer](https://github.com/sm64-port/sm64-port/blob/master/src/pc/gfx/gfx_pc.c)
and vendored RT64 Fast3D handlers. ARM references:
[glibc environment handling](https://github.com/bminor/glibc/blob/master/sysdeps/aarch64/fpu/fenv_private.h),
[Mesa VC4](https://docs.mesa3d.org/drivers/vc4.html).

The browser target reuses this renderer through WebGL; see [WebAssembly](webassembly.md).
