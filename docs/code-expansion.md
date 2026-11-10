# MIPS code expansion and native efficiency

v64recomp translates MIPS instructions into C11 and compiles that C into host
machine code. The generated source is deliberately explicit about guest
semantics. A large C file does not mean that every statement becomes a host
instruction, and a larger native code footprint does not imply a proportional
execution cost.

This document separates source expansion, machine-code expansion and runtime
performance. See the [recompilation pipeline](recompilation.md) for discovery and
validation, and [OS emulation](os-emulation.md) for native services.

## What an expansion ratio measures

Each MIPS instruction occupies four bytes. For the ordinary instructions selected
for translation:

```text
MIPS code bytes       = translated instruction words × 4
C source expansion   = generated CPU C source bytes / MIPS code bytes
Native code expansion = generated CPU object .text bytes / MIPS code bytes
Host bytes per word  = generated CPU object .text bytes / translated instruction words
```

The denominator must describe the same translated workload as the numerator.
Do not use the whole ROM: it contains textures, audio, levels, data, linked OS
routines and RSP programs as well as CPU instructions. Native hooks replace
their guest bodies rather than translating them, so exclude those bodies from
the translated-instruction count. Unsupported failure boundaries are also not
ordinary translated game code.

For native size, sum `.text` and `.text.*` sections from generated CPU objects.
This includes emitted code and compiler alignment within those sections. It
excludes debug information, unwind metadata, function registry data, shared
runtime implementations, RT64, audio backends and other libraries. The complete
executable's disk size is a separate packaging measurement. Object text size
also precedes any final linker transformations.

The current build splits generated functions into `cpu-*.c` files. Its
`functions.c` now contains the address registry; measuring that file alone would
miss the translated game code. See
[`split_sources`](../tools/generate_game.py).

## Measurements from release builds

The following local build artifacts were measured on 2026-10-15 using GCC
16.2.1 for Linux x86-64 and GNU `size` 2.46.1. Both builds used `Release`,
`RECOMP_OPT_LEVEL=3`, `-O3`, `-g1`, `-fno-omit-frame-pointer`,
`-fno-fast-math -frounding-math -ffp-contract=off`, without LTO. The reference
repository revision was `2b3d18b`; the table describes the existing generated
objects rather than a newly established gameplay or performance milestone.

| Measurement | GoldenEye US | Ocarina of Time US V1.2 |
| --- | ---: | ---: |
| Selected ordinary functions (`ready`) | 2,901 | 13,067 |
| Ordinary MIPS instruction words | 258,520 | 906,346 |
| Ordinary MIPS code bytes | 1,034,080 | 3,625,384 |
| Generated CPU C bytes | 36,075,929 | 130,770,090 |
| Generated CPU object text bytes | 5,375,038 | 22,059,296 |
| C source expansion | 34.89× | 36.07× |
| Native code expansion | 5.20× | 6.08× |
| Native bytes per MIPS word | 20.79 | 24.34 |

The C/text numerators include the small failure stubs in the same CPU files:
seven GoldenEye boundaries and ten Zelda boundaries. The denominators exclude
those bodies and the 67/65 hooked functions, respectively. These are therefore
generated CPU object footprint ratios, including boundary overhead, rather than
an exact sum of ordinary native function symbol sizes. Neither build's selected
function set establishes full-game correctness; see [status](status.md).

Source comments, original instruction addresses, explicit register operations
and control-flow scaffolding account for much of the C expansion. The compiler
does not put comments or labels into executable instructions. Native expansion
reflects a different cost: preserving guest state and semantics on another ISA.
Different function sets, compiler versions and optimization choices change the
ratios; these values are examples rather than a universal N64 expansion factor.

## A real SM64 example: MIPS → C → x86-64

The earlier byte-store example comes from SM64 US function `func_80248c40`.
These four words were checked again against the profile's verified ROM:

```asm
# Guest PC   ROM word
# 80248C40   3C018033
lui  $at, 0x8033
# 80248C44   A020D600
sb   $zero, -0x2A00($at)
# 80248C48   03E00008
jr   $ra
# 80248C4C   00000000
nop                         # return delay slot
```

`lui` puts the sign-extended value `0xFFFFFFFF80330000` in guest register `$at`.
The byte store subtracts `0x2A00`, narrowing the guest address to 32 bits, so it
clears the byte at `0x8032D600`. This is an N64 address, not a host pointer.

The current emitter produces these statements for the first two instructions:

```c
mips_set(ctx, 1, UINT64_C(0xffffffff80330000));
recomp_store(runtime, ctx->gpr[1] - 0x2a00, UINT64_C(0), 1);
```

