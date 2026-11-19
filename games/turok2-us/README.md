# Turok 2: Seeds of Evil US 1.0

Experimental partial port of the US header-revision-0 cartridge. Debug and Release
build. Original startup, worker threads, resource decompression, Expansion Pak
code loading, virtual-memory paging and ABI1 audio tasks run. A game-owned C11
Acclaim microcode adapter renders the copyright and 3D Expansion Pak notice.
Title/menu and gameplay bring-up remain in progress. Playable gameplay, audio
quality and save/reload acceptance are not established.

## Build and run

Import your own ROM from the repository root:

```sh
.venv/bin/python tools/import_rom.py --profile games/turok2-us/profile.toml \
  --rom '/path/to/Turok 2 - Seeds of Evil (U) [!].z64'
cmake --workflow --preset turok2-us-release
./build/turok2-us-release/bin/turok2-us
```

The normalized ROM is `local/turok2.us.z64`, size `0x2000000`, SHA-1
`fb0400f21e3f043939ab56500c7b12a3231006f1`. Other revisions are rejected.
The game uses 8 MiB RDRAM and a Controller Pak at
`local/saves/turok2-us.mpk`. Save configuration is integrated; persistence has
not been accepted in gameplay. The corresponding Debug workflow is
`cmake --workflow --preset turok2-us-debug`.

## Verified loading and callback discovery

The complete SN64 boot stub installs a global pair of 1 MiB TLB pages at virtual
`0x00200000`, backed by physical `0..0x200000`, and calls `0x0028d380`.
The initial cartridge copy is ROM `0x1000..0x101000` to `0x80000400`.
The original main clears BSS at `0x800c4000..0x8014a000`. Its thread setup uses
FR=1 and the SN64 SDK's `0x230`-byte thread objects. Native SDK replacements
require complete-body catalog fingerprints, including aliases of duplicated
SDK bodies in the ROM.

The Expansion Pak path loads ROM `0x14a000..0x186ae0` to `0x80780000` and maps
a pair of 256 KiB pages at `0x00400000`. The native map boundary verifies all
`0x3cae0` copied bytes before activating the compiled `.virtual` section. The
code bank remains loaded by the original guest loader.

Data reservations beginning at `0x00480000` are paged by the original VM manager
and reader. The C11 pager delivers a checked fault to those guest threads and
requires an installed mapping before retrying. It does not manufacture missing
pages. Startup has exercised page reads, DMA, mapping and caller restart.
Synthetic bootstrap storage occupies the reserved Expansion Pak code-bank tail;
the real main thread retires the boot continuation at its verified first SDK
call (`0x29e3e0`, return `0x288890`) before heap allocation.

`profile.toml` owns verified switch tables, the eighteen engine modes and all
mapping/hook configuration. `startup-entries.json` records ROM-bound callback
producers and consumers. Shared discovery automatically follows fingerprinted
`osCreateThread` entries, proven register/stack callback arguments and constant
indirect calls, including a valid entry sharing a return delay slot.

Before another executable run, review related callback families together:

```sh
.venv/bin/python tools/review_candidates.py --game turok2-us \
  --build-dir build/turok2-us-debug --preflight
```

Read `summary.json`, `ranked.json` and the linked SDK report under that build's
`candidate-review/turok2-us/preflight/`. These reports describe the discovered
graph, not complete game coverage. Menu descriptors, actor state records, class
initializers and embedded animation methods are reviewed as families. Pointer
stores are checked against the corresponding object and indirect-call consumer;
an address-looking value alone does not establish a callback.

## Current graphics boundary

The first captured graphics task uses text `0x800bc400`, data `0x800bf740` and
an ordinary byte-sized display-list descriptor. Exact ROM spans and task hashes:

| Task region | ROM offset | Bytes | SHA-256 |
| --- | --- | --- | --- |
| Graphics text | `0xbd000` | `0x1000` | `962fad8d596512b7a4b95f2c2ad8736f2b09b0dad70ae2cd793100df8ffb42af` |
| Graphics data | `0xc0340` | `0x800` | `c4ba783211d6f282eea4575dde556da810b0a617833ac6b1e7932dbc4610b409` |

