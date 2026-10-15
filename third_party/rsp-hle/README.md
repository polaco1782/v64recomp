# Native audio DSP and JPEG subset

Upstream: https://github.com/mupen64plus/mupen64plus-rsp-hle
Revision: `8a7a472a7172eb2c8725b305eae26818ed7b51a2`
License: GPL-2.0-or-later, see source notices and `LICENSES`.

`alist.c`, `alist_audio.c`, `alist_nead.c`, `audio.c` and `memory.c` are compiled. These
implement audio-command operations in native C, including ADPCM, envelope
mixing, four-tap resampling and interleaving. No plugin shell, CPU interpreter,
RSP instruction interpreter or graphics microcode modules are imported.
The upstream headers retain declarations for modules outside this subset.

`jpeg.c` supplies the PS/OOT JPEG task decoder: dequantization, inverse DCT and
RGBA5551 tile output. The CPU's JPEG parsing and Huffman decoding stay translated.
Only the PS variant is dispatched by the native adapter; PS0 and OB are not enabled.
`backends/jpeg.c` verifies the complete initialized boot/code/data images against
the game's SHA-256 profile on every task, validates all RAM spans and alignment,
and rejects yielded tasks. JPEG callbacks are renamed at compile time to keep
their completion state separate from audio. `memory.c` is shared by both adapters.

Local JPEG changes remove a negative signed shift, explicitly narrow IDCT results
through a 32-bit integer, and check subblock overlap without subtracting pointers
to separate stack arrays. These retain the upstream pixel arithmetic while
avoiding undefined C operations. Source notices and the pinned revision apply.

Local changes: an owner-thread validation callback before every command;
explicit fixed-width envelope initialization arithmetic; wide ADPCM/filter
accumulators; an ADPCM predictor bound check. The adapter bounds RAM/DMEM
operands and treats upstream warnings as failures. Unsupported microcodes and
resampling flags are rejected. Zelda NEAD command lists use a separate validated
dispatch path; other NEAD variants are not enabled. The NEAD filter keeps its
coefficient cache outside guest RAM, preserves the CPU-owned source table and
resets history on INIT. The pinned Zelda microcode's FILTER handler at IMEM
`0x1cb0..0x1e28` loads that table into DMEM and writes only the 32-byte state
back to RAM. Build with `-fno-strict-aliasing` for upstream's
word-swapped sample/state views.
