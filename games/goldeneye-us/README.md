# GoldenEye 007 native recompilation

GoldenEye is the second ROM target, separate from the working SM64 profile and
launcher. Game code lives in `games/goldeneye-us/`; generated artifacts live in `build/`.
Run these commands from the `v64recomp` checkout root.

## Original startup, menus and Dam

The startup milestone now displays the original copyright screen and animated
Nintendo logo, using translated ROM CPU code and RT64's GoldenEye graphics HLE.
From the repository root, run:

```sh
.venv/bin/python games/goldeneye-us/run_goldeneye.py --render
```

This opens a desktop window, runs 360 VI frames, captures images every 60 frames,
and exits. The final visible capture is
`build/goldeneye-us/rom/startup/window.png`; `verification.json` records
the pixel check, original startup-data comparison and SP/DP callback counts.
`--render --frames 240` captures the copyright screen. No matching ELF/map,
reference source checkout or CPU emulator is used at build time or runtime.

The original title, file, mode, mission and briefing screens now run, and Dam
loads into its opening camera sequence and playable first-person view. This is
still a partial CPU inventory, not a claim that every mission is converted.

For live play, run:

```sh
.venv/bin/python games/goldeneye-us/run_goldeneye.py --render --frames 0
```

Keyboard controls use arrows for the stick; A/X for N64 A/B; Z to fire; S for
Start; C/V for L/R; I/K/J/L for C-Up/Down/Left/Right; T/G/F/H for the D-pad.
Escape or closing the window ends the session. EEPROM persists at
`local/saves/goldeneye-us.eeprom`; use `--save PATH` for an isolated file.
Original GoldenEye ABI1 tasks now play through SDL. `--wav PATH` records stereo
PCM and `--mute` keeps synthesis active without opening the playback device.

Unlock all Solo missions and regular Cheat Options for a session with:

```sh
.venv/bin/python games/goldeneye-us/run_goldeneye.py --render --cheat-menu
```

This defaults to unlimited live play. Choose a file, then **Cheat Options**;
use the stick and A to choose and toggle cheats. Press **X** (N64 B) to return,
move up to **Select Mission**, and press **A** to open the Solo mission selector.
All 20 missions are available, including Aztec and Egyptian; the original bonus
mission difficulty rules still apply. All 23 normal cheat options are
available, including Invincible, Infinite Ammo and the weapon cheats. The option
overrides the original frontend unlock query after verifying its ROM instructions
and switch table. Original menu drawing, toggles and cheat activation still run
as translated C. Save-completion and earned-cheat bits are not changed by the
override. Mission access uses the original game's all-levels debug flag, whose
getter is also bound to the ROM. Without the option, ordinary unlock rules apply. `--cheat-menu`
requires `--render`; an explicit `--frames N` allows a bounded menu check.

The renderer target now compiles generated CPU code at `-O2` with the existing
floating-point correctness flags. Aligned accesses use native word loads/stores
in word-swapped RAM, retaining the byte path for unusual physical mappings and
other host byte orders. The pager remembers TLB slots and rechecks them after
unmapping or replacement. Scheduler draining stops when only the verified
empty idle thread is runnable. A six-second startup comparison fell from
10.26 to 6.24 seconds of wall time and from 100% to 25% of one CPU core, with
audio underruns falling from 29 to zero. After SDK initialization reaches the
first graphics task, the rendered guest clock advances with host monotonic time,
including during asset loads and heavy frames. Headless checks retain their
deterministic clock. GoldenEye requests a 160 ms playback reserve; SM64 keeps its
80 ms default. Reports include underruns, queue resets and renderer timings.

GoldenEye's raw RDP sky packets arrive as RSP half commands. The native adapter
unpacks these packets and preserves both edge bands, including trapezoids that
cannot be represented by one host triangle. Color and perspective texture
coefficients are evaluated at every resulting vertex. This fixes skipped sky
packets and missing wedges during camera turns. The adapter is scoped to
GoldenEye microcode. This sky fix lives in `games/goldeneye-us/` and does not require
patching RT64 or the legacy emulator sources.

`--startup` runs without a window and stops at the first graphics task. The
default command retains the earlier bounded bootstrap check described below.

Import the supplied ROM and run the bounded native checker from the repo root:

```sh
.venv/bin/python games/goldeneye-us/run_goldeneye.py \
  --rom '/home/cmartin/Downloads/N64/ROMS/007 - GoldenEye.v64'
```

Subsequent commands use `local/goldeneye.us.z64`:

```sh
.venv/bin/python games/goldeneye-us/run_goldeneye.py
.venv/bin/python games/goldeneye-us/run_goldeneye.py --build-only
ASAN_OPTIONS=detect_leaks=0 .venv/bin/python games/goldeneye-us/run_goldeneye.py \
  --sanitize-address --output build/goldeneye-us/rom-asan
.venv/bin/python games/goldeneye-us/run_goldeneye.py --report-only
.venv/bin/python games/goldeneye-us/run_goldeneye.py --startup
ASAN_OPTIONS=detect_leaks=0 .venv/bin/python games/goldeneye-us/run_goldeneye.py \
  --startup --sanitize-address --output build/goldeneye-us/rom-asan
```

The checker compiles ROM-derived MIPS into C and runs the original BSS clear,
`init` and relocated RareZip inflater. Native hooks install the startup page
alias and supply OS services. The original main thread is created with ID 3,
priority 10 and entry `0x7000089c`; the scheduler stops before its first instruction.
There is no window, graphics, audio or gameplay in this bounded check.

