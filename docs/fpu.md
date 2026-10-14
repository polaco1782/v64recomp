# Floating-point translation

`tools/emit.py` lowers floating-point instructions to calls with constant
operation and register operands. `runtime/fpu.c` implements those individual
operations. It does not fetch or decode instructions at runtime. Raw FPR bits
remain in the thread's `MipsContext`: an FR=0 double uses the even register's low
word and the following odd register's high word. An explicit `game.fpu_fr = 1`
profile permits independent 64-bit registers, including odd double operands.
The application must also set `RecompSchedulerConfig.fpu_fr1`; its guest thread
layout saves all 32 registers in a 0x230-byte object. Existing profiles default
to FR=0. Generation rejects odd FR=0 double operands and nonzero condition codes.

The supported instructions are:

- Single/double `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `ABS`, `NEG`, `MOV`.
- `CVT.S.D`, `CVT.D.S`, word/long-to-single/double and single/double-to-word/long conversion.
- `ROUND.W/L`, `TRUNC.W/L`, `CEIL.W/L`, `FLOOR.W/L` with single/double input.
- All sixteen single/double comparison predicates and `BC1F/T/FL/TL`.
- `CFC1`/`CTC1` for FCR31, plus raw transfers, `DMFC1`/`DMTC1` and RAM loads.

The implementation follows the [NEC VR4300 manual, chapters 5 and 6](https://manuals.plus/m/c715b005609cb524eac30be04209da9e3d2a168bee77d6662b81dc5561e014ce).
FCR31's rounding mode selects nearest-even, toward zero, toward positive infinity
or toward negative infinity. Fixed-rounding conversions override that mode for
one operation. Cause bits describe the last operation; exception flags accumulate.
The condition bit is captured before executing a branch's delay slot. Likely
branches annul their untaken slots. Diagnostics identify a faulting slot's address.

This first arithmetic backend requires **Linux x86-64 SSE2**, IEEE binary32/64,
`-fno-fast-math -frounding-math -ffp-contract=off`, and libm. Every arithmetic
operation saves the host floating-point environment, masks host traps, selects
guest rounding, disables host DAZ/FTZ, collects exceptions, and restores the
saved environment and errno before returning. Coroutines can therefore keep different
guest rounding modes without changing host callbacks or other guest threads.
Tests verify rounding, sticky flags and restoration of MXCSR and host exceptions
with both GCC and Clang builds. Other hosts need a validated backend.

VR4300 NaNs use the legacy convention: the highest fraction bit is set for a
signaling NaN. Computational signaling NaNs set Invalid; quiet NaNs and denormal
inputs request an unimplemented-operation trap. Comparisons accept denormals
and quiet NaNs, with Invalid determined by their predicate. Raw moves and
loads/stores retain all payload bits. Invalid floating results use a canonical
legacy quiet NaN. With FS set and Underflow/Inexact enables clear, supported tiny
results flush to signed zero or signed minimum normal according to rounding,
and set Underflow/Inexact cause and flag bits, including exact denormal results.

This is an explicitly partial exception implementation. Enabled exceptions and
VR4300 unimplemented conditions stop through `recomp_fail` before writing the
destination. Guest exception-handler execution is not implemented. Nonfinite or
out-of-range word conversions, and inexact results rounded to minimum normal,
also stop explicitly. The latter boundary needs independent tininess analysis;
using host exception flags alone would not establish VR4300 behavior there.
Long integers use even FR=0 register pairs. Integer-to-single conversions round
directly to binary32, avoiding double rounding through a binary64 intermediate.
FCR0 identification, other FCRs and dynamic Status.FR changes are unsupported.
Instruction-ready status in a coverage report does not certify every operand
or the full call graph as runnable.

The SM64 profile converts the SDK's long-integer helpers as well as the game's
ordinary floating-point operations. Broader CPU/FPU conformance remains outside
the current gameplay validation.
