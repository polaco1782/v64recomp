# Native audio DSP subset

Upstream: https://github.com/mupen64plus/mupen64plus-rsp-hle
Revision: `8a7a472a7172eb2c8725b305eae26818ed7b51a2`
License: GPL-2.0-or-later, see source notices and `LICENSES`.

Only `alist.c`, `alist_audio.c`, `audio.c` and `memory.c` are compiled. These
implement audio-command operations in native C, including ADPCM, envelope
mixing, four-tap resampling and interleaving. No plugin shell, CPU interpreter,
RSP instruction interpreter, graphics or other microcode modules are imported.
The generic declarations in the upstream headers are retained; their other
implementations are not linked.

Local changes: an owner-thread validation callback before every command;
explicit fixed-width envelope initialization arithmetic; wide ADPCM/filter
accumulators; an ADPCM predictor bound check. The adapter bounds RAM/DMEM
operands and treats upstream warnings as failures. Unsupported microcodes and
resampling flags are rejected. Build with `-fno-strict-aliasing` for upstream's
word-swapped sample/state views.
