# Shared native OS and library hooks

## Original-preserving function wrappers

For optional game adapters that must retain translated code, declare a top-level
`function_wrappers` array in the game's complete `profile.toml`:

```toml
function_wrappers = [
  {section = ".game", vram = 0x80001000, size = 0x40, sha256 = "FULL_BODY_SHA256", handler = "game_input_hook"},
]
```

The section, entry, size and full body fingerprint must match discovered code.
A wrapper cannot overlap a native replacement or an unsupported boundary.
The generator retains the original `recomp_s...` body and emits a separate thunk
for both direct calls and registry dispatch. The handler has signature
`void handler(Runtime *, MipsContext *, RecompFunction original)`. Call the supplied
`original` pointer to execute the body without re-entering the same wrapper;
looking up its guest entry again would invoke the wrapper recursively. Calls made
by the original body still follow normal dispatch and wrapper rules.

Wrapped functions retain `ready` coverage; `coverage.json` lists wrappers separately.
This is distinct from replacing an entire function. See GoldenEye's `input.c`
for a scoped adapter.

`include/mouse_input.h` provides a game-independent motion buffer. Host input
submissions accumulate deltas while replacing held movement state. One selected
guest update consumes the deltas; later updates retain held movement only.
Inactive submissions and explicit clearing flush both. Host access occurs while
guest execution is parked; this is not a concurrent producer/consumer queue.
`recomp_renderer_mouse_mode` opts into the SDL mouse/keyboard mapping and capture
policy; each game supplies its own verified gameplay gate and angle adapter.

## Native replacements

`tools/symbols.json` is the shared symbol database for every game, organized
alphabetically by symbol. Each entry groups verified `native_signatures`,
imported `sdk_signatures`, and library `callers` / `callees` indexes.
`tools/native_hooks.py` reads only `native_signatures` and finds complete known
routine bodies within the ROM's configured load regions. An instruction prefix
narrows the search; the entire
body must match its SHA-256 before it is bound to a shared native handler.
Addresses are discovered from the ROM and are not stored in the catalog.

The catalog covers standard OS services for threads, queues, events, clocks,
timers, VI/PI/AI, framebuffer queries, controller input/channel selection,
EEPROM, Controller/Rumble Pak absence, RSP tasks, cache operations and
address translation. It also contains `memcpy`, `memset`, `bcopy`, `bzero`,
`bcmp`, `strlen` and `strchr`. Verified compiler/link variants share the same
native service; adding a variant does not require a game-specific binding.
The older emulator's `sym.c` helped identify candidates; its patch code is not
used. Neither that source tree nor a matching ELF/map is a build dependency.
Recognition is deliberately specific to known compiler/SDK bodies. A changed
instruction or delay slot prevents automatic replacement.

For every new game, reuse the shared catalog for ordinary OS and library calls.
Do not duplicate recognized calls in the game's TOML. If a plain call uses an
unrecognized SDK/compiler variant, verify its behavior and complete ROM body,
then add its fingerprint to that symbol's `native_signatures` in
`tools/symbols.json` with the shared handler.
If the shared handler is missing, implement it in the shared runtime and add
the catalog entry there as well. Keep this rule for future game additions.

Game TOML `hooks` entries take precedence at their explicit address. Reserve
them for game-specific customization and special game or hardware behavior.
For example, GoldenEye and Shadow Man retain their custom cache/TLB and startup
adapters. Existing fingerprints still validate those overrides.
Audio managers that inline `osAiGetLength` can read the 32-bit AI length
register through `recomp_load`; its physical, KSEG0 and KSEG1 addresses use the
same polled native DMA state as the SDK hook. Other direct hardware accesses
remain unsupported.
An unknown routine without an explicit override keeps its translated CPU code.
EEPROM services include the blocking eight-byte `osEepromRead`/`osEepromWrite`
calls as well as the long transfers. The single-block ABI has three arguments;
an unrelated value in the fourth argument register does not change its size.
To disable automatic recognition, a profile can set:

```toml
[native_hooks]
enabled = false
```

