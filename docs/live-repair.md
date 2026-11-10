# Live code repair

Live repair is a Debug or Release diagnostic workflow for a **running**
game. A missing function, unsupported-function boundary, or indirect transfer
can pause the guest before it aborts. Edit the game's checked profile or entry
inventory, compile a one-function shared module, and let the paused process
continue without restarting. The process keeps its guest RAM, native thread
stacks and pending OS/device state in memory; it does not save a restartable
state file or modify the ROM.

## Start the game

Build and launch the game normally. Each game waits for repairs in
`local/live-patches/<game>/` by default:

```sh
build/<game>-debug/bin/<game> --rom local/<rom>.z64
```

The executable must have been built with the live repair hooks. On a missing
function, it launches `tools/auto_live_function.py`. The helper checks the
pinned ROM and candidate report, compiles a closed function when the evidence
is clear, and publishes a one-function module. Closed leaf functions without
a stack frame, including two or three instruction return stubs, can qualify
when an owned caller and exact ROM table slot both confirm the callback target.
Candidates with nested indirect calls can qualify when the game profile
explicitly enables registered-target calls; a missing nested target pauses the
game again. A closed nested function can also qualify when an already
published module contains an exact ROM-verified direct call to it. The helper does not edit the game
profile; review the candidate and make a permanent checked change before
rebuilding. The helper writes `auto-<address>.log` in the repair directory and
leaves the game paused if the target is ambiguous or compilation fails.

Set `RECOMP_LIVE_AI=1` to let the helper ask the local Codex CLI to investigate
an ambiguous target. That run uses the workspace-write sandbox and may update
the checked profile or entry inventory before publishing a module. Its
transcript is written to `ai-<address>.log`. Set `RECOMP_LIVE_AUTO=off` to
pause for manual repair without launching the helper.

## Review many candidates before launching

For a game with hundreds of candidates, run `cmake --build build/<game>-debug
--target candidate-review` after generation. This ranks unregistered candidate
tables and independent function call trees, and writes
`build/<game>-debug/candidate-review/<game>/ranked.json`. The report is read-only;
its scanned table lengths are clues, not verified index bounds.

For a missing function family, `tools/probe_function_roots.py` temporarily adds
one candidate root, runs normal discovery against the pinned prepared ROM, and
reports which other functions it reaches. For example:

```sh
.venv/bin/python tools/probe_function_roots.py --game <game> \
  --build-dir build/<game>-debug --address 0x8005d2cc
```

The probe does not change the game profile. To have Codex inspect the pointer,
entry boundary and call tree, then save an accepted root in the game's
`profile.toml`, run `cmake --build build/<game>-debug --target
candidate-review-roots`, or select specific addresses:

```sh
.venv/bin/python tools/review_candidates.py --game <game> \
  --build-dir build/<game>-debug --roots-only --codex \
  --root-address 0x8005d2cc
```

The review checks the ROM fingerprint and regenerates the game before keeping
the TOML change. Accepted roots are reused by normal builds without calling
Codex again. Unresolved roots remain in the report for later review.
Games that set `discovery.auto_candidate_review = true` run bounded table and
root review batches in the normal CMake workflow after live-patch promotion.
The marker at `local/candidate-review/<game>/preflight.json` tracks the checked
profile, entry inventory, review code and runtime missing-call requests;
routine rebuilds skip Codex until
that evidence changes. The explicit `candidate-review-tables` and
`candidate-review-roots` targets review another batch on demand.

To opt into Codex review of the top three table groups, run:

```sh
.venv/bin/python tools/review_candidates.py --game <game> \
  --build-dir build/<game>-debug --codex --max-groups 3
```

