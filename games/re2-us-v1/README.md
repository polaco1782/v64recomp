# Resident Evil 2 US, revision 1

Experimental native bring-up. The opening logos, warning, movie and title menu
render on the desktop, and Start skips the movie. Gameplay, audible audio and
saves are not yet accepted. This is the supplied `Resident Evil 2 (U) [!].z64`, whose
header identifies **revision 1**, not revision 0 or a release named V1.1.

- Original ROM: 64 MiB, big endian, entry `0x80000480`, country `0x45`.
- SHA-1: `62ec19bead748c12d38f6c5a7ab0831edbd3d44b`.
- Imported runtime file: `local/re2.us.rev1.z64`; 8 MiB RDRAM enabled.

From the repository root:

```sh
.venv/bin/python tools/import_rom.py --profile games/re2-us-v1/profile.toml \
  --rom '/path/to/Resident Evil 2 (U) [!].z64'
cmake --workflow --preset re2-us-v1-debug
./build/re2-us-v1-debug/bin/re2-us-v1 --frames 60
cmake --workflow --preset re2-us-v1-release
./build/re2-us-v1-release/bin/re2-us-v1 --frames 60
```

Use a desktop Vulkan session and a wall-time limit during investigation;
`--frames` counts host presentations, not game progress. Debug supports shared
crash reports, `--trace-calls`, `--input`, `--capture-every` and `--wav`.
Routine diagnostic output stays in Debug. Replay frames count host presentations.

Game-specific diagnostic tools live in `debug/check_musyx.c` and
`debug/check_video_dsp.c`. Each source header includes its standalone sanitizer
build command; keep their generated binaries under `build/`.

## Loading model

IPL maps cartridge `0x1000..0x14420` to `0x80000480..0x800138a0`.
Startup clears boot BSS through `0x80018a80`, disables CPU memory watchpoints,
and initializes libultra. The native watch helper accepts only disabled requests
and fails if any watchpoint enable bit is set.

`OVERLAYTABLENUMC` is at cartridge `0x12cd0` / RAM `0x80012150`.
The 16-byte record at `0x12ce0` gives cartridge `0x14420`, compressed size
`0x5e1ef`, and decoded RAM `0x80018a80..0x80136d90`. It starts with a two-byte
header followed by raw DEFLATE. `generate.py` verifies this record, EOF, decoded
size and full SHA-256, then appends its bytes at analysis offset `0x4000000`.
Overlay 3 follows at analysis offset `0x411e310`, overlay 2 at `0x4124fd0`
and overlay 4 at `0x4131070`.
The analysis SHA-1 is `378f7ee4db6fbc6cb0d595849e45c9ee2bd01f4f`.
Prepared offsets must never be used for cartridge DMA or ROM import.

At runtime the original translated inflater runs after native raw PI DMA;
it is not replaced with a preloaded analysis image. Before activating `.main`,
the adapter checks the entire decoded RAM image against its compiled SHA-256.
The loader then clears main BSS `0x80136d90..0x8015b030`, and `0x8004d344`
creates thread 7 at `0x8004d190`. Both the synthetic boot thread and thread 7
have priority zero. The checked `osStartThread` adapter stops IPL after starting
thread 7; the first actual call from thread 7 destroys/joins IPL before its high
RDRAM storage can become a framebuffer.

SDK replacements use complete-body fingerprints in `tools/symbols.json`.
Checked callback roots and switch tables remain partial; their presence does
not classify every mapped byte. Some OS bodies are modified SDK variants;
identification CRCs alone were not used to authorize native replacements.

## Runtime bring-up

The original loader inflates overlays 3 (`0x8015b030`, size `0x6cc0`) and
2 (`0x8017b410`, size `0xc0a0`). Both complete decoded images matched captured
RAM. The adapter invalidates overlapping compiled sections before decompression
and checks the complete decoded hash before activation. The reused loader
thread retires its stopped native stack before receiving a new entry/context.
Other overlay records and the overlapping scene slots remain incomplete.

