# Third-party notices

v64recomp is distributed under GNU GPL version 2; see [LICENSE](LICENSE).
Third-party components retain their original copyright and license notices.

## RT64

The renderer is from the RT64 project. Its main license is reproduced below.
The GPL Rice texture-hashing component is identified separately in the next section.

```text
MIT License

Copyright (c) 2024 RT64 Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Rice / GLideN64 texture hashing

RT64's live ZIP matcher and offline texture hasher share
`src/common/rt64_rice_hasher.cpp`. Its Rice checksum algorithms are derived from
GLideN64 and RiceVideo and licensed under the GPL. Its original attribution is retained in
the source; the full accompanying notice and GPL text are preserved in
[licenses/GLideN64.txt](licenses/GLideN64.txt).

GLideN64's notice credits Sergey Lipskiy and the glN64, Glide64, and GLideHQ
contributors. See the preserved notice for the complete credits.

Turok's automap adapter in `games/turok-us-v12/graphics.cpp` follows the existing
Hexen adapter and uses GLideN64's `L3DEX.cpp` / `gSP.cpp` as references for line
encoding and widths, under GPL version 2 or later.

Conker's graphics adapter in `games/conker-us/graphics.cpp` also uses
GLideN64's `F3DEX2CBFD.cpp` and `gSP.cpp` as references for the CBFD command and
lighting behavior, under GPL version 2 or later. The accompanying notice and
license are linked above.

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
(zlib), libcurl (curl license), and PThreads4W (Apache-2.0). libcurl is also
used on Linux. Installed vcpkg packages preserve their complete notices in
`build/vcpkg_installed/x64-windows/share/<package>/copyright`.

RT64's bundled dependencies retain their licenses under `third_party/rt64/`,
including the notices in its submodules and `src/contrib/`. External libraries
and build tools retain their respective licenses. This project license does
not grant rights to ROMs, game assets, or third-party texture packs.
