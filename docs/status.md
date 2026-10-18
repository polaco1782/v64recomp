# Game support

These are the established port milestones, not new full-game acceptance claims.

- **Super Mario 64 US**: Working gameplay baseline, with Castle Grounds movement/jumping and the castle lobby transition previously validated. Broader courses, ending and prolonged runs still need validation.
- **GoldenEye 007 US**: Original startup, menus and playable Dam. CPU inventory is partial and remaining unsupported functions fail explicitly. Full-game progression remains unverified.
- **Shadow Man US**: Original startup, graphics, audio and Controller Pak screen. Title/gameplay progression remains in progress; this is not a completed game port.
- **Quake 64 US**: Original menu and playable first level, with movement, turning and shotgun firing previously observed. Full-game stability and saves remain unverified.
- **Duke Nukem 64 US**: Original menus and first level with RT64 graphics, ABI1 audio and live input. Full-game progression remains unverified.
- **Duke Nukem: Zero Hour US revision 0**: Original boot, threads, support-code DMA, ABI1 audio tasks, controller polling and visible copyright/logo screens run in Debug. Title/menu and gameplay bring-up are in progress. See the [port notes](../games/duke-zero-hour-us/README.md).
- **Nightmare Creatures US**: Original title, menus, character selection and playable Chelsea with graphics, ABI1 audio and live input. Full-game progression remains unverified.
- **Hexen US**: Hexen builds cleanly in both Debug and Release and generates its CPU inventory; boot/menu/gameplay validation remains unverified. See the [port notes](../games/hexen-us/README.md).
- **Ocarina of Time US V1.2**: Original title, file select, name entry and new-game opening with verified decompressed code and 469 relocatable overlays. Zelda NEAD audio and native JPEG SP decoding are supported; Link's house background has been checked in the opening. Inventory and full-game validation remain incomplete.
- **Perfect Dark US V1.1**: Original startup, agent creation, EEPROM saving, menus and first-person Carrington Institute rendering with NAudio/MP3 audio. Live input and the user-tested mission transition work. CPU inventory is partial; an intermittent ADPCM decoder trap, broader regressions and full-game progression remain unverified.

- **Conker's Bad Fur Day US revision 0**: Debug executable builds and runs original startup, guest threads, paged code, cartridge asset loads, controller polling and scene initialization. The Nintendo logo and 3D N64-logo intro render using the verified custom graphics adapter, and NAudio CBFD tasks produce nonzero PCM. The five-second replay exits successfully; longer intro execution is in progress. Intro audio has been heard on the desktop. Title/gameplay, lighting accuracy and save persistence remain unverified. Remaining CPU paths fail at explicit conversion boundaries. See the [port notes](../games/conker-us/README.md) and [resume handoff](../games/conker-us/RESUME.md).