F3DEX.Rej and other graphics lists reach RT64; their zero `data_size` is
permitted because F3DEX follows list terminators. Native video tasks authenticate
the complete boot and executable microcode plus their vector constants and
validate every input/output span before writing. RE2 selects original-rate VI
presentation: early presentation depends on graphics full-sync events and would
miss CPU/RSP-produced framebuffer updates during the logos and movies.

`video_dsp.c` supplies integer resize, YCbCr conversion and frame blending. Resize preserves
fractional strip offsets, vector rounding, dithering and the RSP's output-DMEM
edge behavior. The movie decoder's complete program is 0x500 bytes at
`0x80184470`; the SDK's fixed 0x1000 size also covers the adjacent fill program
at `0x80184970` and mutable CPU state starting at `0x80184b20`. Those unrelated
bytes are excluded from its executable fingerprint. The complete blend program
at `0x80184970` is 0x150 bytes, ending before the CPU's error string and switch
table. Color conversion retains
the otherwise unwritten alpha bytes left by the RSP's chroma DMA. Synthetic
fixed-point color/fraction/alpha fixtures and all 65,536 byte-blend pairs pass
ASan/UBSan. A bounded desktop replay progresses through the opening movie and
Start reaches the title menu; complete movie playback remains to be exercised.

MusyX v1 tasks use the pinned native DSP implementation with checked descriptor,
sample, history and effect spans. The boot/text fingerprints cover the complete
task images. Its data fingerprint covers the full 0x230-byte constant prefix;
the following bytes include mutable CPU state written at `0x800b41fc`.
Synthetic silence/stereo/PCM/ADPCM and malformed-span fixtures, plus the captured
initial task, pass ASan/UBSan. Audible gameplay and sustained output still need
validation. Unsupported task types fail in the adapter.

See [RESUME.md](RESUME.md) for the latest runtime trigger and retained evidence.

## Public references

[RE2 Studio](https://github.com/theanine3D/re2studio) at commit
`1d10d498374e8a64f4de457311121500e7a03358` is a ROM editing/format toolkit,
not a matching N64 decompilation or a native game port. Its
[Ghidra importer](https://github.com/theanine3D/re2studio/blob/1d10d498374e8a64f4de457311121500e7a03358/ghidra/RE2_Import.py)
and [overlay reader](https://github.com/theanine3D/re2studio/blob/1d10d498374e8a64f4de457311121500e7a03358/Re2.Core/Rom/OverlayTable.cs)
suggested the boot mapping, record format and raw DEFLATE header. Those facts,
the main record, decoded bytes and startup call sites were verified locally
against the pinned ROM. The reference tree is not a build dependency.

[Mupen64Plus RSP HLE MusyX](https://github.com/mupen64plus/mupen64plus-rsp-hle/blob/8a7a472a7172eb2c8725b305eae26818ed7b51a2/src/musyx.c)
at the repository's existing pinned revision supplies native MusyX v1 DSP.
The captured microcode has the v1 identification words used by upstream's
dispatcher; exact ROM-bound hashes and task fields are checked independently.
The [SDK S2DEX documentation](https://jrra.zone/n64/doc/ucode/s2dex/06.htm)
states that F3DEX/S2DEX `data_size` does not describe the essential DL size.

[Salsa64 RE2 video](https://github.com/ricowp81/Salsa64/blob/59cb166c99e7bee2dbf6467814dfab9a13625d1f/src/rsp/jpeg/re2.c)
at revision `59cb166c99e7bee2dbf6467814dfab9a13625d1f` supplies the integer
resize/conversion cores. Its authors describe comparison against actual RSP
execution. Locally, the ROM program boundaries, captured task parameters and
constants, full executable hashes, synthetic arithmetic and rendered warning
screen have been checked; an independent RSP differential run has not been done.
