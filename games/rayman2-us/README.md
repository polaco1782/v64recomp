# Rayman 2: The Great Escape (US revision 0)

This port targets the 32 MiB big-endian US revision 0 cartridge, header title
`Rayman 2`, country `0x45`, entry `0x80000400`:

- SHA-1: `50558356b059ad3fbaf5fe95380512b9dceaaf52`
- MD5: `03aa4d09fde77eed9b95be68e603d233`

From the repository root:

```sh
.venv/bin/python tools/import_rom.py --profile games/rayman2-us/profile.toml \
  --rom '/path/to/Rayman 2.z64'
cmake --preset rayman2-us-debug
cmake --build --preset rayman2-us-debug -j 4
./build/rayman2-us-debug/bin/rayman2-us --frames 360
```

Use `rayman2-us-release` for the Release configure/build presets. Both modes use
the parent-folder adapter, shared runtime, RT64 build and crash reporter. The
default Controller Pak image is `local/saves/rayman2-us.mpk`; `--save FILE`
selects another image. ROMs, saves and renderer data remain under ignored `local/`.

## Verified loading model

| Section | Cartridge bytes, end exclusive | Guest address |
| --- | --- | --- |
| Boot | `0x1000..0x1dcc0` | `0x80000400` |
| Main | `0x1dcc0..0xc5bf0` | `0x80025c50` |
| Support | `0xc5bf0..0xd0a20` | `0x800f64a0` |

The entry clears BSS `0x8001d0c0..0x80025c50`, then jumps to `0x80000450`.
Its main thread at `0x800004c0` creates the game and pre-NMI threads. The game
thread at `0x80000854` uses the original PI DMA path to load main and support.
The cartridge's disabled compressed-load branch remains translated; no analysis
image preparation or guest interpreter is needed for the observed startup path.

Main CPU text ends at `0x800c6d98`, followed by alignment and initialized data.
Runtime executable visibility tracks through `0x800c6da0`, while discovery also
maps the data containing checked tables. The original asset loader uses
`0x800ca140` as an eight-byte scratch buffer. Including that buffer in runtime
code tracking incorrectly unloaded the main section after a legitimate asset DMA.

Synthetic boot-thread storage is retired at mainproc's first call to
`0x80002690`, before the game's heap initialization can reuse upper RDRAM.
The original main thread has priority zero but continues game work when a flag
is set, so it is not configured as a permanently idle thread.

## Checked discovery

`profile.toml` contains all mappings, bootstrap parameters and dispatch tables.
`startup-entries.json` is deliberately partial and ROM-bound. Its callback roots
come from these producers:

- `osCreateThread` sites at `0x800004a0`, `0x8000064c`, `0x80000688`,
  `0x8008f9f4`, `0x8008fa24` and `0x8009f59c`.
- Audio constructor `0x800ae4fc` passes `0x800af120` through `0x80004a10` and
  `0x80006890` to `0x8000dc44`. That callback returns `0x800aef7c`, stored in the
  audio object at `+0x30` for later indirect invocation.
- `0x8009e1f0` installs the three callbacks `0x8009e230`, `0x8009e27c` and
  `0x8009e2dc` at globals `0x800c9440..0x800c9448`.
- `0x80082f2c` calls the command-handler table at `0x800c8b3c`. Slot zero is
  null; the remaining 29 entries are separate callback bodies.
- Audio objects install methods through `0x8000fb20` at offsets `+4` and
  `+8`; the sample decoder's `+0x28` method is installed at `0x8000d750`.
  `0x800ad4cc` installs the three original default audio callbacks. Audio effect
  constructors `0x800b1d8c`, `0x800b21bc`, `0x800b2530` and `0x800b259c`
  install methods at object `+0x14` / `+0x1c`; `0x800b16bc` and `0x800b358c`
  install the audio service callbacks at `0x800ca168` / `0x800ca1c0`.
- `0x800f964c` constructs its complete 40-slot support method table in BSS
  `0x801012ec..0x8010138c`; the verified methods lie in
  `0x800f99ec..0x800f9f90`. The consumer is the indirect call at `0x800f997c`.
- Lifecycle calls at `0x8002a190` and `0x8002a2c0` select fields 4 and 5 of
  eleven six-pointer records at `0x800c6de0`. The loop bounds are eleven.
- Resource methods at `0x800b5014`, `0x800b5080` and `0x800b50f4` use fields
  2, 5 and 3 of six-pointer records at `0x800ca1d0`. Type 0 and unused types
  contain null callbacks; the type range 1..11 is checked by `0x800ad8d0`.
- The mutable scene callbacks at `0x800c6db0..0x800c6dbc` initially contain
  `0x80029658` and three copies of the original empty callback `0x80026370`.
  `0x800263c8` replaces the last two with `0x80027718` / `0x80027b98`;
  `0x80026a8c` invokes them. These are roots rather than an immutable dispatch table.
- `0x80035f4c` constructs thirteen object callbacks (`0x80035578..0x80035d40`)
  in a stack record. `0x800adf44` copies that record to `0x800c95ec..0x800c961c`;
  `0x800ac41c` invokes its first twelve methods to obtain object parameters.

Ordinary SDK calls use full-body fingerprints in `tools/native_hooks.toml`.
Runtime loaded-section checks and registered indirect-call lookup remain enabled.

```sh
.venv/bin/python tools/generate_game.py --game rayman2-us \
  --output build/rayman2-us-report --report-only
```

Debug supports `--trace-calls`, `--input FILE`, `--wav FILE` and
`--capture-every N`. Replays use `frame buttons stick_x stick_y` text rows.
The shared crash reporter writes failure JSON and word-swapped RDRAM under
`local/crashes/rayman2-us/`.

For a failure, collect evidence before rebuilding:

```sh
.venv/bin/python tools/diagnose_rom.py --game rayman2-us \
  --metadata build/rayman2-us-debug/generated/rayman2-us/symbols.json \
  --crash local/crashes/rayman2-us/crash-ID.json \
  --output build/rayman2-us-report/crash-ID
```

See [the model debugging workflow](../../docs/rom-diagnostics.md).
