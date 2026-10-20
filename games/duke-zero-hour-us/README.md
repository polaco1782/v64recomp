# Duke Nukem: Zero Hour (US revision 0)

The port uses the original US cartridge, normalized to big-endian bytes:
32 MiB, SHA-1 `de4db292cc6cf5dd1dd1d3c9700cf8e5c3078410`.
The header title is `DUKE NUKEM ZERO HOUR`, country `0x45`, revision `0`.

From the repository root:

```sh
.venv/bin/python tools/import_rom.py \
  --profile games/duke-zero-hour-us/profile.toml \
  --rom '/path/to/Duke Nukem - ZER0 H0UR (U) [!].z64'
cmake --workflow --preset duke-zero-hour-us-release
./build/duke-zero-hour-us-release/bin/duke-zero-hour-us
```

Debug provides the shared CLI for controller replays, captures, WAV output and
recent-call crash reports:

```sh
cmake --workflow --preset duke-zero-hour-us-debug
./build/duke-zero-hour-us-debug/bin/duke-zero-hour-us \
  --frames 1200 --capture-every 120
```

Private ROMs, Controller Pak images and renderer configuration stay in `local/`.
The default Pak is `local/saves/duke-zero-hour-us.mpk`; `--save` selects another
image. The runtime presents one controller and a memory Pak, with no Rumble Pak
or cartridge EEPROM. Game-level save/reload acceptance is pending.

## Load map and checked metadata

The header entry at `0x80000400` clears BSS at
`0x800ea700..0x801c0d80`, sets SP to `0x80119a30`, and jumps to
`0x8000090c`. Original startup creates thread 1 at `0x800009d0`, priority 10,
OSThread `0x800ea700`, stack `0x800eb8b0`. It creates game thread 3 at
`0x80002548`, then becomes the priority-zero idle thread.

The main initialized image maps ROM `0x1000..0xeb300` to
`0x80000400..0x800ea700`. Game thread 3 DMA-loads the support/menu CPU section
from ROM `0xeb300..0xf6d70` to `0x801c0d80..0x801cc7f0`.
The shared PI service activates that section only after the whole checked load
completes. ROM offsets are retained for asset and audio sample DMA.

A synthetic IPL thread uses `0x807ff000` temporarily and is destroyed on
mainproc's first SDK call, before game allocation can reuse its storage.
Both configurations share the parent-folder runtime adapter and use the core
failure reporter. RT64 is reused from the shared renderer build.

`profile.toml` is the single complete configuration. Checked jump tables bind
instruction words, table bytes and lengths established from the bounds checks.
`startup-entries.json` remains a partial inventory. Its entry fingerprints and
pointer/materialization source sites cover thread entries, the game-script
callback table at `0x800def3c`, actor handlers, audio sequence opcodes at
`0x800e1980`, draw callbacks at `0x800df9d0`, and support/menu callbacks.
Ordinary SDK functions use full-body hashes in the shared native catalog.
No external source tree is required to generate this game.

The audio task produced by `0x800a9080` uses the verified ABI1 microcode;
`audio.h` pins its descriptor and initialized boot/text/data hashes.

## Validation boundary

Bring-up has verified original boot/BSS, threads, support-code DMA, ABI1 audio
tasks, controller polling, framebuffer swaps and visible copyright/logo screens.
Title/menu and gameplay acceptance are still being established.

The unused original debug-buffer reset at `0x80003940` contains a literal call
to the unlinked address `0x80000000`. It remains a checked unsupported boundary;
reaching it is an explicit error, not a silent success. The CPU inventory is
partial and full-game progression is unverified.