With recognition disabled, any required native OS services must be supplied
explicitly by the profile.

Discovery retains existing function names and adds the recognized symbol as an
alias when needed. Complete CPU inventories are checked against the original
ROM, then known library spans are classified as native services in the effective
inventory. This does not require changes to each game's stored code inventory.

OS handlers live in the shared `runtime/` services. The memory/string handlers
live in `runtime/memory.c`. They use guest addresses and the
word-swapped RDRAM layout, preserve each verified routine's return behavior,
and support unaligned bytes, zero-length operations and overlapping `bcopy`.
Aligned blocks use host memory operations. Custom pagers keep separate reads
and writes so faults or evictions cannot invalidate a saved host pointer.

Generated fixed-address calls and explicitly listed indirect targets call verified
native handlers directly when the guest address has a unique registry entry.
They retain loaded-section checks and the optional `prepare_call` callback,
without searching the address registry. Return-address updates, delay slots and
call-site tracking keep their existing behavior. Dynamic indirect calls and
addresses shared by multiple sections still use `recomp_lookup`.


Controller transfers can opt into a completion delay through
`RecompIOConfig.si_transfer_ticks`. Nightmare Creatures uses this to prevent its
continuous high-priority input thread from starving ordinary game logic; zero
retains completion at the next scheduler poll. Both reads and queries snapshot
input when started and publish results and the SI event after the configured
counter deadline.

`osContReset` is a synchronous native service. It waits for any pending read or
query to finish without consuming that transfer's SI event, then fills the
configured channels' `OSContStatus` records from current controller and Pak
state. A disconnected channel receives the no-response error and retains its
other fields, matching the SDK decoder. Reset consumes its own completion
notifications internally, so it leaves no additional SI message for the caller.
Hexen US's complete `0x80058050..0x80058148` body is recognized by the catalog.

## Importing and organizing SDK symbols

