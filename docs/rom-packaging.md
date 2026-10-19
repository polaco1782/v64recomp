# Embedded ROM content

`RECOMP_EMBED_ROM=ON` builds ROM content into the native executable after erasing
verified MIPS CPU instructions. The pinned original ROM remains a build input.
Translation always reads the original/prepared analysis image, before erasure.

```sh
cmake --preset sm64-us-release -DRECOMP_EMBED_ROM=ON
cmake --build --preset sm64-us-release -j 4
./build/sm64-us-release/bin/sm64-us
```

The option works with Debug as well. An embedded executable defaults to its
internal content, and `--rom PATH` explicitly selects an external, original ROM.
Both paths use the shared loader and crash reporter. The returned buffer remains
owned by the app, so existing boot copies, PI DMA, cartridge reads and teardown
keep their current behavior. Texture replacements, saves and renderer state are
still external files.

The runtime checks the expected original ROM identity and size against the
embedded descriptor, then verifies the sanitized payload's own SHA-1 with the
normal ROM attachment function. Crash reports retain the original ROM identity
so they still match generated symbols. Sanitized bytes are never accepted as an
original ROM or substituted into the translation pipeline.

## What is removed

The content has the original size and offsets. Removed bytes become zeros; this
does not currently compress the embedded image or reduce its byte count.

- A complete checked discovery inventory removes entire `code` and `native_os`
  ranges, including unreachable CPU instructions and replaced OS bodies.
- Without that inventory, only explicit `instruction_addresses` from verified
  function metadata are removed. A function's containing span is insufficient:
  gaps can hold tables or constants. Partial discovery leaves undiscovered CPU
  code in the content.
- Data, RSP programs, unclassified bytes, and the first `0x1000` bytes containing
  the cartridge header and IPL boot block are retained by default. A checked
  `packaging.discard` range can remove audited unused content outside discovery;
  SM64 uses this to erase its hardware IPL3 block. The 64-byte header is always
  retained. RSP programs are needed
  for graphics/audio task identification. This is conservative CPU stripping,
  not a guarantee that no MIPS-looking bytes exist anywhere in the executable.

The generator writes these private files beneath
`build/<configuration>/generated/<game>/assets/`:

| File | Purpose |
| --- | --- |
| `rom-assets.bin` | Offset-preserving sanitized ROM content. |
| `rom-assets.json` | Original/payload identity, exact removed ranges and byte count, explicit discard/preservation ranges, inventory coverage scope. |
| `embedded_rom.c` | Content and identity descriptor linked into the executable. |

For a packaging-only audit using existing verified discovery metadata:

```sh
.venv/bin/python tools/package_rom.py --game sm64-us \
  --metadata build/sm64-us-release/generated/sm64-us/symbols.json \
  --output build/sm64-us-package
```

The tool reruns game preparation and validates the metadata's identity, section
hashes, function hashes and instruction ownership. Do not use stale metadata or
offsets from another ROM revision.

## Layout limits and code read as data

The shared packager currently requires the analysis image to equal the original
cartridge image. Uncompressed layouts such as SM64 and Duke fit this model.
OOT, Perfect Dark and Conker append decoded code to their analysis images;
packaging fails explicitly for these games. Their compressed streams can contain
both CPU text and initialized data. They need a verified game-specific repacker
and matching loader changes before code can be removed safely. Embedding their
original compressed streams would retain the CPU code.

Games may read instructions as data for checksums or other behavior. Keep those
bytes using checked exceptions in the same `games/<game>/profile.toml`:

```toml
[packaging]
preserve = [
  # Replace the example offset/size/hash with verified values for this ROM.
  {rom_offset = 0x12340, size = 0x10, sha256 = "<SHA-256 of these original bytes>", reason = "Instruction bytes read by the verified checksum routine"},
]
```

Exceptions require an in-bounds range, matching original-byte SHA-256 and a
reason. They appear in the removal report. Retaining such bytes is necessary
until that consumer has a verified native replacement. Runtime copies that
compare relocated RAM with the same sanitized ROM can remain valid, but this
does not prove every guest code-as-data consumer is safe.

`packaging.discard` uses the same range/hash/reason fields for extra removals.
Discard ranges must be word aligned, exclude the header and avoid inventory data
and RSP ranges. Only add them after tracing the original loader and consumers;
their absence from discovery alone does not prove they are unused.

Validate an embedded build through boot, graphics/audio, input and relevant
level/overlay transitions. Compare with `--rom` when diagnosing a regression.
A complete mapped inventory covers the configured discovery sections; it does
not classify the whole cartridge or prove runtime behavior.

## SM64 validation

The checked SM64 configuration erases 1,229,280 bytes across CPU text, native OS
bodies and unused IPL3 content. Every retained byte matches the pinned original;
the linked executable's embedded array matches `rom-assets.bin` exactly.

Release and Debug builds passed. A bounded Release launch completed 120 frames,
with file-open tracing confirming no external ROM/content image was opened.
A 120-frame Castle Grounds movement replay matched the original-ROM run in its
gameplay summary, captured display lists, screenshots and synthesized WAV bytes.
The final payload also passed a 120-frame jump replay. Payload integrity checks
reject corruption and reject treating the sanitized image as the original ROM.

The automatic lobby route stalled at the same Castle Grounds position
`(x=42.9, z=-196.9)` with both embedded and original content. That replay does not
validate the lobby transition; broader course/overlay acceptance remains open.

ROM-derived outputs and embedded executables contain private game content.
Keep them under ignored `build/` and do not commit them.