The generated function also captures the incoming guest return address, executes
the return's delay slot, and checks the destination against that captured
address. A changed destination goes through the function registry. Normal
returns use the host C return path.

Compiling that complete emitted function with GCC 16.2.1 at `-O3` and the
semantic/frame-pointer flags above produced this relevant assembly excerpt
(Intel syntax; surrounding prologue and return paths omitted):

```asm
mov    ecx, 0x1                          # width = 1 byte
xor    edx, edx                          # value = 0
mov    QWORD PTR [rsi+0x8], 0xffffffff80330000  # ctx->gpr[1]
mov    r13, QWORD PTR [rsi+0xf8]           # preserve guest return address
mov    esi, 0x8032d600                    # folded guest store address
call   recomp_store                     # runtime remains in rdi
```

There is no runtime subtraction for the store address: the host compiler
propagated the constant through the inline `mips_set` helper and folded the
calculation. It still writes guest `$at`, because that register value is part of
the guest state. `recomp_store` resolves the guest mapping and performs the
supported access checks and word-swapped memory write.

The complete standalone function occupied **118 bytes of `.text` for 16 bytes
of original MIPS**, or **7.375×**. That includes its prologue, epilogue and checked
return path, but excludes the shared `recomp_store` and `recomp_lookup`
implementations. It is a small-function example, not an estimate for the whole
game. Its object contained 1,114 bytes across all reported sections; using that
total would wrongly count debug information and other metadata as code.

To inspect the same function in a normal SM64 build, import its ROM and run:

```sh
cmake --preset sm64-us-release
cmake --build --preset sm64-us-release -j 4
objdump -d -Mintel --disassemble=recomp_s0_80248c40 \
  build/sm64-us-release/bin/sm64-us
```

Addresses, instruction selection and alignment can differ from the standalone
example. The useful observation is the constant guest address and native helper
call, rather than a fixed instruction listing across all builds.

## Why the compiled code can execute efficiently

### Decode once, compile control flow ahead of time

Rabbitizer decodes CPU instructions during generation. At runtime the game runs
compiled host functions, without a CPU instruction-fetch/decode loop or an
interpreter opcode dispatch for each MIPS operation. Local branches become C
conditions and labels; checked local computed jumps become switches. The host
compiler can lay out and optimize those blocks as native control flow.

MIPS delay slots remain explicit. A branch condition is evaluated before a slot
that could change its inputs, and branch-likely annulment is preserved. Removing
instruction dispatch does not remove these behavioral requirements.

### Expose simple operations to the host compiler

Guest register indices, immediate operands and access widths are constants in
generated C. Small helpers in [`include/recomp.h`](../include/recomp.h), including
`mips_set`, are inline. That lets the compiler fold constants, discard writes to
guest register zero, eliminate redundant expressions and keep intermediate
values in host registers where aliasing and calls permit it. The SM64 example
shows constant propagation across two original instructions.

