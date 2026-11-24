# Desktop input, FoV and GLES2 coverage

This covers the exact 22 game profiles in the tree, not the separate Bad Apple
and Media Player demos. A feature listed here is implemented, not necessarily
accepted in gameplay. RT64 remains the default. "Offscreen" uses the software
GLES2 backend without physical desktop input; a boot or menu check does not
establish gameplay, camera quality, audio playback, or hardware compatibility.

| ROM profile | WASD / mouse camera | Adjustable FoV | GLES2 evidence and remaining limits |
| --- | --- | --- | --- |
| [1080 Snowboarding JU](../games/1080-us/README.md) | WASD and arrows; mouse camera pending | Pending riding camera | Debug/Release build; rendered title/language. Riding not verified; longer Debug replay hit the existing SP completion/yield mismatch. |
| [Banjo-Kazooie US](../games/banjo-kazooie-us/README.md) | WASD, orbit, first-person look | Yes | Earlier game-local check reached Spiral Mountain on both renderers; climbing camera verification remains open. |
| [Conker US](../games/conker-us/README.md) | Opt-in WASD and mouse buttons; camera pending | Pending gameplay | Compiled F3DEXBG with a checked constant data prefix and live task parameters. |
| [Doom 64 US](../games/doom64-us/README.md) | WASD, mouse turn | Yes | Prior Staging Area replay, shotgun pickup and automap. |
| [Duke Nukem 64 US](../games/duke-us/README.md) | WASD, mouse-look | Yes | Prior textured main-menu check. |
| [Duke Nukem: Zero Hour US](../games/duke-zero-hour-us/README.md) | WASD and arrows; mouse camera pending | Pending coupled projection/sector traversal | Debug strict ES2 startup/copyright; RT64 Release built. |
| [GoldenEye US](../games/goldeneye-us/README.md) | WASD, mouse-look, mouse menus | Yes | Prior Debug/Release Dam gameplay replay. |
| [Hexen US](../games/hexen-us/README.md) | WASD, mouse-look | Yes | New GLES2 Debug ran 2,600 offscreen presentations; publisher/copyright screens. Pinned L3DEX builds, but live automap not exercised. RT64 Release built. |
| [Majora's Mask US](../games/mm-us/README.md) | WASD, mouse orbit | Yes | Native GLES Debug rendered the title, file creation and opening story over 3,200 frames. Release ran 600 frames. Both JPEG boot variants matched the reference RSP on replay. |
| [Nightmare Creatures US](../games/nightmare-us/README.md) | WASD, horizontal mouse steering | Yes, new; gameplay check pending | Debug GLES2 reached title; RT64 Release built. Isolated FoV checks passed. |
| [Ocarina of Time US 1.2](../games/oot-us-v12/README.md) | WASD, mouse orbit | Yes | First-slot Temple of Time exterior background checked against decoded guest RAM. |
| [Perfect Dark US 1.1](../games/perfectdark-us-v11/README.md) | Opt-in WASD and mouse-look, native style 1.1 | Yes, normal single-player view; gameplay check pending | Earlier Carrington Institute evidence. New hooks pass ROM and focused checks; complete build with all new controls pending. |
| [Quake US](../games/quake-us/README.md) | WASD, mouse-look | Yes | Prior Debug/Release first-level gameplay replay. |
| [Quake II US](../games/quake2-us/README.md) | WASD, mouse-look | Yes, new; gameplay check pending | Earlier intro/menu, and Debug startup before the final FoV scope correction. Final changed build pending; isolated FoV checks passed. |
| [Rayman 2 US](../games/rayman2-us/README.md) | WASD, mouse camera | Yes | Prior rendered first scene with character/HUD. |
| [Resident Evil 2 US 1.1](../games/re2-us-v1/README.md) | WASD and arrows; fixed scene cameras | Not applied to fixed framing | Compiled F3DEX.Rej and S2DEX with RAM scanout for guest-decoded video. |
| [Shadow Man US](../games/shadowman-us/README.md) | WASD and arrows; mouse camera pending | Pending projection trace | Debug strict ES2 opening cinematic; RT64 Release built. |
| [Super Mario 64 US](../games/sm64-us/README.md) | WASD and arrows, stepped C-button mouse camera | Yes, new; gameplay check pending | Debug strict ES2 title / Press Start visuals; RT64 Release built. Isolated input/FoV checks passed. Browser layout unchanged. |
| [Turok US 1.2](../games/turok-us-v12/README.md) | WASD, mouse-look | Yes | Prior gameplay, automap and pause replay. |
| [Turok 2 US](../games/turok2-us/README.md) | WASD, mouse-look | Yes | New Debug GLES2 rendered title/menu and a paused Port of Adia view; longer replay timed out. |
| [Turok 3 US](../games/turok3-us/README.md) | WASD, mouse-look | Yes | New Debug GLES2 rendered title and opening cinematic; real gameplay input not verified. |
| [The World Is Not Enough US](../games/twine-us/README.md) | Pending mission startup | Pending mission startup | New Debug GLES2 reached title/main menu. Start Game hits unregistered overlay entry `0x801193ec`. |

These results were collected during the controls and GLES2 work. The subsequent
shared presentation-pacing change was exercised separately with a deterministic
scheduler test. Final whole-game rebuilds and loaded-system A/B gameplay checks
were not run while the Fedora desktop was in use.