Run this between game sessions, when no other process is editing the profile.
Codex inspects each group in a read-only sandbox and returns structured table
proposals. The script checks proposed sites and ROM bytes, adds checked
`[discovery].function_tables` entries or individual verified roots, then runs
normal generation in a separate
build directory. A failed check restores the profile. Logs, proposals and a
results report stay under `build/<game>-debug/candidate-review/<game>/`.
When a selector bound cannot be proved, Codex can instead propose individual
table-backed function roots. Each proposed slot must match the ROM, have a
closed candidate body, and translate as ready. These roots do not assert that
every table index or adjacent pointer is safe.
Each run reviews at most ten groups; omit `--codex` for a report without edits.
Later `--codex` runs skip groups left unresolved in the previous results, so
they can reach new tables. Add `--retry-unresolved` after new evidence is found
for those groups.
Rebuild the game after accepting any entries. This preflight complements the
per-miss live helper; it does not publish modules or resume a paused process.

Set
`RECOMP_LIVE_PATCH_DIR` to an alternate directory, or set it to `off` (or an
empty value) to retain fatal behavior. On a miss it
prints the target address, transfer site when available, and the control file
it is waiting for. Guest execution, scheduler polling and the guest clock stop
while waiting. Use another terminal for the build command; the game window
does not pump events until execution resumes.

## Supply the checked repair

First verify the target and edit `games/<game>/profile.toml` or the game's
ROM-bound entry inventory. Follow [missing-function triage](adding-a-rom.md#8-track-a-missing-symbol-or-guest-function)
for code/data ownership, section loading, ROM bytes and table bounds. A target
in an unloaded section is a loader problem and remains fatal.

For a missing function entry or unsupported-function boundary:

```sh
.venv/bin/python tools/build_live_function.py --game <game> \
  --address 0x80001234 --build-dir build/<game>-debug
```

For a checked indirect call or jump, include the site PC shown by the paused
process. This validates that the updated checked table authorizes the target:

```sh
.venv/bin/python tools/build_live_function.py --game <game> \
  --address 0x80001234 --site 0x80005678 \
  --build-dir build/<game>-debug
```

For a local jump, the helper compiles the containing function with an entry at
the target instruction. The paused old function passes its saved return and
stack context to that entry; after the repaired function returns, the old
native frame returns too. This avoids repeating the original branch delay
slot. For a jump to another function, the helper compiles that function.
Use `--section N` for an ambiguous target or `--site-section N` for an ambiguous
source site when overlays share addresses.

Use `--patch-dir` when the running game has `RECOMP_LIVE_PATCH_DIR` set to a
different directory. The helper uses the game's normal `prepare`, discovery, metadata and
translation checks against the pinned original ROM. It compiles only the
selected function, publishes a uniquely named module, then atomically replaces
`function-XXXXXXXX.path` in the repair directory. The paused process checks
the module ABI, build mode, ROM SHA-1 and active section before running it.
Failed builds leave the old control file in place and the game paused; edit
the evidence and rerun the helper.

For a game process built before automatic invocation was added, run
`.venv/bin/python tools/auto_live_function.py --game <game> --address
0x80001234 --build-dir build/<game>-debug` in another terminal.

To stop waiting and take the original failure path, write `abort` to the
printed `function-XXXXXXXX.path` control file.

The original executable stays loaded. Only the repaired transfer and calls
made by its new module use the new code. Other already compiled call sites
continue to use their existing generated code. Rebuild the normal executable
after the session to make the checked fix permanent. Keep generated modules in
the temporary repair directory; do not add them or private ROMs to the repo.

## Promote repairs after a session

Close the game, then run `cmake --workflow --preset <game>-debug`. Its promotion
step runs before compilation. You can also run `cmake --build
build/<game>-debug --target promote-live-patches` separately. The task reads
published `function-XXXXXXXX.path` files,
adds the roots needed to discover the remaining guest functions to
`games/<game>/profile.toml`, and checks them with normal ROM preparation,
discovery, and translation. Direct callees of those roots and addresses already
registered by a checked table or transfer need no new root. Only after all
published functions match the generated code does the task clear
`local/live-patches/<game>/`. If a check fails, it restores the original profile
and leaves the live patch files for review. Rebuild the game executable before
the next run.

This task promotes translated guest functions. Existing native OS hooks, such
as `osContInit`, require a checked C hook change and a normal rebuild; a live
module cannot override their already registered call sites.
