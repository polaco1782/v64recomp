# Game support

These are the established port milestones, not new full-game acceptance claims.

This fork uses only the [C11 GLES2 renderer and compiled RSP worker](armv8-gles2.md).
Graphics display-list HLE and renderer selection have been removed. Audio and
JPEG retain their existing native implementations.

Checked graphics programs exist for SM64, GoldenEye, 1080, Shadow Man, Perfect
Dark, Quake, Quake II, Duke Nukem 64, Duke Zero Hour, Nightmare Creatures,
Rayman 2 and Banjo-Kazooie. All twelve build in Debug and Release. GoldenEye has
bounded Dam movement/turning/firing/watch evidence; Perfect Dark reaches the
Carrington Institute room. SM64, 1080 and Shadow Man pass native-RSP startup
checks. Other programs have the scene boundaries recorded in the graphics guide.
Full campaigns, accurate VI framing, listening quality and ARM hardware remain
unverified. The Media Player keeps its independent SDL interface; Bad Apple uses
raw RDP commands without display-list HLE.

The remaining CPU ports (Zelda, Conker, Hexen, Turok, RE2 and TWINE) still need
checked compiled graphics profiles and are not currently enabled for building.
The older CPU milestones below do not establish graphics acceptance in this fork.

- **Bad Apple!! demo**: Build-time download of a pinned external ROM; no ROM is bundled. Windows Debug/Release builds and rendered playback pass. The MMIO store bypass and recurring audio starvation are fixed. A 60-second Release trace had one startup underrun and none afterward; a 30-second Debug trace had zero underruns, exact ROM PCM output and a verified rendered frame. Neither run reset the audio queue. Full-loop, listening and Linux acceptance remain unverified. See the [demo notes](../games/bad-apple/README.md).

- **007: The World Is Not Enough USA revision 0**: Experimental partial port. Original startup, guest threads, VI/AI events, cartridge DMA and fingerprinted MusyX v1 task execution are integrated. Title/menu rendering, playable missions, input acceptance, audible accuracy and save/reload remain unverified. Unconverted module calls fail explicitly. See the [port notes](../games/twine-us/README.md).

- **Banjo-Kazooie US revision 0**: Experimental partial port. Original cartridge DMA/RareZip loading, verified resident/level images and NAudio synthesis run. The Nintendo/Rareware logos render; the user confirmed working audio. Missing CIC-6103 boot state caused the projection check to force clipping planes to 750/1250; restoring the ROM-verified IPL3 values fixes the clipped sky/background. Presentation retains the original steady 30 Hz rate, and idle detection begins after thread initialization. Intro/menu and gameplay callbacks remain in progress; playable progression and save/reload are unverified. See the [port notes](../games/banjo-kazooie-us/README.md).


- **Resident Evil 2 US header revision 1**: Experimental bring-up. Original main-image and overlay decompression run with full decoded-image verification. Opening logos, warning, movie and title menu render on the desktop; Start skips the movie. MusyX v1 synthesis and native video resize/conversion are integrated; gameplay, audible audio and save acceptance remain in progress. See the [port notes](../games/re2-us-v1/README.md).


- **Conker's Bad Fur Day US revision 0**: Debug executable builds and runs original startup, guest threads, paged code, cartridge asset loads, controller polling and scene initialization. The Nintendo logo and 3D N64-logo intro render using the verified custom graphics adapter, and NAudio CBFD tasks produce nonzero PCM. The five-second replay exits successfully; longer intro execution is in progress. Intro audio has been heard on the desktop. Title/gameplay, lighting accuracy and save persistence remain unverified. Remaining CPU paths fail at explicit conversion boundaries. See the [port notes](../games/conker-us/README.md) and [resume handoff](../games/conker-us/RESUME.md).

The keypad FOV/140-degree extension for Duke and OoT has source and
ROM-evidence review only; rebuilds and gameplay acceptance are pending.
Hexen's live matrix correction has Linux Debug/Release build coverage and
user-confirmed live controls; full-range visibility and combined F7 acceptance
remain pending.
GoldenEye has current Debug/Release build coverage; full-range FOV gameplay
acceptance remains pending.
