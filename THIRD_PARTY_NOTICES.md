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

The scanner links RT64's `src/tools/texture_hasher/texture_hasher.cpp` directly.
That file identifies its Rice checksum algorithms as derived from GLideN64 and
RiceVideo and licensed under the GPL. Its original attribution is retained in
the source; the full accompanying notice and GPL text are preserved in
[licenses/GLideN64.txt](licenses/GLideN64.txt).

GLideN64's notice credits Sergey Lipskiy and the glN64, Glide64, and GLideHQ
contributors. See the preserved notice for the complete credits.

## Mupen64Plus RSP HLE

The audio and JPEG task code under `third_party/rsp-hle/` comes from
Mupen64Plus-rsp-hle. Source files specify GPL version 2 or later and retain
individual copyright notices. The upstream license and author list are
preserved in [licenses/Mupen64Plus-rsp-hle.txt](licenses/Mupen64Plus-rsp-hle.txt).

## Other dependencies

RT64's bundled dependencies retain their licenses under `third_party/rt64/`,
including the notices in its submodules and `src/contrib/`. External libraries
and build tools retain their respective licenses. This project license does
not grant rights to ROMs, game assets, or third-party texture packs.