The check poisons BSS before running its translated clear loop, checks an end
guard, verifies every relocated inflater byte before activating its compiled
section, and compares all 247,120 decompressed startup bytes against an independent
bounded deflate decoder. Python zlib provides this verification oracle; the native
startup executes the original translated inflater.

Results live in `build/goldeneye-us/rom/boot/`: generated C and headers,
`symbols.json`, `coverage.json`, `rom-layout.json`, `goldeneye_native_boot`,
`startup-data.bin`, `boot-report.json` and `verification.json`. The selected subset
has 18 translated functions, 12 hooks and one explicit stop-before-main boundary.
It does not claim full conversion. ROM bytes and generated game code remain
ignored local artifacts.

## ROM and load mappings

The supplied 16 MiB `.v64` is byte-swapped, region `E`, revision 0, header CRC
`DCBC50D1 09FD1AA3`. Its normalized SHA-1 is
`a963ea5308a4dda41354bca7751446248302a099`. This differs from the US SHA-1 in the
[public decompilation repository](https://github.com/n64decomp/007); the profile
pins the exact supplied dump and rejects different bytes.

| Section | Guest address | ROM offset | Size | Loading |
| --- | --- | --- | --- | --- |
| Startup / native TLB entry | `0x80000400` | `0x1000` | `0xbc` | Initial IPL3 copy |
| Library/system CPU text | `0x700004bc` | `0x10bc` | `0x208d4` | Low physical RAM alias |
| Inflater | `0x70200000` | `0x33590` | `0x15a0` | Original startup relocation |
| Main game CPU text | `0x7f000000` | `0x34b30` | `0xe2d50` | Demand-paged ROM code |

The `1172` block at ROM offset `0x21990` decompresses initialized data and embedded
RSP programs to `0x80020d90`. Ordinary main CPU text is stored directly in the ROM.
Decode jumps at their real virtual addresses: MIPS `j`/`jal` retain upper PC bits.

Mappings were inspected in the supplied ROM and cross-checked during development
against the public [linker layout](https://github.com/n64decomp/007/blob/master/ge007.ld),
[startup](https://github.com/n64decomp/007/blob/master/src/_start.s) and
[init definition](https://github.com/n64decomp/007/blob/master/src/init.c).
Builds and runtime do not read an ELF, map, reference C tree or network resource.

## Native paging and discovery

`--report-only` emits broader discovery under
`build/goldeneye-us/rom/discovery/`. Initially, 1,877 reachable entries were
found: 1,776 ready, 12 hooked and 89 requiring work. This is not a complete CPU
inventory. Unvisited bytes and unresolved indirect transfers stay visible.

The startup profile now registers 2,975 entries: 2,907 translated functions,
61 native OS/hardware hooks and seven explicit unsupported SDK boundaries.
Startup never enters those boundaries. These counts describe discovered CPU
functions, not a complete classification of every ROM byte.

`startup.toml` extends `services.toml` and keeps permanent checked metadata:
`startup-entries.json` pins partial entry points to the supplied ROM and their
instruction prefixes; `dispatches.json` pins 132 local switch tables to the
decompressed ROM data. Callback calls resolve only registered, loaded entries.
An explicit checked fallthrough preserves the original cosine-to-sine wrapper.
Discovery never treats an arbitrary address as a callable function.

The native pager preserves the original 128-entry virtual page table and
90-slot cache of 8 KiB pages. It copies pages from ROM into the game's allocated
cache, updates original slot ownership and counters, evicts/reloads pages and
maintains native TLB aliases. Generated C calls load their original code page,
so the cache competes for the same RAM space that the original game expects.
Writes to paged ROM code and noncontiguous multi-page pointer spans fail explicitly.

Native services cover startup threads, queues, timers, VI/PI/AI/SI, controller
queries/reads and persisted EEPROM. Services initialize after the original
inflater completes: earlier SDK global writes would overwrite compressed input.
GoldenEye's older task ABI leaves audio `ucode_size` unused; recognition checks
the actual immutable program bytes instead. `osViBlack` and `osViRepeatLine`
use longer distinct fingerprints because their function prefixes are identical.

Failures capture thread registers, native stack symbols, recent guest call
targets, task words, pager counts and word-swapped RDRAM alongside the report.
Pass the JSON report to `tools/inspect_crash.py` to resolve the embedded
executable/source paths to C and guest instruction locations:

```sh
.venv/bin/python tools/inspect_crash.py \
  build/goldeneye-us/rom/startup/boot-report.json.crash.json
```

Keep the matching executable and generated C when retaining a crash report;
rebuilding those files can make older return addresses and source lines stale.

## Verification and remaining work

`--input-script PATH` accepts a controller replay JSON with `format_version: 1`,
the pinned `rom_sha1` and an `events` array. Each event has an increasing `frame`
(presentation number), `buttons` (N64 bit mask), and optional `stick_x`/`stick_y`
in -128..127. Pad state is held between events. Replays exercise original menus
and gameplay inputs; they do not change guest game state. Frame captures and
reports remain under `build/goldeneye-us/rom/startup/`.

Desktop checks have entered Dam, captured cloud coverage while turning, exercised
live controller reads, loaded persisted EEPROM, submitted PCM to SDL, and shown
all 23 Cheat Options with Gold PP7 toggled ON. The all-levels option exposes
the 20-mission selector and successfully opens Facility's difficulty screen.
The bounded first-graphics check
also passed ASan/UBSan with byte-identical ROM startup data during development.

Continue performance checks in larger mission views, then complete the CPU
inventory and test mission progression and prolonged gameplay while retaining
bounded bootstrap, first-graphics, menu and Dam checks as regressions.
