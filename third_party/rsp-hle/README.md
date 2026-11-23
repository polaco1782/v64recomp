# Native JPEG subset

Upstream: https://github.com/mupen64plus/mupen64plus-rsp-hle
Revision: `8a7a472a7172eb2c8725b305eae26818ed7b51a2`
License: GPL-2.0-or-later, see source notices and `LICENSES`.

`jpeg.c` supplies the PS/OOT JPEG task decoder: dequantization, inverse DCT and
RGBA5551 tile output. The CPU's JPEG parsing and Huffman decoding stay translated.
Only the PS variant is included.
`backends/jpeg.c` verifies the complete initialized boot/code/data images against
the game's SHA-256 profile on every task, validates all RAM spans and alignment,
and rejects yielded tasks. The adapter uses the word-swapped RDRAM helpers in `memory.c`.

Local JPEG changes remove a negative signed shift, explicitly narrow IDCT results
through a 32-bit integer, and check subblock overlap without subtracting pointers
to separate stack arrays. These retain the upstream pixel arithmetic while
avoiding undefined C operations. Source notices and the pinned revision apply.
