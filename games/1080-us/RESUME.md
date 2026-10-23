# 1080 Snowboarding continuation notes

## Accepted milestone (2026-10-23)

Debug and Release workflows build. Both 90-frame desktop replays render the
Nintendo 64 logo, exit 0 and complete without timeout. Retained artifacts:
`build/1080-us-debug/logo-verified/` and
`build/1080-us-release/logo-verified/`. Debug logged 35 graphics tasks and
90 presentations. The last interruption, audio callback `0x800dbdc8`, is fixed
with a checked original-code entry; both successful replays pass that point.
There is no currently failing target within this bound. Extend the replay to
find the next transition; title, menus and riding are not accepted yet.

Focused shared validation covered the global-mask SDK variant's preserved IE,
CPU/RCP filtering and zero-RCP special case against the reviewed MIPS semantics,
plus the existing ordinary interrupt-mask handler. The temporary harness was
removed after passing. The shared symbol catalog loads successfully. This is
focused validation, not a full cross-game regression run.

## Exact load model

Original cartridge: `local/1080.ju.z64`, SHA-1
`79cd1166c365e5809dec9b62e6d40d6032d5db3a`, 16 MiB, 4 MiB RDRAM.
Kernel ROM `0x1000` maps at `0x80000400`, size `0xa7c0`. Original stub clears
`0x8000abc0..0x8001ffa0`; Debug checks clearing and an overrun guard. The header
entry `0x80100400` is the IPL load address. IPL3 at ROM `0x6d8/0x6dc` supplies
CIC 6103 value `0x17d7` at `0x80000310`; omitting it triggers the original
security-check panic. The profile supplies that measured value.

| Module | Header ROM | Text ROM | Text VRAM | Text size | Reloc ROM / count |
| --- | --- | --- | --- | --- | --- |
| bootup | `0xd9fe28` | `0xdd0a6c` | `0x80095e50` | `0x89910` | `0xe620ec` / 15543 |
| n64proc | `0x5926dc` | `0x594a58` | `0x802660b0` | `0x430` | `0x5950b0` / 49 |
| gui | `0x58d4ac` | `0x58d534` | `0x8024cbb0` | `0x4880` | `0x592048` / 131 |

The USO marker is `0x12345678`. Headers start at module +4 and are
`kind,size,flags`; data starts after the 12-byte header. BSS kind 8 has no
payload; kind 11 terminates. Relocations are 12-byte records:
`encoded_symbol_and_kind, byte_offset, saved_word_storage`. The loader writes
the original instruction into the third word. Text uses J26/HI16/LO16; ROD uses
ABS32. Earlier notes with Text ROM `0xdd0a68` or base `0x80095e4c` were off by
one word and are superseded by this checked directory layout.

Bootup ROD: ROM `0xd9fe44`, VRAM `0x80065240`, size `0x25a0`, 922 relocations
at ROM `0xe95950`. Prepared analysis offsets live in the profile and must never
be used as cartridge DMA offsets.

`uso-symbols.json` contains 1672 resolved symbol values and 24 initial values
for imports bound when GUI/N64 modules load. Initial bootup Text hash:
`1a7c94c794028a09ff001d4fae47039afed676931bcf67ba300650018a53b48a`.
Linked Text hash:
`551aab4e973d0a93f9e16ba0ca176af4c3163fc51e5bd603c825815c520606df`.
Generated import guards validate live J26 sites; relocated HI/LO instructions
read guest immediates. Module activation runs through canonical-address lookup
before the shared loaded-section check.

The original initial snapshot is
`local/crashes/1080-us/crash-4-978705462886552.json`; the linked snapshot is
`local/crashes/1080-us/crash-3-985183580513219.json`. Keep their adjacent
`.json.rdram.bin` files to reproduce full-image evidence. For example:

```sh
.venv/bin/python games/1080-us/debug/inspect_uso.py \
  --rom local/1080.ju.z64 \
  --crash local/crashes/1080-us/crash-3-985183580513219.json \
  --module-offset 0x58d4ac --text-base 0x8024cbb0 \
  --output build/1080-us-debug/gui-evidence.json
```

This inspector checks complete Text against word-swapped crash RAM and exports
symbol evidence. It does not register code. Current GUI verification matches
all 4640 words. Keep module mappings and tables in the profile, with symbol
inventories as separate ROM-bound evidence.

## Next work

1. Extend the bounded desktop run beyond the logo; capture the first failure or
   new scene module. Diagnose with the shared crash/callback tools and the USO
   inspector. Do not assume the three current modules cover menus or gameplay.
2. Add verified module mappings and callback/table ownership, then handle any
   changed allocations or import bindings. The current adapter deliberately
   rejects different live Text; it is not a general relocation implementation.
3. Validate title, menu selection, riding and a scene transition in both builds,
   then audible output, input behavior and gameplay save/reload. Preserve the
   existing `local/saves/1080-us.sra`.

Standard OS signatures belong in shared `tools/symbols.json`. The explicit
CP0/TLB/exception boundaries in the profile were not reached by the accepted
replay. Do not replace them with success stubs. The public decomp reference and
its pinned revision are linked in README; it supplies hypotheses, not automatic
native replacement authorization.
