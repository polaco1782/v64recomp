# v64recomp Steam Deck layout

From the extracted game package, run `./install-controls.sh` in Desktop Mode,
then restart Steam. For the non-Steam game, open **Controller settings → current
layout → Templates → v64recomp - N64 → Apply Layout**. Leave Steam Input enabled.
The template works with every game using the shared native gamepad backend.
Launch the game's shortcut through Steam in either Desktop Mode or Game Mode
to activate its layout on the Deck.

The installer adds only `v64recomp_n64.vdf` to Steam's `controller_base/templates`.
It preserves a backup when updating an existing copy. It detects native Steam
and Flatpak Steam; use `--steam-dir PATH` to select another installation.
To install manually, copy the VDF into that templates folder and restart Steam.
Steam updates may remove custom templates; rerun the installer if necessary.

| Deck control | N64 action |
| --- | --- |
| Left stick | Analog stick, including gentle walking and full running |
| D-pad | C-buttons in OOT/GoldenEye gameplay; N64 D-pad in other games |
| A / X | A / B |
| B / Y | C-down / C-left |
| Left trigger | Z (OOT targeting) |
| Right trigger / right bumper | R (OOT shield) |
| Left bumper | L |
| Menu | Start |
| Right stick | OOT camera; GoldenEye look/aim; C-buttons in other games |
| Right stick click | C-up |
| Right trackpad | C-buttons in each direction |
| L4 / R4 | C-up / C-down |
| L5 / R5 | C-left / C-right |
| Hold View for 1.5 seconds | Exit normally and save reports |

The sticks output gamepad axes, preserving analog travel. The template leaves
Steam's stick inner deadzone at zero; the app applies a 12% radial deadzone and
rescales the remaining range. Avoid changing the left stick to directional
keyboard keys, which removes analog movement. Adjust the layout in Steam Input
and export it as a personal template to keep your own version.

The OOT launcher enables `RECOMP_OOT_GAMEPAD_CAMERA=1`: the left stick moves
Link and navigates menus, the right stick orbits the standard follow camera,
and the top-left D-pad supplies C-up/down/left/right, including ocarina notes.
Camera motion needs no mouse capture. Dialog, pause, cutscenes and Z-targeting
retain the game's native camera behavior. Set `RECOMP_OOT_GAMEPAD_CAMERA=0`
before launching to restore the original gamepad mapping.

The GoldenEye launcher enables `RECOMP_GOLDENEYE_GAMEPAD=1` and disables mouse
capture: left stick moves/strafes, right stick looks, LT fires and RT/RB aims.
A changes weapons, X uses/reloads, and Menu opens the watch. Menus retain native
navigation. The native 2.2 input adapter restores the saved style after each
gameplay call. Set `RECOMP_GOLDENEYE_GAMEPAD=0` for the legacy controller path.

For a quick start without installation, choose Steam's built-in **Gamepad**
template. It supplies the same sticks, face buttons, D-pad, triggers, bumpers and
Start. The custom template adds trackpad/back-button shortcuts and the exit hold.

The VDF structure follows the installed Steam Deck Gamepad template. Template
installation follows [EmuDeck's Steam Input profile workflow](https://emudeck.github.io/community-creations/steamos/community-creations/#how-to-download-steam-input-profiles).
Steam still needs to load and apply the layout on the Deck; generating the file
does not verify the current Steam client UI or hardware behavior.