RT64's checked-out database matches the text's first `0x1390` bytes with XXH3
`0x07287e3b3682efbb` and data's first `0x420` bytes with
`0xd62bdc3a200cbb35`. Both select `F3DEX2_NON_FIFO_ACCLAIM`, currently assigned
`GBIUCode::Unknown`. The diagnostic hashes use RT64's word-swapped memory layout;
its text hash extends beyond the OSTask's nominal `0x1000` text size.

`graphics.c` binds a standard F3DEX2 command map through the renderer's generic
C microcode interface and overrides vertex loading and MOVEMEM for this ROM's
Acclaim variant. All game-specific command decoding, fingerprints, reset data
and lighting are in this folder; the shared backend only bridges the C callbacks
to RT64. RT64's sources and shader ABI are unchanged.

Every submitted graphics task verifies the complete `0x1390`-byte program and
`0x420`-byte data span in guest memory, including the RSP overlay images beyond
the task descriptor's nominal text size:

| Verified span | SHA-256 |
| --- | --- |
| Text and overlays | `6734f26b67b25c6a521dc2ee56d343d33c8005ea4001c2db83fbabf539e62e57` |
| Microcode data | `462006091cd750bfb2f191d8da8f6221d64fd4257852cacab4aacd94e27b3ff1` |

`lighting.c` implements the compact 16-byte light layout and the original RSP's
signed-halfword Manhattan attenuation and Q16/Q7 arithmetic. It supplies computed
vertex shades while retaining the original normals for texture generation.
The exact ROM's forced-matrix MOVEMEM returns directly to command dispatch and
consumes one command. The implementation follows that continuation rather than
skipping an extra command based on another microcode variant.

The initial black output also required complete-body native bindings for this
SDK's VI mode, special-feature and current-framebuffer routines. Guest SDK
context writes alone did not configure the native VI service. Copyright/Expansion
Pak captures verify that boundary; point-light scenes, title/menu and gameplay
still require rendered acceptance. The Acclaim L3DEX2 line/automap variant is not
implemented by this adapter. The bounded copyright run submitted
ABI1 audio and AI buffers but its captured PCM was silent, so it does not prove
audible output.

The maintained Debug replay `boot-menu.txt` presses Start/A through the startup
notices. Use `--input games/turok2-us/boot-menu.txt --capture-every 120` with a
bounded `--frames` run to reproduce this bring-up path.
The latest replay stops at the unregistered CPU callback `0x2668d4`, called at
`0x26e0d8`. This remains a callback-discovery boundary beyond the verified startup
graphics; the replay has not reached an accepted title/menu or gameplay frame.

The shared snapshot inspector can reproduce the capability check without
launching the game (optional `--rt64` requires system `libxxhash`). Its database
check does not execute or validate this game's custom C hook:

```sh
.venv/bin/python tools/inspect_rsp_task.py local/crashes/turok2-us/REPORT.json \
  --rom local/turok2.us.z64 --rt64 --commands 16
```

The separate DRAM-output DP path also remains under review: this ROM's
`osDpSetNextBuffer` uses a 64-bit length in `a2:a3`, so a conventional two-argument
native binding would be incorrect. The current FIFO graphics task does not
establish that path's acceptance.

## Reference evidence

[mgrz18/turok2-decomp](https://github.com/mgrz18/turok2-decomp) supplies a matching
ROM identity, SN64 naming and VM investigation hypotheses. Its current layout
and microcode notes contain claims that differ from the verified runtime bytes;
the profile and checks above follow this cartridge. In particular, an SDK string
does not prove binary identity or backend decoder support.
[SirBraga/turok2-recomp](https://github.com/SirBraga/turok2-recomp) was also
consulted for hypotheses. Neither external project is a build dependency and no
external runtime implementation was copied. Reference branches were consulted
on 2026-11-11; their later changes do not update this port automatically.

[GLideN64's Acclaim handlers](https://github.com/gonetz/GLideN64/blob/master/src/uCodes/F3DEX2ACCLAIM.cpp)
and [lighting implementation](https://github.com/gonetz/GLideN64/blob/master/src/gSP.cpp)
were consulted for command/layout hypotheses. The C11 implementation was written
independently and checked against the RSP instructions in this ROM.
