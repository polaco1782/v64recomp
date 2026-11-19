# Browser build (experimental)

**Super Mario 64 US** builds for the browser with Emscripten. Its graphics tasks
run through the same ROM-verified, statically compiled RSP program and GLES2 RDP
renderer as the desktop GLES2 build. The browser presents the result with WebGL.
The browser loader currently accepts the pinned SM64 US ROM.

## Build and run

Install the normal Python generation dependencies and import the pinned SM64 US
ROM to `local/sm64.us.z64`, as described in the root README. An existing Wasm
build reuses its configured Emscripten toolchain. A fresh Debug or Release tree
can reuse the other configuration's SDK.

For a first build, [install Emscripten](https://emscripten.org/docs/getting_started/downloads.html)
(tested with SDK **6.0.13**). Source `emsdk_env.sh` in your shell, set `EMSDK` to
the installed SDK directory, or put the installed `emcc` on `PATH`. The launcher
also recognizes SDKs installed under `build/emsdk`, `local/emsdk` or `emsdk` in
the repository. Run from the repository root:

```sh
python3 tools/build.py sm64-us --release --wasm
python3 tools/serve_web.py --directory build/sm64-us-wasm-release/bin --port 8080
```

Open `http://localhost:8080`, select the ROM and press **Play**. The loader
normalizes `.z64`, `.v64` and `.n64` byte orders and checks the US ROM's SHA-1;
the runtime independently validates the normalized ROM. The browser build does
not embed or download ROMs. The server serves only `build/sm64-us-wasm-release/bin`.
The chosen ROM is kept in the page's in-memory filesystem and is not uploaded.

```sh
python3 tools/build.py sm64-us --debug --wasm
python3 tools/serve_web.py --directory build/sm64-us-wasm-debug/bin --port 8081
```

Both configurations use `games/sm64-us/main.c`. Debug retains shared diagnostics,
controller replay support, captures, browser assertions and stack checks.
The `bin` directory contains the page, JavaScript, Wasm and license notices.
Serve those files together. Do not serve the repository root or private `local/`.

## Runtime choices

- Emscripten runs the game on an application worker. Guest CPU threads use the
  shared minicoro scheduler, with Asyncify saving and restoring Wasm call frames.
  A separate worker executes compiled graphics RSP tasks. SDL/WebGL and WebAudio
  use the browser main thread where required.
- WebGL context creation/presentation use Emscripten's explicit proxy API, so
  worker texture uploads share the correct context. SDL retains window/input
  handling; WebAudio device initialization runs on the browser thread.
- Browser worker timestamps share a monotonic host-clock floor to handle
  independently rounded performance time origins. Guest clock checks remain active.
- The GLES2 backend uses direct GL entry points on Emscripten. The shared backend draws the compiled RSP’s RDP output using the GLES2 shaders and buffers. Browser rendering starts at
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

### Merged browser build

Emscripten 6.0.13 Debug and Release were exercised in Chromium with
SwiftShader. Both rendered SM64 and produced nonzero WebAudio samples. The
Release run accepted keyboard input, played the opening castle sequence and
reached Mario outside the castle. The loader rejected an invalid ROM and a
test file persisted across reload in IndexedDB. Desktop GoldenEye runs also
completed with RT64 Release and GLES2 Debug.

### Original browser branch

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

See [ARMv8/GLES2](armv8-gles2.md) for RSP microcode and renderer coverage.
