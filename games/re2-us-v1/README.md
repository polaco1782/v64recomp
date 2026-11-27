# Resident Evil 2 US, revision 1

Experimental native bring-up. The opening movie and title menu render on desktop GLES3.
Some earlier startup frames show corrupted pixels. Start skips the movie. Gameplay, audible audio and
saves are not yet accepted. This is the supplied `Resident Evil 2 (U) [!].z64`, whose
header identifies **revision 1**, not revision 0 or a release named V1.1.

- Original ROM: 64 MiB, big endian, entry `0x80000480`, country `0x45`.
- SHA-1: `62ec19bead748c12d38f6c5a7ab0831edbd3d44b`.
- Imported runtime file: `local/re2.us.rev1.z64`; 8 MiB RDRAM enabled.

From the repository root:

```sh
.venv/bin/python tools/import_rom.py --profile games/re2-us-v1/profile.toml \
  --rom '/path/to/Resident Evil 2 (U) [!].z64'
python3 tools/build.py re2-us-v1 --debug
./build/re2-us-v1-gles-debug/bin/re2-us-v1 --frames 60
python3 tools/build.py re2-us-v1 --release
./build/re2-us-v1-gles-release/bin/re2-us-v1 --frames 60
```

Use a desktop GLES3 session and a wall-time limit during investigation;
`--frames` counts host presentations, not game progress. Debug supports shared
crash reports, `--trace-calls`, `--input`, `--capture-every` and `--wav`.
Routine diagnostic output stays in Debug. Replay frames count host presentations.
The Debug workflow promotes published live modules and reviews bounded batches
of missing-function tables and roots when checked evidence changes. Accepted
entries are saved in `profile.toml`; routine builds reuse them without another
Codex call. Use the `candidate-review-tables` or `candidate-review-roots` CMake
target to review another batch explicitly.

Cartridge saves use 32 KiB SRAM at `local/saves/re2-us-v1.sra` (override with
`--save`). The previous `.mpk` path was a Controller Pak image and is kept
separate. The default controller has no Pak attached.

## Desktop keyboard

WASD and the arrow keys both drive the original analog stick. Return maps to
N64 A and Tab maps to Start; the remaining keyboard bindings and physical
controller mapping are unchanged. The original scene cameras and aiming are
unchanged. Free mouse look and adjustable field of view are not provided because
the fixed camera compositions are tied to their pre-rendered backgrounds.

## GLES3 graphics

The graphics profile binds the captured F3DEX.Rej FIFO
and S2DEX FIFO task code and data to exact spans of the decoded `.main` image.
Its bootstrap and overlay instructions are checked again during generation.
The boot depth command uses a 512-pixel stride (including the immediate at
`0x80025608`), and the guest scissor gives 327 rows. The subsequent 436x383
settings come from the captured VI mode and restored scissor. A later S2DEX
task sets and clears a 508x438 RGBA16 color target at `0x0063e000`.

The video tasks write the movie and other software-generated images directly
into guest RAM. GLES presents the active VI framebuffer from RAM so that
writes between graphics tasks are visible. A 2,600-presentation Debug run with default GLES3 settings
captured distinct movie scenes and the title menu. Some earlier startup
frames were scrambled in guest framebuffer RAM. Release completed 1,200
presentations.

```sh
python3 tools/build.py re2-us-v1 --debug --gles
python3 tools/build.py re2-us-v1 --release --wasm
```

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
Overlay 3 follows at analysis offset `0x411e310`, overlay 2 at `0x4124fd0`,
overlay 4 at `0x4131070` and overlay 5 at `0x413f650`.
Enemy overlay 11 follows at `0x4144850`; its second-slot variant (record 39)
follows at `0x41513e0`. The analysis SHA-1 is
`923396abc251e332452edcb1494965de8c465d40`.
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
The character-confirmation SRAM wait uses the complete-body-verified
`osEPiStartDma` binding; its 32 KiB read/write completion and persistence are
verified. Main gameplay thread entries and their bounded switches are included.

## Runtime bring-up

The original loader inflates overlays 3 (`0x8015b030`, size `0x6cc0`) and
2 (`0x8017b410`, size `0xc0a0`). Both complete decoded images matched captured
RAM. The adapter invalidates overlapping compiled sections before decompression
and checks the complete decoded hash before activation. The reused loader
thread retires its stopped native stack before receiving a new entry/context.
Enemy type 0x10 is included in both fixed slots. Record 11 inflates to
`0x80161cf0..0x8016e880`; record 39 is a raw byte-patch list for the same image
at `0x8016e880..0x8017b410`. The original loader at `0x8003287c` inflates the
source into the selected slot and applies (big-endian u16 delta, byte value,
padding) records at `0x80032ac0..0x80032ae4`. Its 1,910 patches modify
instructions and data pointers. Generation reproduces the complete second
layout from ROM bytes and compiles both variants; it does not depend on a RAM
dump. Runtime validates the complete original image after inflation, invalidates
the overwritten slot, and enables the patched variant only after its complete
hash matches. The original translated patch loop still runs. Other enemy
overlays and module records remain incomplete.

The graphics tasks run the checked F3DEX.Rej and S2DEX RSP programs. Native
video tasks authenticate the boot and executable microcode and vector constants,
then validate the input and output spans before writing the decoded frame.

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

MusyX v1 tasks run the recompiled cartridge program on live RDRAM. The boot/text fingerprints cover the complete
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
documents the v1 identification words used to identify the captured microcode.
The [SDK S2DEX documentation](https://jrra.zone/n64/doc/ucode/s2dex/06.htm)
states that F3DEX/S2DEX `data_size` does not describe the essential DL size.

[Salsa64 RE2 video](https://github.com/ricowp81/Salsa64/blob/59cb166c99e7bee2dbf6467814dfab9a13625d1f/src/rsp/jpeg/re2.c)
at revision `59cb166c99e7bee2dbf6467814dfab9a13625d1f` supplies the integer
resize/conversion cores. Its authors describe comparison against actual RSP
execution. Locally, the ROM program boundaries, captured task parameters and
constants, full executable hashes, synthetic arithmetic and rendered warning
screen have been checked; an independent RSP differential run has not been done.

The enemy byte-patch format and complete second-slot image were established
locally from the original loader and captured RAM. RE2 Studio's pinned overlay
reader enumerates records but only decompresses DEFLATE payloads; its cached
source did not implement these raw patch records. The public OpenBiohazard2
project is a Go/OpenGL engine reimplementation, not a matching decompilation of
this N64 revision.
