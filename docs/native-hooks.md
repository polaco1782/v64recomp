# Shared native OS and library hooks

`tools/native_hooks.toml` is the default recognition catalog for every game.
`tools/native_hooks.py` finds complete known routine bodies within the ROM's
configured load regions. An instruction prefix narrows the search; the entire
body must match its SHA-256 before it is bound to a shared native handler.
Addresses are discovered from the ROM and are not stored in the catalog.

The catalog covers standard OS services for threads, queues, events, clocks,
timers, VI/PI/AI, framebuffer queries, controller input/channel selection,
EEPROM, Controller/Rumble Pak absence, RSP tasks, cache operations and
address translation. It also contains `memcpy`, `memset`, `bcopy`, `bzero`,
`bcmp`, `strlen` and `strchr`. Verified compiler/link variants share the same
native service; adding a variant does not require a game-specific binding.
The older emulator's `sym.c` helped identify candidates; its patch code is not
used. Neither that source tree nor a matching ELF/map is a build dependency.
Recognition is deliberately specific to known compiler/SDK bodies. A changed
instruction or delay slot prevents automatic replacement.

For every new game, reuse the shared catalog for ordinary OS and library calls.
Do not duplicate recognized calls in the game's TOML. If a plain call uses an
unrecognized SDK/compiler variant, verify its behavior and complete ROM body,
then add its fingerprint to `tools/native_hooks.toml` with the shared handler.
If the shared handler is missing, implement it in the shared runtime and add
the catalog entry there as well. Keep this rule for future game additions.

Game TOML `hooks` entries take precedence at their explicit address. Reserve
them for game-specific customization and special game or hardware behavior.
For example, GoldenEye and Shadow Man retain their custom cache/TLB and startup
adapters. Existing fingerprints still validate those overrides.
Audio managers that inline `osAiGetLength` can read the 32-bit AI length
register through `recomp_load`; its physical, KSEG0 and KSEG1 addresses use the
same polled native DMA state as the SDK hook. Other direct hardware accesses
remain unsupported.
An unknown routine without an explicit override keeps its translated CPU code.
EEPROM services include the blocking eight-byte `osEepromRead`/`osEepromWrite`
calls as well as the long transfers. The single-block ABI has three arguments;
an unrelated value in the fourth argument register does not change its size.
To disable automatic recognition, a profile can set:

```toml
[native_hooks]
enabled = false
```

With recognition disabled, any required native OS services must be supplied
explicitly by the profile.

Discovery retains existing function names and adds the recognized symbol as an
alias when needed. Complete CPU inventories are checked against the original
ROM, then known library spans are classified as native services in the effective
inventory. This does not require changes to each game's stored code inventory.

OS handlers live in the shared `runtime/` services. The memory/string handlers
live in `runtime/memory.c`. They use guest addresses and the
word-swapped RDRAM layout, preserve each verified routine's return behavior,
and support unaligned bytes, zero-length operations and overlapping `bcopy`.
Aligned blocks use host memory operations. Custom pagers keep separate reads
and writes so faults or evictions cannot invalidate a saved host pointer.

Generated fixed-address calls and explicitly listed indirect targets call verified
native handlers directly when the guest address has a unique registry entry.
They retain loaded-section checks and the optional `prepare_call` callback,
without searching the address registry. Return-address updates, delay slots and
call-site tracking keep their existing behavior. Dynamic indirect calls and
addresses shared by multiple sections still use `recomp_lookup`.


Controller transfers can opt into a completion delay through
`RecompIOConfig.si_transfer_ticks`. Nightmare Creatures uses this to prevent its
continuous high-priority input thread from starving ordinary game logic; zero
retains completion at the next scheduler poll. Both reads and queries snapshot
input when started and publish results and the SI event after the configured
counter deadline.
