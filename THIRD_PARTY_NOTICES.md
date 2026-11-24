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
The native interface uses Sean Barrett's public-domain `stb_easy_font.h` in
`third_party/stb/`. The header includes its MIT and public-domain license options. The external homebrew ROM and user music are not
included in the repository or executable.

## Rice / GLideN64 texture hashing

The C11 texture matcher implements the RiceVideo and GLideN64 checksums.
The GLideN64 notice credits Sergey Lipskiy and the glN64, Glide64 and GLideHQ
contributors. [GLideN64 license and credits](licenses/GLideN64.txt).

## Mupen64Plus RSP HLE

RE2's integer video cores in `games/re2-us-v1/video_dsp.c` are adapted
from [Salsa64](https://github.com/ricowp81/Salsa64), revision
`59cb166c99e7bee2dbf6467814dfab9a13625d1f`, `src/rsp/jpeg/re2.c` and `re2.h`.
It is GPL version 2 or later, retains the original Gilles Siberlin notice,
and builds on the Mupen64Plus RE2 task implementation. Its local adapter adds
ROM-bound microcode authentication and validates the complete RAM plan. The
[upstream license and author list](licenses/Mupen64Plus-rsp-hle.txt) accompany the port.

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
(zlib), libcurl (curl license), and PThreads4W (Apache-2.0). libcurl is also
used on Linux. Installed vcpkg packages preserve their complete notices in
`build/vcpkg_installed/x64-windows/share/<package>/copyright`.

The optional GameShark menu uses Dear ImGui at revision
`277ae93c41314ba5f4c7444f37c4319cdf07e8cf`. Its
[MIT license](third_party/cimgui/imgui/LICENSE.txt) accompanies the vendored sources. The C11 menu uses cimgui 1.90.4 at revision
`f0fb387921c688d59034f19d4479726dc021e41a`. [cimgui license](third_party/cimgui/LICENSE).

The desktop texture pack loader uses [miniz](licenses/miniz.txt) for ZIP archives
and [stb_image](licenses/stb.txt) for PNG decoding.

## Berkeley SoftFloat (optional portable guest FPU)

Browser builds and desktop builds with `RECOMP_SOFTFLOAT=ON` fetch Berkeley
SoftFloat Release 3e, copyright John R. Hauser and The Regents of the University
of California, from <https://github.com/ucb-bar/berkeley-softfloat-3> at commit
`07ebd29db0cfb55723a79fbbffe99103ef38e305`. Its BSD 3-clause license is copied
from the pinned upstream `COPYING.txt` into `bin/licenses/SoftFloat.txt`.
The repository supplies a portable platform configuration and build integration;
upstream arithmetic sources are unmodified.
