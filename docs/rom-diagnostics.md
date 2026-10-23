# Collecting ROM failure evidence

Use `tools/diagnose_rom.py` at the start of a model's ROM debugging pass. It
combines the maintained address, callback, crash and RSP inspectors into one
private diagnostic directory. It reads the selected game's pinned cartridge,
runs its preparation hook in temporary storage when needed, and verifies the
result against the supplied `symbols.json`. It does not run the game, regenerate
CPU sources, add roots/hooks, change profiles or edit an inventory.

Always use metadata and a binary from the build that produced the failure.
Keep the failure JSON, its `.rdram.bin`, executable and generated sources before
rebuilding. The cartridge hash cannot establish that two generated builds match.
The collector flags artifacts newer than the crash and disables loaded-section
labels and native source resolution when that warning appears. Recorded native
frames remain in `crash.txt`. Older timestamps alone cannot establish a matching
build; preserve the original files.

## Commands for a model

Start with a concrete crash path, not an automatic latest-report selection:

```sh
.venv/bin/python tools/diagnose_rom.py --game rayman2-us \
  --metadata build/rayman2-us-debug/generated/rayman2-us/symbols.json \
  --crash local/crashes/rayman2-us/crash-ID.json \
  --output build/rayman2-us-report/crash-ID
```

The output directory must be new so an earlier collection cannot be overwritten.
Use `build/` or `local/`; these reports contain private ROM disassembly, addresses,
register values and local paths and must not be committed.

For a static generation failure, omit `--crash`. The collector selects PCs from
unsupported functions in the adjacent `coverage.json`. `--coverage PATH` selects
another coverage report with the same verified analysis identity:

```sh
.venv/bin/python tools/diagnose_rom.py --game rayman2-us \
  --metadata build/rayman2-us-report/symbols.json \
  --output build/rayman2-us-report/static-triage
```

For a missing callback, supply its guest address and a **previously established**
bounded pointer-table range. Ends are exclusive. This Rayman example is its
30-slot command table, whose slot zero is null:

```sh
.venv/bin/python tools/diagnose_rom.py --game rayman2-us \
  --metadata build/rayman2-us-debug/generated/rayman2-us/symbols.json \
  --address 0x800841a4 --data 1:0x800c8b3c:0x800c8bb4 \
  --output build/rayman2-us-report/command-callback
```

Repeat `--address` for the target and its producer/traversal function. Use
`--section ID_OR_NAME` when mapping an address is ambiguous. The default limit
is six addresses; `--limit 12` can expand a focused collection, up to 32.
For a game's graphics task descriptors measured in command counts rather than
bytes, use `--list-scale 8` after proving that format; the default is bytes.

## Reading the collection

Read `report.txt`, then `summary.json`. The JSON includes the exact metadata and
profile SHA-256, original and analysis ROM identities, discovery/coverage
summaries, unsupported functions, section mappings, and suggested next checks.
With a crash, it also retains the original failure operand, all guest thread
registers, recent calls and recorded section states. Selected addresses come
from the failure, current guest PC, last three calls and explicit arguments.
The collector does not assume that a failure address is executable code or
that a recent call proves a caller relationship.

Follow the evidence files according to the failure:

| Evidence | What the model should establish |
| --- | --- |
| `address-ADDRESS.txt` | Section and image offset, exact instruction ownership, entry identity, nearby instructions and delay slots, discovery errors and direct/checked references. |
| `callbacks.txt`, `callbacks.json` | Address materializations, registration sites and supplied table references. Trace the producer and consumer before accepting a target's ABI or table bound. |
| `crash.txt` | Native stack resolved with the matching executable and generated source files. Check artifact-freshness warnings first. |
| `task.txt`, when a task exists | Complete OSTask, microcode/data hashes and identifiers, exact original-cartridge matches and a bounded graphics or audio command listing. |
| `summary.json` | Machine-readable evidence, raw snapshot state, all unsupported functions and individual subtool exit results. A failed subtool is an evidence gap. |

For `unregistered or unloaded function`, inspect entry existence and section
state separately. A registered target in an unloaded section calls for a loader,
DMA or relocation fix; adding the same root again does not repair it. If the
target lacks an entry, prove its pointer source, whole dispatch table and calling
convention. An address inside a containing function span can still be unowned
data or a gap. For RAM/hardware errors, follow the memory operand back to its
writer. For unsupported tasks, identify the actual microcode before selecting
an audio or graphics adapter.

