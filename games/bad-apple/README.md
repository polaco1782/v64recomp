# Bad Apple!! N64 demo

This target recompiles Peter "krom" Lemon's 320x240, 8-bit grayscale LZSS/RLE
video demo. The MIPS video decoder and movie loop execute as generated native C;
the adapter handles direct PI/AI/VI/DPC hardware accesses. RT64 executes the
original raw RDP command list, and the shared SDL audio backend plays the ROM's
stereo PCM stream. There is no RSP microcode or libultra thread manager in this ROM.

## External ROM download

**The Bad Apple demo does not include the ROM. Building it automatically
downloads the ROM from upstream into ignored `local/bad-apple.z64`.** Neither
the repository nor release packages contain the ROM, video, or audio assets.
The generated executable requires the external file. Embedding is disabled in
both CMake and the packaging tool for this demo.

- [Upstream source and ROM](https://github.com/PeterLemon/N64/tree/7085543e4a19d8c539fc9e0a4d2869e788b4ed4b/Video/I8LZRLEVideo)
- [Pinned ROM download](https://raw.githubusercontent.com/PeterLemon/N64/7085543e4a19d8c539fc9e0a4d2869e788b4ed4b/Video/I8LZRLEVideo/I8LZRLEVideo.N64)
- Upstream revision: `7085543e4a19d8c539fc9e0a4d2869e788b4ed4b`.
- Size: 59,345,912 bytes; header entry: `0x80001000`.
- SHA-1: `638e81902bbd57114de3eba39fba3aabd322c6df`.
- SHA-256: `61bd899128485672c04aedf2dde310f633e39ab080fdcc3d41b740919650dd8b`.

Selecting this target invokes its download hook before source generation,
including CMake configuration, Visual Studio builds, and direct generator runs.
Other game targets retain their existing user-provided ROM workflow. The first
build needs HTTPS access to `raw.githubusercontent.com`. Subsequent builds verify
and reuse the cached file without network access. A truncated or mismatched
download is rejected and its temporary file removed; an existing mismatched ROM
is reported without overwriting it. For an offline first build, supply the exact
pinned big-endian ROM at `local/bad-apple.z64` before configuring.

Bad Apple!! music and animation retain their respective owners' rights. The
upstream software's Unlicense is not evidence of public-domain status for that
media. The external link/download is not a redistribution license.

## Build and run

```powershell
.\tools\build_msvc_project.ps1 -Game bad-apple -Configuration Release
.\build\msvc\bad-apple\release\bin\Release\bad-apple.exe
```

```sh
cmake --workflow --preset bad-apple-release
./build/bad-apple-release/bin/bad-apple
```

Use `Debug` / `bad-apple-debug` for diagnostics. `--frames N` limits the number
of movie frames; `--frames 0` plays until the window closes. `--mute` disables
playback, and `--rom PATH` selects another copy of the same verified ROM.
The original movie loop restarts after 6,572 frames. Close the window to exit;
the original demo has no gameplay controls.
Debug writes `final.ppm`, `final.i8` and `audio.wav` beneath its build's
`debug-output/bad-apple/` directory.

## Verified layout and adapter boundaries

- ROM `0x1000..0x12ec` maps directly to `0x80001000..0x800012ec`:
  one reachable function, 187 instruction words, with no indirect transfers.
- The initial load also includes the raw RDP list at `0x12f0..0x16d8`.
  The original commands upload each 10-row grayscale strip and draw it to the
  RGBA32 framebuffer at physical `0x100000`, ending in full sync.
- PCM begins at ROM `0x16d8`; compressed frames begin at `0x126eed8`.
  The original PI transfer is 2,940 bytes per movie frame. AI length masking
  preserves the hardware's eight-byte alignment (2,936 bytes played).
- The decoder uses RDRAM `0x300000` for the intermediate RLE stream and
  `0x380000` for the 320x240 grayscale output. Native scheduler objects occupy
  unused memory above `0x3f0000`.
- The cache instruction at `0x800012a0` is Index Writeback Invalidate D;
  native CPU and graphics already share coherent host memory.
- The native adapter implements only this ROM's verified hardware accesses.
  The line-480 wait paces the movie at 30 FPS; PI DMA completes synchronously,
  and AI busy polling follows the shared timed audio queue. Unknown accesses
  still fail through the common reporter.

## Validation

Automatic download, pinned checksum verification, offline cache reuse, download
failure-path checks and CPU generation passed. Windows/MSVC Debug and Release
built. A 90-frame Debug run exited successfully with the expected animation in
the captured image, 90 audio buffers and nonzero PCM samples.

The first Release launch crashed. Source inspection found that Release's inline
stores treated MMIO addresses as RAM, bypassing the device adapter; those stores
now fall back to the shared writer outside the ordinary RAM window. This final
fix has not been rebuilt or run: further testing was left to the user on request.
Release playback, a complete movie loop, sustained audio/listening quality and
Linux builds remain unverified. Rebuild before testing the final changes.
