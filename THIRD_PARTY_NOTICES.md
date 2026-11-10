# Third-party notices

v64recomp is distributed under GNU GPL version 2; see [LICENSE](LICENSE).
Third-party components retain their original copyright and license notices.

## minicoro

Guest CPU threads use stackful fibers from [minicoro](https://github.com/edubart/minicoro),
revision `02dad0f8b7cbb12fe6e216ae7a76db15ca55cd7b`. The unmodified header is in
`third_party/minicoro/` and retains its original dual license. This project uses
the MIT No Attribution option, preserved in [licenses/minicoro.txt](licenses/minicoro.txt).

## N64 Media Player target

The optional `media-player` target translates decoder instructions from
[lewilou22's Media Player V2](https://github.com/lewilou22/N64-Media-Player).
Its MIT notice is in [licenses/N64-Media-Player.txt](licenses/N64-Media-Player.txt).
The decoder is [minimp3](https://github.com/lieff/minimp3), released under
[CC0 1.0](https://github.com/lieff/minimp3/blob/master/LICENSE).
The native interface uses Sean Barrett's public-domain `stb_easy_font.h` from
`third_party/stb/`. The external homebrew ROM and user music are not
included in the repository or executable.

## stb_easy_font

The Media Player native interface uses Sean Barrett's `stb_easy_font.h`, kept
independently in `third_party/stb/`. Its unmodified header includes both the MIT
and public-domain license choices and the original notice.

## Mupen64Plus RSP HLE

The audio and JPEG task code under `third_party/rsp-hle/` comes from
Mupen64Plus-rsp-hle. Source files specify GPL version 2 or later and retain
individual copyright notices. The upstream license and author list are
preserved in [licenses/Mupen64Plus-rsp-hle.txt](licenses/Mupen64Plus-rsp-hle.txt).

RE2's integer video cores in `games/re2-us-v1/video_dsp.c` are adapted
from [Salsa64](https://github.com/ricowp81/Salsa64), revision
`59cb166c99e7bee2dbf6467814dfab9a13625d1f`, `src/rsp/jpeg/re2.c` and `re2.h`.
It is GPL version 2 or later, retains the original Gilles Siberlin notice,
and builds on the Mupen64Plus RE2 task implementation. Its local adapter adds
ROM-bound microcode authentication and validates the complete RAM plan.

## n64sym signature data

The SDK identification signatures and library references in `tools/symbols.json`
are imported from [shygoo/n64sym](https://github.com/shygoo/n64sym), commit
`ccf4600f3389f1a84bde23339225cf372fdf7712`. They are distributed under the MIT
license, copyright (c) 2017-2020 shygoo; the complete notice is preserved in
[licenses/n64sym.txt](licenses/n64sym.txt). The database retains the source URL,
revision and source-file SHA-256. Native replacement fingerprints are measured
independently from the supplied ROMs.

## Other dependencies

Windows dependencies are pinned in `vcpkg.json`: OpenSSL (Apache-2.0), SDL2
(zlib) and PThreads4W (Apache-2.0). Installed vcpkg packages preserve their complete
notices under `build/vcpkg_installed/x64-windows/share/<package>/copyright`.
External libraries and build tools retain their respective licenses. This project
license does not grant rights to ROMs, game assets or third-party texture packs.
