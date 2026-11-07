# 1080 Snowboarding continuation notes

## SP-status crash repair and current blocker (2026-11-07)

`local/crashes/1080-us/crash-2290557-475547973434220.json` fails reading
`SP_STATUS` at `0xa4040010`, guest PC `0x8011ed18`. The call chain is the task
scheduler at `0x800f7cd0` -> `osSpTaskYielded` at `0x80119f90` ->
`__osSpGetStatus` at `0x8011ed10`. Load, start and yield already had native
bindings; the yield-query variant was missing.

All 32 instructions of `0x80119f90..0x8011a00f` and all three instructions of
the status getter match the supplied crash RAM and the verified prepared ROM.
The complete query body SHA-256 is
`2ca5c293ab062321cb75121e1f335368a1475e14bfae6aa8212f0243c4819db1`.
It has the standard o32 `OSTask *` ABI: status bit `0x100` determines its return,
and bit `0x80` gates setting `OS_TASK_YIELDED` and clearing `OS_TASK_DP_WAIT`.
This agrees with the reviewed [libultra implementation](https://github.com/n64decomp/libreultra/blob/master/src/io/sptaskyielded.c)
and existing `recomp_os_sp_task_yielded` semantics. Its new shared catalog
fingerprint binds the entire function, removing the direct hardware-read path
without adding low-level SP register emulation.

Both `cmake --workflow --preset 1080-us-debug` and `1080-us-release` pass.
Generated metadata in both builds selects the native replacement, and the
scheduler's generated call invokes `recomp_os_sp_task_yielded` directly.
The SDK scanner confirms that the status getter's only caller is now replaced
natively. Catalog normalization, formatting and project regeneration pass.

The current 3,000-presentation menu replays are **incomplete**, with exit `-6`
and no timeout. Debug reaches Select Rider (48-second capture), then fails at
68.3 seconds; Release fails at 33.1 seconds. Both report
`1080 loaded scene differs from compiled USO` at `0x803f0c2c`. No SP-status fault
occurred, but the Debug log did not exercise the rare yield query before this
separate stop. Keep these artifacts for the next scene investigation:

- `build/1080-us-debug/sp-yield-replay/`: failed replay report/log and Select
  Rider capture.
- `build/1080-us-release/sp-yield-replay/`: failed replay report/log.
- `local/crashes/1080-us/crash-2294861-475962130632338.json` and matching RAM:
  Debug scene failure.
- `local/crashes/1080-us/crash-2296435-476058149026584.json` and matching RAM:
  Release scene failure.

At the new target, neither configured genproc nor game Text matches RAM (14,675
of 14,728 and 18,043 of 18,124 words differ, respectively), and no compiled entry
exists at that address. Identify the loaded USO and its actual allocation from
the loader and crash RAM before adding mappings or roots. Preserve the image
check. This is separate from the missing SDK fingerprint.

## Earlier verified state (2026-11-02)

Debug and Release render the intro/title and navigate Match Race into Select
Rider, including the character animation and stats. Both complete the
3,000-frame `first-menu.txt` replay with exit 0 and no timeout:

| Build | Duration | Evidence directory |
| --- | --- | --- |
| Debug | 65.7 seconds | `build/1080-us-debug/first-menu-verified/` |
| Release | 51.3 seconds | `build/1080-us-release/first-menu-verified/` |

Debug logged 3,491 graphics tasks and 3,000 presentations. The input sends Start
at 26 seconds, A at 30 seconds and A at 34 seconds. Player-controlled riding,
later modes/transitions, audible accuracy and gameplay save/reload are unverified.
The earlier successful idle intro is retained in
`build/1080-us-debug/module-symbols-verified/`.

## Discovery and source files

Use the game-specific symbol tool before replaying a newly mapped module:

```sh
.venv/bin/python games/1080-us/discover_symbols.py
# Install only after every reported ordinary CPU transfer is resolved:
.venv/bin/python games/1080-us/discover_symbols.py --apply
cmake --build build/1080-us-debug -j4
```

The tool reads original 12-byte USO symbol records. The loader at
`0x800025d4..0x80002624` resolves defined symbols as `section_base + value`.
Flags `0x52` identify defined Text symbols; section markers (`0x54`), imports
and data symbols are not function declarations. Checked profile ranges exclude
15 RSP/padding symbols. Normal metadata and instruction validation must pass
before the tool installs an inventory. It never launches a game or guesses
roots from pointer scans.

The current `uso-entries.json` contains 2,489 CPU entries. Discovery registers
2,785 functions: 2,716 ready, 57 SDK hooks and 12 explicit privileged boundaries.
These counts cover mapped modules, not the whole cartridge. Reports are generated
under `build/1080-us-debug/uso-symbol-review/`.

Keep one complete `profile.toml`. `uso.py` handles directories/Yay0;
`uso_symbols.py` reconstructs symbol evidence; `generate.py` verifies inventories,
prepares Text/ROD and emits configuration. `startup-entries.json` retains checked
resident/observed entries. The `*-symbols.json` files pin original section,
relocation and complete prepared Text hashes; they contain metadata, not code.

## Exact load model

ROM: `local/1080.ju.z64`, 16 MiB, SHA-1
`79cd1166c365e5809dec9b62e6d40d6032d5db3a`; RDRAM is 4 MiB.
Kernel ROM `0x1000` maps at `0x80000400`, size `0xa7c0`. The original stub clears
`0x8000abc0..0x8001ffa0`; Debug checks clearing and an overrun guard. Header entry
`0x80100400` is the IPL load address. IPL3 at ROM `0x6d8/0x6dc` supplies CIC 6103
value `0x17d7` at `0x80000310`; the profile preserves this security-check input.

| Module | Header ROM | Text ROM | Text VRAM | Logical Text size |
| --- | --- | --- | --- | --- |
| bootup | `0xd9fe28` | `0xdd0a6c` | `0x80095e50` | `0x89910` |
| n64proc | `0x5926dc` | `0x594a58` | `0x802660b0` | `0x430` |
| gui | `0x58d4ac` | `0x58d534` | `0x8024cbb0` | `0x4880` |
| titproc | `0x595408` | `0x599ccc` | `0x802686d0` | `0x2a20` |
| game | `0x7cc900` | `0x7ccf86` | `0x803ecaa0` | `0x11b30` |
| snoweffects | `0x9023d2` | `0x909926` | `0x803ca590` | `0xa900` |
| heli | `0x7e3d9a` | `0x7f0a82` | `0x802dee00` | `0x1b60` |
| mgrproc | `0x5a0184` | `0x5a0a56` | `0x80264a90` | `0x3420` |
| genproc | `0x5bca4e` | `0x5bd2c2` | `0x803ef230` | `0xe620` |

The normal USO marker is `0x12345678`; snoweffects has a checked zero marker.
Directory records are `kind,size,flags`. BSS kind 8 has no payload; kind 11
terminates. Stored compressed lengths are exact bytes, without implicit word
padding. Named filesystem headers may precede a USO by 40 bytes. Game, mgrproc
and genproc Text use Yay0. Relocation records are 12 bytes:
`encoded_symbol_and_kind, byte_offset, saved_word_storage`. Text uses J26/HI16/LO16;
ROD uses ABS32. All ROD mappings, relocations and checked switches are in the
profile. Prepared analysis offsets must never be used as cartridge DMA offsets.

Bootup's initial Text SHA-256 is
`1a7c94c794028a09ff001d4fae47039afed676931bcf67ba300650018a53b48a`;
its GUI/N64-linked hash is
`551aab4e973d0a93f9e16ba0ca176af4c3163fc51e5bd603c825815c520606df`.
Unbound JAL imports read the live instruction before the delay slot. Relocated
HI/LO instructions read guest immediates. Scene activation hashes complete Text
directly in word-swapped RAM storage at every entry, without allocations.

## First-menu repair and retained evidence

The original stop at `0x80264c2c`, called at `0x800a74b4`, identifies mgrproc.
Its companion genproc enters at `0x803ef6a8` through the guest-linked import at
`0x800b30bc`. Complete RAM verification matched 3,336 mgrproc and 14,728 genproc
Text words. Ten additional bounded switches matched ROM and RAM.

Mgrproc reuses n64proc Text/title ROD; genproc reuses game Text. Explicit overlay
groups describe this reuse. Activation updates all overlapping candidates and
requires a matching full image; stale code remains rejected. Switches with reused
addresses declare `section` for the owning Text and `table_section` for the ROD.
Preparation verifies their fingerprints before resolving checked transfers.
The adapter supports the measured allocations, not arbitrary rebasing.

Five JSON/RAM snapshot pairs remain in `local/crashes/1080-us/`:

- `crash-4-978705462886552.json`: initial bootup image.
- `crash-3-985183580513219.json`: linked bootup and early GUI/N64 evidence.
- `crash-152959-9774774110853.json`: complete intro scene modules and SDK probe.
- `crash-162862-23124539313485.json`: mgrproc menu mapping.
- `crash-165311-23543119497698.json`: genproc menu mapping.

The SDK caller `osVirtualToPhysical` at `0x80116840` has a catalog-authorized
124-byte body. Its shared hook returns signed `-1` for probe misses/invalid page
halves; actual invalid RAM accesses and overlapping mappings still fail. Focused
translation checks passed; their temporary standalone harness was removed.

## Next work

Continue from Select Rider: confirm a rider/board, start Match Race and validate
controls, visible riding and a race/scene transition in both builds. Map new
modules with complete ROM/RAM evidence, then use the static symbol tool; do not
restart per-callback replay loops. Keep SDK hooks in the shared symbol catalog
and preserve fatal privileged boundaries until their exact behavior is implemented.
Retain `local/saves/1080-us.sra`. The public reference and pinned revision are
linked in README; external findings remain hypotheses until locally verified.
