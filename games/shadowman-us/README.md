# Shadow Man US native recompilation

Shadow Man uses its own ROM profile, application, audio recognition and native
virtual-memory adapter. Generated code and captures stay under
`build/shadowman-us/`; the normalized ROM stays in `local/shadowman.us.z64`.
Run commands from the checkout root.

## Import, build and run

```sh
.venv/bin/python games/shadowman-us/run_shadowman.py \
  --rom '/home/cmartin/Downloads/N64/ROMS/Shadow Man (U) [!].z64'
.venv/bin/python games/shadowman-us/run_shadowman.py --render --build-only
.venv/bin/python games/shadowman-us/run_shadowman.py --render --frames 0
```

`--render` opens an X11/Vulkan window with RT64 graphics, SDL audio and live
controller input. Its default limit is 360 presentations; `--frames 0` runs
until Escape or window close. Keyboard controls are arrows for the stick,
A/X for N64 A/B, S for Start, Z for Z, C/V for L/R, I/K/J/L for C directions,
and T/G/F/H for the D-pad. Use `--jobs N` to limit build parallelism.

The current controller has no accessory. The original game displays
**No Controller Pak found** and offers **Continue without saving**.
Controller Pak persistence and rumble are not implemented.

`--mute` keeps synthesis active without opening an audio device. `--wav PATH`
captures PCM. `--capture-every 60` adds periodic PPM captures beside the report;
the final capture is exported as `boot-report.json.window.png`.

`--input-script PATH` accepts JSON with `format_version: 1`, the pinned
`rom_sha1`, and an `events` array. Events have strictly increasing `frame`
presentation numbers, a `buttons` N64 mask, and optional signed `stick_x` and
`stick_y` values in -128..127. State persists between events. Replays supply
controller input through the normal OS path.

## Bounded checks

```sh
.venv/bin/python games/shadowman-us/run_shadowman.py
.venv/bin/python games/shadowman-us/run_shadowman.py --startup
ASAN_OPTIONS=detect_leaks=0 .venv/bin/python games/shadowman-us/run_shadowman.py \
  --startup --sanitize-address --output build/shadowman-us/rom-asan
.venv/bin/python games/shadowman-us/run_shadowman.py --startup --report-only
```

The default checker runs the original two BSS clears and system initialization,
then stops before the first application thread begins. It poisons BSS, checks
end guards, verifies the startup virtual alias and checks the original thread
ID, priority and stack. `--startup` executes application initialization and
stops all guest threads when the first real RSP task is submitted, before any
backend execution. It does not fabricate SP/DP completion.

Both checks pass ASan/UBSan. A desktop check renders the original Controller
Pak screen with F3DEX 2.08 graphics and ABI1 audio. Rendered pixels, matching
SP/DP callback counts and zero audio underruns/resets were measured. The CPU
inventory is partial; title progression and gameplay are being brought up.

Reports are `build/shadowman-us/rom/boot/verification.json` and
`build/shadowman-us/rom/startup/verification.json`. Failures capture guest
threads, recent call targets, task words, native frames and word-swapped RAM:

```sh
.venv/bin/python tools/inspect_crash.py \
  build/shadowman-us/rom/startup/boot-report.json.crash.json
```

## ROM and native services

The supplied big-endian 32 MiB ROM is US region E, revision 0, header CRC
`3A4760B5 2D74D410`, SHA-1 `cfb99dd949fdb2b2a0002ef595ab1813fe865b2c`.
The profile rejects different bytes and uses 8 MiB expansion RAM.

| Mapping | Virtual address | ROM offset | Extent |
| --- | --- | --- | --- |
| IPL entry / inline TLB setup | `0x80000400` | `0x1000` | `0x12c` |
| System/library region | `0x00100400` | `0x1000` | `0x4a040` |
| Game region, including initialized data | `0x00200000` | `0x1defbe0` | `0xdfdf0` |

The inline boot setup maps virtual `0x00100000` to physical RAM zero. Original
startup clears `0x8004a440..0x80052760`, then `0x8004a440..0x8007e6c0`, and
creates the first application thread at `0x00103298`. The expansion-RAM path
copies game pages into physical `0x00600000` and installs their virtual alias.
CPU code is stored directly in the ROM; original translated code decodes RNX
assets as needed.

Shadow Man's SDK sets Status.FR and uses 0x230-byte thread objects. The shared
runtime now supports 32 independent 64-bit FPU registers while existing games
retain FR=0. Physical VI/RSP addresses bypass CPU page aliases, so low virtual
addresses cannot redirect hardware accesses. Its graphics tasks use
`OS_TASK_LOADABLE`, which permits graphics microcode changes as described in
the [Nintendo graphics manual](https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro25/25-03.html).

`pager.c` turns misses in the game's virtual allocations into native OS fault
events. The original translated fault handler and workers allocate, load,
decompress, evict and map the 4 KiB pages, then restart the requesting thread.
CPU exception instructions do not execute. `startup-entries.json` pins partial
callback roots; switch tables and OS replacements are fingerprinted against the
ROM. There is no build/runtime dependency on a matching ELF, reference source
tree, CPU interpreter or network resource.
