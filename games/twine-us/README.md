# 007: The World Is Not Enough (USA, revision 0)

Experimental partial port, registered as `twine-us`. Native startup and MusyX v1
task execution are integrated. Rendered gameplay, audible accuracy, mission
transitions and save/reload acceptance have not yet been established.

## Pinned cartridge

- Header title: `TWINE`; country `0x45`, revision `0`, entry `0x80000400`.
- Normalized size: `0x2000000` (32 MiB).
- SHA-1: `d347159808f0374a93cf44cfb6135d8f56279f7b`.
- MD5: `9d58996a8aa91263b5cd45c385f45fe4`.
- Private normalized path: `local/twine.us.z64`. Import detects byte order from
  magic; the supplied `.n64` file actually uses byte-swapped byte order.

```powershell
.\.venv\Scripts\python.exe tools/import_rom.py --profile games/twine-us/profile.toml --rom 'C:\tools\007 - The World Is Not Enough (USA).n64'
.\tools\build_msvc_project.ps1 -Game twine-us -Configuration Debug
.\build\msvc\twine-us\debug\bin\Debug\twine-us.exe --frames 900 --mute --capture-every 120
.\tools\build_msvc_project.ps1 -Game twine-us -Configuration Release
.\build\msvc\twine-us\release\bin\Release\twine-us.exe --frames 900
```

Linux uses `cmake --workflow --preset twine-us-debug` or `twine-us-release`.
Debug supports the shared `--input`, `--wav`, `--capture-every` and
`--trace-calls` options. Replays use zero-based frame numbers. Its report and
captures live under `build/msvc/twine-us/debug/debug-output/twine-us/` on Windows.
EEPROM uses `local/saves/twine-us.eeprom`; the first Controller Pak uses that path
with `.pak` appended. Save type/capacity and persistence still need acceptance.

## Verified startup and remaining work

ROM `0x1000..0xd7c50` maps to `0x80000400..0x800d7050`. The entry clears
`0x800d7050..0x80117740`, sets SP to `0x80102ca0`, and enters `0x80000460`.
Original thread creation and native message/event/DMA services run. The synthetic
IPL thread is destroyed at mainproc's checked first call, before game allocation.
Checked local switches remain in `profile.toml`; reviewed callbacks retain their
ROM identity and first-16-byte fingerprints in `startup-entries.json`.

The audio task uses boot `0x800ac110`, code `0x800ad570`, data `0x800d1b30`, and
counts `0xa10`-byte MusyX v1 subframes. Its constant data prefix matches RE2's
verified MusyX v1 table. Code, boot and immutable data fingerprints are checked
before synthesis; the mutable portion of DMEM data is excluded.

The cartridge also contains modules loaded into a shared slot at `0x80117740`.
Their table at `0x800c2c14` has 16-byte records containing BSS start/end and ROM
start/end, indexed by `0x80117386`; loader `0x80044c4c` performs the DMA and BSS
clear. These modules are not yet registered. Calls into unconverted code stop at
explicit generated failure boundaries, listed in `pending-boundaries.json`.
Successful bounded startup runs do not establish title/menu or gameplay support.

## References and local evidence

- [riseio/TWIPE_00Sheven](https://github.com/riseio/TWIPE_00Sheven), inspected at
  `0fd6c00060b6a5da7b838a1b7f38f359d6762688`. This is a separate recompilation
  project. Its resident mapping and overlay hints were checked against this ROM;
  it is not a build dependency and its translated game code was not imported.
- [Mupen64Plus RSP HLE detection](https://github.com/mupen64plus/mupen64plus-rsp-hle/blob/master/src/hle.c)
  identifies TWINE's MusyX v1 data layout. Recognition here additionally binds
  the measured complete code and constant data bytes.
- `tools/symbols.json` contains the reviewed full-body SDK variants used here.
  Masked SDK matches alone never authorize a native replacement.

Use `tools/diagnose_rom.py --game twine-us --metadata <matching-symbols.json>
--crash <failure.json> --output <new-directory>` for a controlled failure.
Keep its executable, generated sources and shared crash artifacts together.