Generation also writes `function-candidates.json` before translation. Review it
for missing entries before the next run. The shared scanner includes raw pointers
in script/assets outside CPU mappings and address uses in owned code; candidate
body checks expose unsupported transfers and overlaps. Use
`tools/inspect_function_candidates.py --game <game> --metadata PATH
--include-registered --output build/<game>-function-candidates.json` to recover
source evidence for an entry that was registered after an older crash. A likely
candidate still needs source/ABI and ownership review before becoming a root.

The scanner also follows constant-base, word-indexed `lw` values into `jalr`
through owned control flow. `indexed-table-call` references connect a candidate
to its table slot, pointer load and call site. The CLI lists tables with likely
missing targets first, and prioritizes their candidates over raw pointer hits.
The `indirect_tables` records describe a bounded scan (at most 256 words), its
stopping reason and `bounds_verified = false`. A neighboring observed base can
be an alias into the same table; a run of code pointers does not establish the
selector's legal range. Verify complete bounds, bytes and ABI before adding a
checked table. Conflicting paths discard address expressions, calls discard
volatile registers, and replaced native bodies do not supply active consumers.
Run the ROM-free regression checks with
`.venv/bin/python tools/check_function_candidates.py`.

Task code/data sizes can include padding, DMEM BSS or adjacent cartridge data.
An ASCII identifier anywhere in that full span does not establish the executing
microcode. Check the initialized spans, descriptor producer and command format.

Update the owning profile, checked inventory, adapter or shared runtime only
after that investigation. Rebuild and replay the exact trigger beyond the old
failure, then collect fresh evidence under a new output directory. Successful
inspection, generation or presentation counts do not establish rendered gameplay.

## SDK identification and missing-handler review

For a hardware-register failure or unidentified SDK routine, generate a symbol
report using the same verified metadata:

```sh
.venv/bin/python tools/inspect_sdk_symbols.py --game hexen-us \
  --metadata build/hexen-us-debug/generated/hexen-us/symbols.json \
  --scope sections --symbol osContReset --symbol __osSiRawStartDma \
  --symbol __osSiDeviceBusy --output build/hexen-us-report/sdk-reset
```

Omit `--symbol` to examine all database signatures; omit `--scope sections` for
the default discovered-entry scan. The report records confidence, ownership,
actual owned/checked caller references, SDK-inferred callers, native binding
status, and bounded direct hardware-access evidence. Section-qualified output
keeps overlays distinct. The diagnostic map excludes ambiguous names.

Check the selected higher-level native boundary before implementing a helper:
Hexen's `__osSiRawStartDma` still exists in the ROM after `osContReset` is replaced,
but calls inside that replaced body no longer execute as MIPS. The report labels
those original-ROM calls. Neither an SDK CRC match nor a missing registered
handler authorizes a replacement or establishes a gameplay failure. See the
[scanner reference](native-hooks.md#identifying-rom-routines-and-reviewing-handler-gaps).

## Comparing an unrecognized SDK body

`tools/compare_native_hooks.py` supplements the collector when a suspected OS
routine has no exact catalog match:

```sh
.venv/bin/python tools/compare_native_hooks.py \
  --rom local/rayman2.us.z64 \
  --metadata build/rayman2-us-debug/generated/rayman2-us/symbols.json \
  --reference-rom local/quake.us.z64 --reference-rom local/duke.us.z64 \
  --output build/rayman2-us-report/native-candidates.json
```

For prepared games, export the exact analysis image first with
`tools/export_analysis_rom.py` and pass that file to `--rom`. Reference files can
also be prepared images. Only complete catalog bodies whose SHA-256 verifies in
a reference image are compared. References never become build dependencies.

The comparison masks potential address operands and compares instruction
structure. Its results are **hypotheses**, including false positives. For example,
`osGetThreadPri` and `osGetThreadId` can differ only in a structure-field offset;
AI length and AI status reads can differ only in a register offset. Review every
reported changed word, callee, constant, global and structure offset, along with
the full body and callers. Then add an exact complete-body fingerprint using the
correct shared handler. Neither tool installs a hook or relaxes runtime checks.
