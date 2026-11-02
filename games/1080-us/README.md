# 1080 Snowboarding JU revision 0

Partial native port of the shared Japanese/US cartridge. **Debug and Release
render the intro/title and navigate the first menu into Select Rider.** Both
completed the 3,000-frame `debug/first-menu.txt` replay with exit 0 and no timeout:
65.7 seconds in Debug and 51.3 seconds in Release. Captures show the rider
animation and stats. Debug logged 3,491 graphics tasks and 3,000 presentations.

The first-menu freeze was caused by missing mgrproc/genproc module mappings and
reused scene addresses. Both modules now have checked Text, ROD, symbol entries
and bounded switches. Player-controlled riding, later modes/transitions and
gameplay save/reload remain unvalidated. See `RESUME.md` for exact evidence.

## Build and run

The normalized big-endian ROM is 16 MiB, title `1080 SNOWBOARDING`, country
`0x41`, revision 0, SHA-1 `79cd1166c365e5809dec9b62e6d40d6032d5db3a`,
MD5 `fa27089c425dbab99f19245c5c997613`. Supply your own matching cartridge:

```sh
.venv/bin/python tools/import_rom.py --profile games/1080-us/profile.toml \
  --rom '/path/to/1080 Snowboarding (JU).z64'
cmake --workflow --preset 1080-us-debug
cmake --workflow --preset 1080-us-release
./build/1080-us-debug/bin/1080-us --mute --frames 90
./build/1080-us-release/bin/1080-us --frames 90
```

Rendering needs an X11 desktop. The ROM stays in ignored `local/1080.ju.z64`.
The 32 KiB SRAM file is `local/saves/1080-us.sra`: boot initialization exercised
reads/writes and subsequent runs loaded the file. In-game save/reload is pending.
Controller 1 is connected to shared renderer input; the first-menu acceptance
replay sends Start and A. NEAD 1080 audio tasks execute through the bundled RSP HLE backend.
Debug was muted; Release completed with playback enabled, but audible accuracy
has not been assessed.

## Why bring-up needs module analysis

The resident kernel loads and links Kyoto `.uso` modules. Their code contains
relocated jumps and address pairs; loading another module can bind previously
unresolved imports. Virtual methods and audio callbacks introduce additional
indirect targets. Native execution requires checked entries and the same live
bindings as the compiled image.

`generate.py` prepares the original cartridge's bootup, GUI, N64 logo, title,
game, snow-effects, helicopter, mgrproc and genproc Text, plus their checked ROD
when switches need it.
The game Text uses Yay0 compression; stored directory sizes are exact byte
lengths. The inventories bind the cartridge, original section and
relocation hashes, symbol values and complete prepared Text hashes. They contain
metadata, not extracted code. `profile.toml` holds all regions, checked switches,
bootstrap values and explicit unsupported boundaries. `startup-entries.json`
remains a partial entry inventory for resident/observed entries. The separate
`uso-entries.json` records the modules' declared CPU Text symbols, after excluding
checked RSP images and padding. Neither implies complete gameplay coverage.

For module-wide discovery, run the game-specific tool before another replay:

```sh
.venv/bin/python games/1080-us/discover_symbols.py
# After resolving every reported ordinary CPU transfer:
.venv/bin/python games/1080-us/discover_symbols.py --apply
cmake --build build/1080-us-debug -j4
```

This reads original 12-byte USO symbol records and validates all mapped modules
together. It reports unresolved bodies/transfers and only installs the inventory
after static validation passes. Generation revalidates the complete symbol
evidence against the pinned ROM. The tool does not launch a game. New game tools
and their metadata belong in this directory.

Process modules reuse scene addresses. The profile declares overlapping regions
with explicit overlay groups; activation accepts only a matching complete Text
image and invalidates mismatching candidates at the requested address. For
reused switch mappings, `section` names the owning Text and `table_section`
names its checked ROD. The game preparation helper verifies and resolves both.

The runtime lets the original kernel perform loading. It verifies bootup Text
before activation, checks late-bound import instructions, and verifies complete
scene Text before its entries execute. Unresolved JAL imports read the live
instruction installed by the guest linker, capturing the target before the
delay slot executes. These checks currently support the measured allocations.
Other scene modules and a complete unload/reload lifecycle are unfinished.
Scene Text is hashed at each entry for correctness. Generation pins the same
bytes in word-swapped RAM order, allowing a direct full-span hash without a
temporary allocation or byte-at-a-time guest loads.

All ordinary SDK hooks are in shared `tools/symbols.json`, authorized by complete
body hashes. The shared runtime supplies the SDK global interrupt-mask variant,
early PI status access and the NEAD 1080 audio selection. The game has no private
OS hook table. Graphics microcode identifies as Nintendo F3DEX 1.23 and was
matched to cartridge bytes before RT64 replay.

## Evidence and reference

First-menu acceptance reports and inspected captures are under
`build/1080-us-{debug,release}/first-menu-verified/`; both reports say
`complete=true`, `timed_out=false`, `exit_code=0`. Reproduce from the repository
root with either executable:

```sh
RECOMP_LIVE_PATCH_DIR=off RECOMP_LIVE_AUTO=off RECOMP_LIVE_AI=off \
  .venv/bin/python tools/desktop_replay.py \
  --executable build/1080-us-debug/bin/1080-us --frames 3000 \
  --input games/1080-us/debug/first-menu.txt \
  --output build/1080-us-debug/menu-replay --capture-at 48 --timeout 90
```

`build/1080-us-debug/uso-symbol-review/report.json` covers the current 2,489
declared CPU Text entries and reports no unresolved ordinary CPU transfers.
Privileged boundaries remain explicit; this report is not gameplay acceptance.

The earlier successful intro report and 20/27-second captures are retained in
`build/1080-us-debug/module-symbols-verified/`. Superseded probe runs, resolved
crash dumps and temporary standalone checks have been removed; the retained
evidence is listed in RESUME.

The matching [1080 decompilation](https://github.com/djsaunde/1080-decomp/tree/033dc80261c36564de242aa43b905aed4e0a417f)
provided module and SDK investigation hints. Its cartridge MD5 matches. Layout,
relocations, executable entries and native signatures were checked against our
ROM and live RAM; the external repository is not a build dependency.
See [RESUME.md](RESUME.md) for exact mappings and continuation steps.
