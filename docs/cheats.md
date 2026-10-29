# GameShark cheats

F12 opens and closes the shared cheat menu in every rendered Debug and Release
build. The first opening loads cached lists, or downloads missing lists from
[Mupen64Plus](https://github.com/mupen64plus/mupen64plus-core/blob/master/data/mupencheat.txt)
and [Project64](https://github.com/project64/project64/tree/develop/Config/Cheats).
The menu shows each source's download/cache status. Downloads run in a background
thread; gameplay and the menu continue while they complete. An unsuccessful
refresh retains the source's previous cache.

Both ROM header CRC words and the country byte must match a database section.
There is no fallback to a different region or revision based on the title.
Project64 publishes individual files: `Project64Files` in
`backends/cheat_database.cpp` maps the supported cartridges to those filenames,
and each downloaded file's section is independently matched. Mupen64Plus uses
one database and needs no per-game filename mapping. Identical fixed-code
sequences from the two providers appear once; parameterized entries remain
separate. The upstream files retain their original notices in the local cache;
no cheat database is bundled with the executable.

Use each checkbox to enable or disable a cheat. Expand its name to inspect the
source, notes, code words and any selectable values. Choose a value before
enabling a parameterized code. Search filters the list. Disable all active
cheats before using **Refresh downloads**. All cheats start disabled on launch;
enabled state is never silently restored. F11, held during gameplay, supplies
the GameShark button used by button-activated codes.

Mouse input and keyboard navigation (Tab, arrows, Space) operate the menu.
F12, Escape, or the close button dismisses it. Controller and keyboard gameplay
input is suppressed while it is open; guest execution and audio continue.
Opening the menu closes an open RT64 inspector so it cannot cover the menu.
After closing, keyboard input resumes once the held keys are released, so
Escape used to close the menu does not also exit the game.

Disabling stops subsequent RAM writes. It does not roll back values, actions,
or saves already changed by the game. Reloading a save or restarting may be
needed to undo a cheat's effects.

## Code execution

`runtime/cheats.c` implements physical RDRAM operations using the runtime's
word-swapped storage. It supports:

| Types | Operation |
| --- | --- |
| `80`, `A0` / `81`, `A1` | Repeated 8-bit / 16-bit RAM writes |
| `88`, `A8` / `89`, `A9` | RAM writes while the GS button is held |
| `D0`, `D1` | Equal byte / halfword condition |
| `D2`, `D3` | Not-equal byte / halfword condition |
| `D8`, `D9`, `DA`, `DB` | Conditions gated by the GS button |
| `5000CCSS IIII` | Repeat the following write `CC` times, incrementing address by `SS` and value by `IIII` |

Consecutive conditions are combined with AND and govern the next write or
repeat group. The condition state then resets. Mupen64Plus's inline value lists
and Project64's following value/name lines supply `?` placeholders in value
words. Unknown syntax, missing option values, unsupported types, unaligned
halfwords and addresses outside the actual game's RDRAM make the entire cheat
unavailable. Limits bound source files, cheats, code lines and expanded writes.

Boot, encrypted, hardware-control and instruction-patching codes are
unsupported. The validator rejects writes overlapping known direct-mapped
compiled function ranges. Code reached through game-specific paging or
relocation cannot always be identified from those ranges: modifying its RAM
bytes still cannot change the compiled C instructions. A downloaded code being
available does not guarantee its intended effect in a native recompilation.

`backends/rt64.cpp` routes F12 and menu events before RT64's inspector filter.
`backends/cheat_menu.cpp` owns a private ImGui context and Vulkan resources in
both configurations. The SDL owner handles events, builds the UI and validates
and applies enabled programs between guest dispatches, once per input poll.
The present hook only renders completed draw data. All context switches and
draw-data access use RT64's inspector mutex, alongside the Debug HUD and
inspector. The network worker never accesses Runtime or ImGui.

## Downloading and cache

Files live under `<data>/cheats/<CRC1>-<CRC2>-C-<country>/`, where `<data>` is
the renderer data directory selected by `--data` (normally
`local/rt64/<game>`). `Mupen64Plus.txt` and `Project64.txt` are separate raw
source caches. Close and reopen the menu to keep using the current list;
**Refresh downloads** explicitly updates it. Removing a cache file causes the
next launch's first menu opening to download that source again. The folder uses
`C-<country>` so it is valid on Windows; database section matching and the ROM ID
shown in the menu retain `C:<country>`. Older caches in folders with a colon are
downloaded again under the new name.

The downloader uses libcurl on Windows and Linux, with HTTPS-only requests and
redirects, TLS verification, connection/transfer timeouts, an 8 MiB response
limit and cancellation. Windows uses its native certificate store. Successful
responses replace cache files by atomic rename. There is no Python or curl
command-line runtime dependency. vcpkg installs libcurl on Windows; Linux builds
use a system development package or vcpkg, and portable packages include the
linked library and its dependencies.

Build/link checks cover the shared implementation in both configurations.
Interactive menu, network-error handling and in-game cheat acceptance are left
for manual testing.
