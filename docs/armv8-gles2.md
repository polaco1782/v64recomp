# ARMv8 and Fast3D GLES2

Target: SM64 US on the development machine first, then Raspberry Pi 3 running
64-bit Linux at 320x240 and the original game frame rate. RT64 remains the default;
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
with EGL requested on Linux/X11. Raw RDP DMA, RT64 extensions and interpolation
are unsupported. Other games require verified microcode/command coverage.

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
./build/sm64-gles2-debug/bin/sm64-us --frames 60 --mode move --mute
```

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
