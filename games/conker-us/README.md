# Conker's Bad Fur Day US

The US revision 0 port has a native Debug executable. It runs original startup,
creates five guest threads, decompresses paged code on demand into the game's
allocated cache, loads scene assets through PI DMA, polls the controller, and
runs scene initialization. Title/menu rendering and playable gameplay have not
been demonstrated. Remaining unsupported paths fail explicitly.

The audio thread generates its first command list. Its task profile is pinned
to the verified NAudio CBFD microcode; actual synthesis and audible playback
remain unverified. Graphics task support and save persistence are unfinished.
See [RESUME.md](RESUME.md) for addresses and the current replay boundary.

The normalized 64 MiB ROM has SHA-1
`4cbadd3c4e0729dec46af64ad018050eada4f47a` and MD5
`00e2920665f2329b95797a7eaabc2390`.

From the repository root:

```sh
.venv/bin/python tools/import_rom.py \
  --profile games/conker-us/profile.toml \
  --rom "/home/cmartin/Downloads/N64/ROMS/Conker's Bad Fur Day (U) [!].z64"
cmake --preset conker-us-debug
cmake --build --preset conker-us-debug -j 4
./build/conker-us-debug/bin/conker-us --frames 300
```

The bounded replay currently stops at an unsupported boundary. Debug keeps its
report/captures under `build/conker-us-debug/debug-output/conker-us/`; crashes include a JSON
report and RDRAM under ignored `local/crashes/conker-us/`. Release uses the
`conker-us-release` presets and shares the runtime adapter.

Generate a fresh raw CPU report independently of the explicit runtime boundaries:

```sh
.venv/bin/python tools/generate_game.py --game conker-us \
  --output build/conker-us-report --report-only
```

Generation expands 507 RareZip code pages and the game data image, validates
page sizes and decoded SHA-256 hashes, and appends analysis sections to an
in-memory copy of the ROM. Cartridge DMA offsets retain their original meaning.
The checked entry inventory is bound to this prepared image's SHA-1; the import
profile remains bound to the original cartridge SHA-1. Full code classification
is incomplete. The normal build emits a `pending-boundaries.json` inventory;
those failing functions do not implement the missing behavior.

`debug/analyze_switches.py` reports bounded switch candidates for manual review.
It verifies the prepared image and table destinations and never edits metadata.
