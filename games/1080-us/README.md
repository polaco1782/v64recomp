# 1080 Snowboarding JU revision 0

Partial native port of the shared Japanese/US cartridge. **The Nintendo 64 logo
renders through RT64 in Debug and Release.** Both completed a 90-frame desktop
replay with exit 0 and no timeout. Debug submitted 35 graphics tasks and made
90 presentations. This is first-graphics acceptance; title/menu navigation,
riding, later module transitions and gameplay save/reload remain unvalidated.

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
Controller 1 is connected to shared renderer input; the acceptance replay sent
no buttons. NEAD 1080 audio tasks execute through the bundled RSP HLE backend.
Debug was muted; Release completed with playback enabled, but audible accuracy
has not been assessed.

## Why bring-up needs module analysis

The resident kernel loads and links Kyoto `.uso` modules. Their code contains
relocated jumps and address pairs; loading another module can bind previously
unresolved imports. Virtual methods and audio callbacks introduce additional
indirect targets. Native execution requires checked entries and the same live
bindings as the compiled image.

`generate.py` prepares the original cartridge's bootup, GUI and N64 logo Text
plus bootup ROD. The inventories bind the cartridge, original section and
relocation hashes, symbol values and complete prepared Text hashes. They contain
metadata, not extracted code. `profile.toml` holds all regions, checked switches,
bootstrap values and explicit unsupported boundaries. `startup-entries.json`
remains a partial entry inventory; its 16-byte hashes identify entries and do
not imply complete function coverage.

The runtime lets the original kernel perform loading. It verifies bootup Text
before activation, checks late-bound import instructions, and verifies complete
GUI/N64 logo Text before their entries execute. These checks currently support
the measured initial allocations. General rebasing, other scene modules and a
complete unload/reload lifecycle are unfinished. Scene Text is hashed at each
entry for correctness; this has not been optimized for gameplay.

All ordinary SDK hooks are in shared `tools/symbols.json`, authorized by complete
body hashes. The shared runtime supplies the SDK global interrupt-mask variant,
early PI status access and the NEAD 1080 audio selection. The game has no private
OS hook table. Graphics microcode identifies as Nintendo F3DEX 1.23 and was
matched to cartridge bytes before RT64 replay.

## Evidence and reference

The 2026-10-23 desktop reports and inspected screenshots are under
`build/1080-us-{debug,release}/logo-verified/` (`run.json`, `run.log`, captures at
0.8 and 1.5 seconds). Both reports say `complete=true`, `timed_out=false`,
`exit_code=0`. Reproduce with:

```sh
RECOMP_AUDIO_MUTE=1 .venv/bin/python tools/desktop_replay.py \
  --executable build/1080-us-debug/bin/1080-us --frames 90 \
  --output build/1080-us-debug/logo-replay --capture-at 1.5 --timeout 20
```

The matching [1080 decompilation](https://github.com/djsaunde/1080-decomp/tree/033dc80261c36564de242aa43b905aed4e0a417f)
provided module and SDK investigation hints. Its cartridge MD5 matches. Layout,
relocations, executable entries and native signatures were checked against our
ROM and live RAM; the external repository is not a build dependency.
See [RESUME.md](RESUME.md) for exact mappings and continuation steps.