Release fixed-width loads inline the small direct-RAM path for both constant
and variable guest addresses. Mapped addresses, ROM and MMIO retain shared
readers; Debug retains the checked path. This avoids calls for common RAM reads
at the cost of repeating the address test/load sequence in generated functions.
The [profiling measurements](profiling.md#inline-variable-address-ram-loads-2026-10-18)
record both helper timing and the resulting generated-code growth.

Wrapping arithmetic uses explicit unsigned operations and width/sign-extension
rules. Trapping signed arithmetic can use compiler overflow builtins. This
expresses guest semantics without relying on undefined signed overflow or
requiring a generic instruction decoder at runtime.

These are optimization opportunities, not a promise that all guest registers
stay in host registers. `MipsContext` remains observable shared state at calls
and scheduling boundaries. Memory and floating-point helper calls constrain
optimization across those boundaries. The emitter does not recover the original
source's local variables or perform general register-value analysis itself.

### Replace verified service implementations

Known OS/library routines bind to native implementations after ROM fingerprint
validation. The game can use a host implementation of a queue operation or a
supported memory routine instead of executing the SDK's translated body.

Fixed calls to uniquely registered native hooks use direct handler calls with
`recomp_prepare_direct_call`, preserving section-loading and preparation checks.
Ordinary translated function calls, dynamic indirect calls and ambiguous
addresses still use `recomp_lookup`; they are not all direct native calls.
See [native hook recognition](native-hooks.md).

Graphics and supported audio tasks likewise use RT64 and the native audio
backend. Their implementations are shared services, outside the generated CPU
text counted above. Moving work there reduces the translated CPU workload but
does not make the service itself free.

### Use targeted host instructions while preserving semantics

The current x86-64 floating-point backend uses scalar SSE/SSE2 intrinsics for
supported arithmetic, square roots, conversions and comparisons when `USE_INTRISICS=1`
(the existing option spelling). It saves/restores MXCSR and implements the
supported guest rounding and exception behavior. `USE_INTRISICS=0` selects the
C/libm implementation with full `fenv` isolation. This is individual-operation
acceleration, not automatic SIMD vectorization of the game's loops.
See [floating-point semantics](fpu.md).

Release presets select `RECOMP_OPT_LEVEL=3` for handwritten game code. Explicit
configurations default that option to `2`; [`cmake/Game.cmake`](../cmake/Game.cmake)
accepts either `2` or `3`. Generated CPU code uses the size policy below by
default. Debug uses `-O0 -g3`, so compare expansion and performance using the
intended build mode. Splitting CPU sources makes parallel compilation practical;
it is a build-time improvement, not by itself a runtime optimization.

## Release build policy

`RECOMP_OPTIMIZE_SIZE=ON` is the default. Generated CPU sources use `-Oz` when
supported, otherwise `-Os`, and disable LTO for those objects. MSVC selects
`/O1 /Os /GL-`. Runtime, renderer and handwritten game optimization levels stay
unchanged. Local Linux targets use function/data sections and the game linker
discards unused sections; MSVC uses `/Gy /Gw` and `/OPT:REF /OPT:ICF` without
incremental linking. Dynamic exports, native symbols and unwind information
remain available for the existing crash reporter, profiler and live modules.
Scheduling boundaries, RAM checks and strict floating-point semantics remain
enabled. `RECOMP_OPTIMIZE_SIZE=OFF` restores the generated sources' optimization
settings inherited from the selected game.

`RECOMP_SEPARATE_DEBUG_INFO=ON` moves Linux Release debug sections into a matching
compressed `bin/<game>.debug` file and adds `.gnu_debuglink` to the executable.
The sidecar is a linker byproduct: deleting it triggers a fresh link, and source
changes refresh both files. Separation happens before texture ZIP embedding so
the ZIP's offsets describe the final executable. The regular executable is also
the input to the existing UPX `pack` target. Debug retains embedded information;
Windows MSVC continues to use separate PDB files. Set the option to `OFF` to keep
Linux Release debug sections in the executable instead.

All native game objects and their static runtime libraries support relocation.
Linux executables explicitly link as PIE after checking both C and C++ linker
support. Guest addresses in generated C still refer to the original N64 memory
map; host ASLR relocates native code and registry pointers independently.

The 2026-11-08 Linux builds with this policy and explicit PIE produced these
executable sizes, excluding their optional compressed debug sidecars:

| Game | Previous executable, MiB | Optimized PIE, MiB | Reduction |
| --- | ---: | ---: | ---: |
| Turok US V1.2 | 35.70 | 10.72 | 70.0% |
| Rayman 2 US | 38.44 | 11.37 | 70.4% |
| 1080° US | 44.37 | 12.89 | 71.0% |
| Media Player | 18.42 | 5.94 | 67.8% |

All four Release builds and Turok Debug passed. Release binaries verified as
`ET_DYN` with the PIE dynamic flag and no text relocations. Matching compressed
sidecars resolved `recomp_scheduler_run` back to its source line; symbol tables,
unwind sections and live-repair API exports remained present. Turok Debug also
verified as PIE with embedded debug information.

Temporary CMake integration checks passed with Ninja and Ninja Multi-Config,
including inherited IPO, the `-Os` fallback, Debug, sidecar regeneration, unchanged
incremental builds, rebased texture ZIP offsets after a ZIP update, and distinct
ASLR addresses across launches. Turok completed a 3,300-presentation X11 replay
before and after the change; captures showed rendered first-person gameplay,
and the optimized executable's profiler resolved generated and static native
functions under PIE. The captures differed in camera/weapon state, so these runs
do not establish a frame-time or CPU-speed improvement. MSVC flags were not
built on this Linux host. Reports are under the ignored
`build/release-optimization-2026-11-08/` directory; temporary fixtures were removed.

## Scheduling and function size audit, 2026-11-08

The current emitter does not put an instruction budget or a general resumption
state machine in ordinary generated functions. Guest fibers retain the native
C stack across blocking OS calls. `recomp_scheduler_run(max_resumes)` bounds
scheduler handoffs in shared runtime code; it does not decrement a counter at
every translated instruction. Its compiled body occupied 1,127–1,219 bytes in
the Release executables inspected here. See [native threading](threading.md).

The `switch (start_pc)` in `emit_function(..., resume_entry=True)` belongs to a
separate live-repair module for an interior jump. Normal game generation leaves
that option false. None of the four audited games' normal CPU sources contained
that switch. Checked ROM-specific continuations are local control-flow edges,
not a general scheduler resumption mechanism.

### Executable disk size

These existing Linux x86-64 Release artifacts were inspected without rebuilding
or changing their build settings. Sizes are MiB (1,048,576 bytes). Removing only
debug sections from temporary copies produced:

| Game | Original executable | Without debug sections | Reduction |
| --- | ---: | ---: | ---: |
| GoldenEye US | 72.58 | 29.80 | 58.9% |
| 1080° US | 44.37 | 17.77 | 59.9% |
| Turok US V1.2 | 35.70 | 14.58 | 59.2% |
| Rayman 2 US | 38.44 | 15.46 | 59.8% |
| Media Player | 18.42 | 7.77 | 57.8% |

This saves distribution space without reducing executable instructions or
runtime memory used by loaded sections. An experiment with a separate debug file
on Turok used `objcopy --only-keep-debug`, `strip --strip-debug` and
`objcopy --add-gnu-debuglink`. Every allocated ELF section retained its address,
size and content, and `addr2line` resolved `recomp_scheduler_run` to the same
source line through the sidecar. Keep matching debug files for diagnostics;
discarding them loses source information. Embedded texture ZIPs need their
offsets rebuilt after any operation that changes executable file size, as in
the existing [packing path](../cmake/PackUPX.cmake).

### Loop checkpoint cost

Checkpoints remain necessary for polling devices and preempting translated
loops. They save cached guest registers only when a checkpoint is due, then
reload registers after another thread or native callback may have changed them.
Deleting them can leave a runnable loop executing indefinitely, preventing
device service and the host frame loop from progressing. Removing the scheduler's
resume bound can likewise prevent a run call from returning to presentation.

To measure their size contribution, all 119 cached generated CPU units from
GoldenEye, 1080°, Turok and Rayman 2 were compiled three ways with Clang 22.1.8:
their existing optimization flags, the same flags with every generated
`recomp_checkpoint_due(runtime, true/false)` replaced by `false`, and `-Oz`
with the original checkpoint code. All variants retained the floating-point
semantic flags and frame pointers; `-fno-lto -g0` made their object text directly
comparable. The existing baseline was `-O3` except 1080°, whose cache selected
`-O2`. All 357 compilations passed. Sums below count `.text` and `.text.*`, in
MiB, excluding shared libraries, registry data and debug sections.

| Game | CPU units | Baseline text | Checkpoints removed | Reduction | `-Oz`, checkpoints kept | Reduction |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| GoldenEye US | 34 | 12.12 | 11.00 | 9.18% | 8.49 | 29.95% |
| 1080° US | 34 | 9.75 | 9.34 | 4.24% | 6.52 | 33.08% |
| Turok US V1.2 | 24 | 6.71 | 6.39 | 4.83% | 4.54 | 32.29% |
| Rayman 2 US | 27 | 7.37 | 6.95 | 5.74% | 4.93 | 33.13% |

The checkpoint-free variant was a compile-only size experiment and was never
run. These differences include register-allocation and compiler optimization
effects; they are not an exact attribution of bytes to individual checks.
`-Oz` offers a larger code-size opportunity while keeping scheduling semantics,
but its gameplay and frame-time effects have not been measured. No runtime code,
generation policy or default compiler settings were changed for this audit.

The existing GoldenEye cache additionally enabled ThinLTO. An isolated relink
with its original objects reproduced the original executable size and text
size. Replacing only the 34 CPU objects with the measured ordinary ELF objects,
while retaining the other original objects, libraries and link flags, gave:

| GoldenEye CPU compilation | Final `.text`, MiB | Executable without debug sections, MiB |
| --- | ---: | ---: |
| Existing `-O3 -flto=thin` | 25.78 | 29.80 |
| `-O3 -fno-lto` | 16.44 | 20.43 |
| `-Oz -fno-lto` | 12.81 | 16.81 |

All three relinks succeeded. This is evidence that cross-module optimization
substantially expands this particular build; it does not establish a performance
regression or imply that every game's cache enables LTO. Its speed/size tradeoff
needs measurement before changing the build policy.

### Function fragmentation

`split_sources` groups whole generated functions into C translation units by
section and 32-KiB guest-address buckets. It does not split a guest function into
native fragments for an execution budget. The discovery inventories also showed
almost no duplicated instruction ownership: zero repeated `(section, PC)` words in
GoldenEye, 1080° and Rayman 2; one in Turok, at `0x80000c78`. This counts explicit
instruction addresses, rather than overlapping function address spans.

There were 419, 365, 106 and 606 ordinary functions shorter than eight MIPS words
in GoldenEye, 1080°, Turok and Rayman 2, respectively. Their native function
symbols accounted for only 0.23%, 0.51%, 0.23% and 0.85% of the translated
function-symbol bytes in the existing executables. Small functions and shared
tails may still warrant targeted analysis, but these measurements do not show
widespread duplicated fragments to remove. Merging checked entries globally
would need to preserve indirect-call identity, section loading, relocations and
native return behavior.

The first size reduction to pursue is distribution with separate debug files.
For actual instruction footprint, compare size-oriented compilation and inlining
policy under representative gameplay. Further checkpoint work should reduce
repeated synchronization with proven register data flow while retaining every
required scheduling boundary.

The local audit's input hashes, per-unit compiler commands and measurements are
under the ignored `build/code-size-audit-2026-11-08/` directory. Temporary source
copies, objects and linked executables were removed after recording the results.

## Reproduce the footprint measurement

Build the chosen release preset first, then run this from the repository root.
It counts the instruction addresses of selected `ready` functions and locates
their generated CPU objects through CMake's compilation database. Use a fresh,
completed build so the sources, reports and objects describe the same revision.
Change both arguments to measure another game.

```sh
python3 - build/goldeneye-us-release goldeneye-us <<'PY'
import json
from pathlib import Path
import subprocess
import sys

build = Path(sys.argv[1]).resolve()
generated = build / 'generated' / sys.argv[2]
coverage = json.loads((generated / 'coverage.json').read_text())
symbols = json.loads((generated / 'symbols.json').read_text())
functions = {(f['section'], f['vram']): f for f in symbols['functions']}
words = 0
for entry in coverage['functions']:
    if entry['selected'] and entry['status'] == 'ready':
        f = functions[entry['section'], entry['vram']]
        words += len(f.get('instruction_addresses',
                           range(f['vram'], f['vram'] + f['size'], 4)))

source_bytes = text_bytes = objects = 0
commands = json.loads((build / 'compile_commands.json').read_text())
for entry in commands:
    source = Path(entry['file'])
    if source.parent.resolve() != generated or not source.name.startswith('cpu-'):
        continue
    obj = Path(entry['output'])
    if not obj.is_absolute():
        obj = Path(entry['directory']) / obj
    sections = subprocess.check_output(['size', '-A', str(obj)], text=True)
    for line in sections.splitlines():
        fields = line.split()
        if fields and (fields[0] == '.text' or fields[0].startswith('.text.')):
            text_bytes += int(fields[1])
    source_bytes += source.stat().st_size
    objects += 1

if not words or not objects or not text_bytes:
    raise SystemExit('Missing translated code; configure and complete the build first.')
mips_bytes = words * 4
print(f'{objects} CPU objects; {words:,} ordinary MIPS words ({mips_bytes:,} bytes)')
print(f'CPU C: {source_bytes:,} bytes ({source_bytes / mips_bytes:.2f}x)')
print(f'CPU text: {text_bytes:,} bytes ({text_bytes / mips_bytes:.2f}x)')
print(f'{text_bytes / words:.2f} native bytes per MIPS word')
PY
```

The aggregate `coverage.json` `instruction_count` includes decoded hook and
boundary bodies, so it is not automatically the ordinary translated denominator.
Likewise, a function's `size` can span gaps; use its explicit
`instruction_addresses` when present. The script follows the same convention as
the table, including generated boundary stubs in object text.

## Code size is not execution speed

A 5× byte expansion does not mean 5× as many executed instructions, a 5×
slowdown or a 5× speedup. MIPS uses fixed-width words; x86-64 has variable-width
instructions. Branch frequency, helper work, cache locality and compiler layout
all affect execution cost. Larger code can pressure the instruction cache even
while avoiding interpreter dispatch.

To assess efficiency, compare the same ROM, scene, input sequence, compiler,
renderer settings and frame cap. Measure guest CPU work, helper/lookup costs,
CPU time and frame-time distributions. An FPS cap can hide CPU savings; GPU,
audio and synchronization costs can limit an end-to-end improvement. A faster
floating-point helper or a smaller code footprint alone does not establish a
whole-game speedup.