The identification signatures come from
[n64sym](https://github.com/shygoo/n64sym), pinned to commit
`ccf4600f3389f1a84bde23339225cf372fdf7712`. The database stores the source URL,
commit and SHA-256, as well as each signature's source ID. Its MIT license is
preserved in [licenses/n64sym.txt](../licenses/n64sym.txt).

The importer validates and merges records, deduplicates variants, sorts symbols
and variants, and rebuilds the forward/reverse library reference indexes. It
preserves native handlers, complete SHA-256 fingerprints and original ROM notes.
Writes are atomic and repeated imports leave identical output untouched.

```sh
# Download the pinned signatures and license, then merge them into the database.
.venv/bin/python tools/merge_symbol_database.py --download

# Or import an already downloaded copy, checked against the pinned source hash.
.venv/bin/python tools/merge_symbol_database.py --n64sym /path/to/signatures.json

# Reorganize after editing native signatures; no network access is needed.
.venv/bin/python tools/merge_symbol_database.py

# Validate the database's canonical organization without writing files.
.venv/bin/python tools/merge_symbol_database.py --check
```

Updating upstream requires both `--revision FULL_COMMIT` and `--sha256 HASH`.
The default never follows a moving branch. A changed upstream license also
requires its verified `--license-sha256`. The one-time `--native-catalog PATH`
option migrates the former TOML catalog, including its preceding ROM comments.

`sdk_signatures` store the body size, prefix/body CRC32 and named relocation
references (`hi16`, `lo16`, `targ26`). These are identification evidence, **not
native replacement authorization**; the importer never creates handlers or
SHA-256 fingerprints from CRCs. Imported names are not automatically added to
ROM discovery or used to seed code. Native replacement still requires review
and an independently verified complete body.

`callers` and `callees` are unions of named `targ26` jump/call references across
SDK variants. They include tail jumps and unresolved library-local labels; an
entry with no signatures exists only as a reference target. These indexes do
not establish which variant a game uses or provide its game-specific callers.
Use `tools/inspect_callbacks.py` against matching ROM metadata for that evidence.
Data/address references remain in each signature and do not become call edges.

## Identifying ROM routines and reviewing handler gaps

Run the SDK scanner against metadata from the build being investigated:

```sh
.venv/bin/python tools/inspect_sdk_symbols.py --game hexen-us \
  --metadata build/hexen-us-debug/generated/hexen-us/symbols.json \
  --output build/hexen-us-debug/sdk-symbols
```

The output directory must be new. It contains `report.txt`, `report.json`, and a
section-qualified `symbols.map`. Reports include the analysis-image identity,
metadata/database/profile hashes, signature provenance, matching variants, body
hashes, callers, decoded SDK call targets, and native-handler coverage. Reports
remain private build data. The scanner never changes names, discovery roots,
profiles, native bindings, or generated sources. A missing handler is a review
item; ordinary recompiled library code may already work correctly.

`--game` prepares compressed/paged code through the registered game adapter and
verifies it against `symbols.json`. `--rom PATH` instead requires that exact
prepared image; it does not assume raw cartridge offsets are decoded offsets.
With `--rom` alone, a metadata native boundary has `binding_unknown` because its
game profile/custom handler selection is unavailable.

The default `--scope entries` examines discovered function entries. Use
`--scope sections` to also scan word-aligned positions throughout selected mapped
sections, including unvisited SDK helpers hidden behind replaced callers. Those
sections can contain data: `unowned` and `owned_interior` matches remain
candidates, not new function roots. Use `--section ID_OR_NAME` to select an
overlay. Equal addresses in multiple sections retain separate records and make
cross-section call targets ambiguous.

For the original Hexen failure, inspect the three relevant symbols explicitly:

```sh
.venv/bin/python tools/inspect_sdk_symbols.py --game hexen-us \
  --metadata build/hexen-us-debug/generated/hexen-us/symbols.json \
  --scope sections --symbol osContReset --symbol __osSiRawStartDma \
  --symbol __osSiDeviceBusy --output build/hexen-us-debug/sdk-reset
```

SDK CRCs are computed after clearing only the operands listed by each variant's
relocations, as described by the pinned n64sym data. Both prefix and complete
body CRCs must match, and the entire body must fit inside its mapped section.
A changed non-relocated instruction or delay slot rejects the match. Confidence
labels describe evidence rather than numeric probabilities:

| Confidence | Evidence |
| --- | --- |
| `verified_native_sha256` | Matches an independently measured complete native signature. |
| `corroborated_crc_candidate` | Prefix/body CRCs match and at least one expected SDK target matches a uniquely identified target in the selected scan. |
| `crc_candidate` | Prefix/body CRCs match without that corroboration. |
| `ambiguous_crc` | Multiple SDK names match the same address in one section; all candidates are retained. |

The symbol map excludes ambiguous names and retains confidence labels. It is a
diagnostic map, not an input to translation. `--symbol NAME` is repeatable and
limits matching; limiting symbols can remove corroborating target evidence.

| Native status | Meaning |
| --- | --- |
| `native_replacement` | Supplied metadata selects a native boundary and its current profile/database handler has a source definition. |
| `variant_unbound` | A registered handler has a source definition, but this address has no selected matching native replacement. Review the variant or disabled-hook policy. |
| `no_registered_handler` | The database has no registered native handler for this symbol. |
| `handler_definition_missing` | A registered/selected handler lacks a definition in shared runtime or the selected game sources. |
| `binding_unknown` | Metadata records a native boundary, but `--rom` supplies no game profile to establish its handler. |

Definition checks are source-level evidence, not proof of successful linking.
`--missing-only` hides selected native replacements in the text report; JSON and
the map retain the complete result. A handler registered under another symbol
can still supply equivalent behavior, so inspect the implementation before
adding a duplicate handler.

Caller evidence distinguishes owned direct instructions and checked indirect
targets from SDK-inferred references in matched bodies. Calls inside replaced
native bodies describe the original ROM, not the native execution path. The
bounded hardware scan recognizes adjacent constant-address memory accesses and
reports the instruction and address; absence of that evidence does not prove
that a function avoids hardware. Review the actual caller boundary before
deciding whether a low-level helper needs its own handler.
