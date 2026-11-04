# Browser build (experimental)

The `codex/wasm` branch starts from `feature/armv8-fast3d-gles2` and merges the
current shared game layout. Its first target is **Super Mario 64 US**, using
Fast3D → GLES2 → WebGL. It does not build RT64. Other games still need verified
GLES2 microcode and command coverage before a browser target can be enabled.

## Build and run

Install the normal Python generation dependencies and import the pinned SM64 US
ROM to `local/sm64.us.z64`, as described in the root README. Install and activate
[Emscripten](https://emscripten.org/docs/getting_started/downloads.html)
(the initial port uses SDK **6.0.13**), then run from the repository root:

```sh
emcmake cmake -S . -B build/wasm-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/wasm-release -j4
python3 tools/serve_web.py --directory build/wasm-release/bin --port 8080
```

Open `http://localhost:8080`, select the ROM and press **Play**. The loader
normalizes `.z64`, `.v64` and `.n64` byte orders and checks the US ROM's SHA-1;
the runtime independently validates the normalized ROM. The browser build does
not embed or download ROMs. The server serves only `build/wasm-release/bin`.
The chosen ROM is kept in the page's in-memory filesystem and is not uploaded.

```sh
emcmake cmake -S . -B build/wasm-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DRECOMP_OPTIMIZE_DEBUG=ON
cmake --build build/wasm-debug -j4
python3 tools/serve_web.py --directory build/wasm-debug/bin --port 8081
```

Both configurations use `games/sm64-us/main.c`. Debug retains shared diagnostics,
controller replay support, captures, browser assertions and stack checks.
The `bin` directory contains the page, JavaScript, Wasm and license notices.
Serve those files together. Do not serve the repository root or private `local/`.

## Runtime choices

- Emscripten pthreads put the existing blocking scheduler in an application
  worker, leaving the browser UI thread available for SDL/WebGL and input.
  Eight pre-created workers cover the initial SM64 thread set. This pool size
  is not a guarantee for other games.
- WebGL context creation/presentation use Emscripten's explicit proxy API, so
  worker texture uploads share the correct context. SDL retains window/input
  handling; WebAudio device initialization runs on the browser thread.
- Browser worker timestamps share a monotonic host-clock floor to handle
  independently rounded performance time origins. Guest clock checks remain active.
- The GLES2 backend uses direct GL entry points on Emscripten. Existing shaders,
  display-list parsing and GPU buffers are reused. Browser rendering starts at
  native 320×240 internal resolution, displayed on a 960×720 canvas.
- `RECOMP_SOFTFLOAT=ON` is the browser default. Berkeley SoftFloat implements
  guest arithmetic, rounding and exception flags. The existing VR4300 NaN,
  denormal, flush and trap policy remains in `runtime/fpu.c`; no relaxed guest
  arithmetic mode is introduced. SoftFloat is pinned to upstream commit
  `07ebd29db0cfb55723a79fbbffe99103ef38e305` and fetched into the build directory.
  Its first configure needs network access; its BSD license is copied to `bin`.
  Desktop builds can enable this option for comparisons; native SSE/AArch64
  remains their default.
- Synchronous SHA-1/SHA-256 fingerprints use portable code in the browser;
  desktop builds keep OpenSSL behind the same shared hash interface.
- EEPROM is mounted under `/saves` using IndexedDB with automatic persistence.
  **Download save** exports the current EEPROM. Browser storage is origin-specific:
  changing the hostname or port selects a different save store. Save import is
  not implemented yet.
- Native shared-library live repair and the Linux sampling profiler are not
  available in a browser. Missing guest functions still fail explicitly. Guest
  crash reports remain in the virtual filesystem; browser stacks go to the log.

Threaded Wasm requires a secure context (HTTPS or localhost) and these response
headers, which the included server supplies:

```text
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

See [Emscripten pthreads](https://emscripten.org/docs/porting/pthreads.html)
and [GLES/WebGL mapping](https://emscripten.org/docs/porting/multimedia_and_graphics/OpenGL-support.html).
A plain file URL or server without isolation headers will not start the game.

## Validation and limits

Initial numerical checks compared 96,776 arithmetic/conversion/comparison cases
against x86 SSE, including all four rounding modes; outputs and FCR31 matched.
Portable SHA-1/SHA-256 passed 32 reference cases covering empty input, block and
padding boundaries, and a million-byte input. A desktop GLES2 Debug run using
SoftFloat completed 180 presentations and its logo capture was inspected.
Emscripten 6.0.13 Debug and Release builds passed. Debug rendered the animated
title screen. A Chromium/SwiftShader Release run reached file selection and the
opening castle sequence through Mario standing outside the castle, accepted
menu/dialogue keyboard input, created an EEPROM file, and exited cleanly with
code 0. WebAudio reported a running 48 kHz context; an output callback probe
observed 956,062 nonzero left-channel samples across 2,071 callbacks (peak 0.879). Invalid-size ROM rejection and an
IndexedDB write/reload/read check passed in an isolated browser profile.
Desktop GLES2 Debug (SoftFloat) and Release (native floating point) builds and
bounded startup runs also passed.

This first pass does not validate full-game progression, save-game resumption,
gamepads, audible quality, Firefox, Safari or mobile browsers. The browser
backend currently uses SDL2's deprecated ScriptProcessorNode; migrating playback
to AudioWorklet and profiling WebGL proxy traffic are follow-up work.

The renderer retains the GLES2 branch's experimental coverage and interpolation
limits; see [ARMv8/GLES2](armv8-gles2.md). This is not a full-game, mobile browser,
performance or RT64 feature-parity claim.
