# Dumping CPU code from a ROM

Run the shared script against the pinned game ROM and matching discovery metadata:

```sh
.venv/bin/python tools/dump_cpu_code.py --game sm64-us \
  --metadata build/sm64-us-release/generated/sm64-us/symbols.json \
  --output build/sm64-us-cpu-dump
```

If metadata does not exist yet, generate it first:

```sh
.venv/bin/python tools/generate_game.py --game sm64-us \
  --output build/sm64-us-report --report-only
```

Then pass `build/sm64-us-report/symbols.json` to `--metadata`.

The output directory contains:

| File | Contents |
| --- | --- |
| `cpu.bin` | Original big-endian CPU text, packed in analysis-offset order. Includes native OS bodies and unreachable instructions from the checked inventory; excludes data and RSP programs. |
| `cpu.asm` | Disassembly with guest PCs, analysis-image offsets, original words, section headings and discovered function labels. |
| `cpu.json` | Original/analysis identities, completeness scope, total size/hash, and per-range section, guest address, analysis offset, packed offset, size and hash. |
| `ipl3.bin` | Original cartridge bytes `0x40..0x1000`, the separate hardware boot block. |
| `ipl3.asm` | Raw IPL3 disassembly at `0xa4000040`; this block can also contain data, so not every decoded word is proven to be an instruction. |

For SM64, the checked inventory exports 1,225,248 bytes of CPU text plus a
separate 4,032-byte IPL3 block. `cpu.bin` is packed and cannot be loaded back as a
cartridge image. Use each range's `dump_offset` and `size` to locate its bytes,
and `vram` / `image_offset` to recover the original mapping.

The tool verifies ROM identity and metadata hashes and uses the game's prepared
analysis image, so decoded compressed code can be exported. An
`original_rom_offset` is provided only when the containing analysis section
matches original cartridge bytes at that offset; otherwise it is `null`.
Decompressed offsets must not be used to modify compressed cartridge streams.

By default, a complete checked code/data inventory is required. That means all
CPU text in the configured sections, not proof that every CPU instruction across
the whole cartridge has been identified. Other games can export known
instructions with `--allow-partial`; the listing and manifest explicitly label
that output partial. Function gaps are omitted because they may contain data.
IPL3 is always exported separately and does not imply complete game-code coverage.

Existing matching output files are left unchanged. The tool refuses to replace
different existing files. ROM input files and build metadata are never modified.
Keep ROM-derived dumps under ignored `build/` or `local/` directories.
