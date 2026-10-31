# N64 MP3 Player

`media-player` recompiles the MP3 decoder in lewilou22's **Media Player V2**
homebrew ROM. A small native SDL interface supplies file access, controls and
PCM playback. MP3 decompression and synthesis execute the translated MIPS
functions; there is no native MP3 library in the player.

This is the decoder-only milestone. The original EverDrive SD browser, video
player, libdragon RSPQ/RDPQ menu and microcode are not executed.

## ROM and build

Obtain `MediaPlayer_64.z64` from the [official V2 release](https://github.com/lewilou22/N64-Media-Player/releases/tag/V2)
and import it with `tools/import_rom.py --profile games/media-player/profile.toml --rom PATH`.
The pinned file is 524288 bytes, SHA-1 `e453629d68936382266bd85bc47962078638e818`.
The importer places it at `local/media-player.z64`. The ROM and music stay
external; no downloads happen during builds and embedding is disabled.

```powershell
.\tools\build_msvc_project.ps1 -Game media-player -Configuration Release
.\build\msvc\media-player\release\bin\Release\media-player.exe
```

The **N64 MP3 Player** VS Code tasks/launches and the Visual Studio
`media-player` project select the same target. On Linux:

```sh
cmake --workflow --preset media-player-release
./build/media-player-release/bin/media-player
```

## Controls

Drop an MP3 onto the window, or press **O** / click **Open file** for the Windows
file picker. On Linux, use drag-and-drop or the environment variable below.
**Space** pauses/resumes, **R** restarts, **M** mutes/unmutes, and **Esc** exits.
The pause and restart buttons are also clickable. Mute changes queued output
within about 400 ms. Windows paths support Unicode; the small in-window font
shows ASCII, while the window title retains the full filename.

To select a track at launch:

```powershell
$env:RECOMP_MEDIA_FILE = 'C:\Music\track.mp3'
.\build\msvc\media-player\release\bin\Release\media-player.exe
```

Input is streamed through a 64 KiB buffer. Stereo PCM is queued with a 200 ms
initial prebuffer and a 400 ms target; SDL converts to 48 kHz for the output
device. Mono is duplicated into both channels. No original menu, seeking,
playlist or gapless metadata trimming is implemented. Encoder delay/padding
may be audible between separately opened tracks.

## Evidence and boundaries

The embedded ELF starts at ROM offset `0x1e00`. Its main raw-LZ4 block starts at
`0x2070`, expands from `0x6dbe9` to `0xa7558` bytes, and loads at `0x80000400`.
BSS ends at `0x800b3c68`; adapter scratch starts at `0x80100000` and its stack
at `0x807effe0`. The profile pins the decompressed SHA-256. Preparation and
runtime independently unpack the same authenticated ROM bytes.

The ROM's SYMT v3 table identifies `mp3dec_init` at `0x80038880` and
`mp3dec_decode_frame` at `0x800388a0`, in `vendor/minimp3/minimp3.h`.
Discovery follows their direct calls: **12 functions, 4100 instruction words**,
with no indirect dispatch or native decoder hooks. Other ROM candidate
functions belong to the excluded application and are not registered. The O64
ABI uses a 6668-byte decoder state, 24-byte frame information, signed 16-bit
PCM, four register arguments and an eight-byte information pointer slot at
`sp+32` (the low word at `sp+36`). The prologue at `0x800388b4`/`0x800388f0`
and ELF flags `0x20002001` establish this calling convention.
FR=1 matches the ROM's startup Status register setup. MIPS `TEQ` is lowered
as a conditional shared fatal trap, including its instruction address/code.

References used during reconstruction:

- [Homebrew source snapshot](https://github.com/lewilou22/N64-Media-Player/tree/c9e6e761dd08dc538809b9d75e42d1cf7af26394):
  the advertised `sdmp3` sources are absent from this snapshot; the released
  combined ROM and its embedded symbols are the evidence for this port.
- [libdragon SYMT implementation](https://github.com/DragonMinded/libdragon/blob/bb89f105e5b636dda0610885595511f4313fa0e0/src/symtable.c):
  reference revision from the ROM's `libdragon.version` (marked dirty).
- [minimp3 API](https://github.com/lieff/minimp3/blob/master/minimp3.h):
  reference for the ABI, checked against the ROM and audio output.

## Diagnostics

Debug accepts the common `--wav FILE` capture flag. `--frames N` limits decoded
MP3 frames and exits after queued audio drains, or at EOF; a bounded run
requires `RECOMP_MEDIA_FILE`. Captures contain decoder PCM before mute and
resampling. A sample-rate change stops the WAV capture and preserves its
existing audio, while interactive playback continues. Set
`RECOMP_MEDIA_SNAPSHOT` to a BMP path for a final Debug screenshot.

```powershell
.\tools\build_msvc_project.ps1 -Game media-player -Configuration Debug
.\.venv\Scripts\python.exe games/media-player/debug/validate_audio.py `
  --exe build/msvc/media-player/debug/bin/Debug/media-player.exe `
  --ffmpeg PATH/TO/ffmpeg.exe --output build/media-player-validation
```

The fixture generates original transient/tonal stereo CBR, mono MPEG-2 and
stereo VBR MP3s, compares captured PCM against FFmpeg, and checks playback
underruns. `tools/check_teq.py` separately executes synthetic trap, 64-bit
comparison, delay-slot and likely-branch annulment cases without a ROM.
Run the latter from Developer PowerShell for MSVC.
