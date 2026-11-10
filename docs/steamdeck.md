# Building for Steam Deck

Build against an older glibc in a container on your x86_64 Linux PC, then copy
the resulting package to the Deck. Compiling on a newer distribution can import
glibc and libstdc++ symbol versions that SteamOS does not provide.

This build uses **Debian 12 (Bookworm), glibc 2.36**, and targets a native SteamOS
desktop/Game Mode launch with glibc **2.36 or newer** and a GCC 12 or newer
C++ runtime (`GLIBCXX_3.4.30`). On the Deck, check glibc with:

```sh
getconf GNU_LIBC_VERSION
```

This is a userspace compatibility target; Deck hardware gameplay and performance
still need testing. It does not change SteamOS or replace its system libraries.

## Build on your PC

Use rootless Podman, Docker or Bubblewrap. Bubblewrap may already be installed
as a dependency of desktop software. To install its host dependencies on Fedora:

```sh
sudo dnf install bubblewrap util-linux shadow-utils curl tar coreutils
```

Import your ROM as described in the root
[README](../README.md). The GoldenEye profile requires the US ROM, normalized as
`local/goldeneye.us.z64`. Then run:

```sh
./tools/build_steamdeck.sh goldeneye-us 4
```


```sh
CONTAINER_ENGINE=bwrap ./tools/build_steamdeck.sh goldeneye-us 4
```

Initial setup needs network access for Debian packages; generation dependencies
are installed from the repository's pinned `requirements.txt` inside Debian.
All three engines use the same dependency setup script and compilation steps.


The results are:

```text
build/steamdeck/goldeneye-us/package/
build/steamdeck/goldeneye-us/goldeneye-us-steamdeck.tar.gz
```

The package bundles the executable's shared dependencies from Debian, including
OpenSSL and SDL2. glibc, libstdc++, libgcc, the graphics driver stack and the
X11/XCB/Wayland libraries come from the Deck. This lets a newer graphics driver
load its matching C++ runtime and window-system libraries.
`GLIBC-REQUIREMENTS.txt` records the highest required glibc and GLIBCXX symbol
versions for each bundled ELF; packaging rejects versions newer than glibc 2.36
or GLIBCXX 3.4.30.
`DEPENDENCIES.txt` records bundled and system dependencies. Packaging also
checks library resolution and runs the launcher's `--help` inside Debian.
This verifies loader/startup compatibility, without exercising graphics or gameplay.

By default the package excludes ROMs, existing saves, texture packs and generated
CPU sources. Supply your own matching ROM separately. If the build explicitly
enables ROM embedding, packaging preserves it and the launcher uses the embedded
content. These private packages
contain the supplied assets; existing saves are still excluded.
Keep the build tree for crash investigation. To package again, move the previous
`package/` directory aside first: the tool refuses to overwrite a directory that
may contain your saves.

## Copy and launch

Copy the archive to the Deck using your preferred transfer method. In Desktop
Mode, extract it to a writable folder, for example:

```sh
mkdir -p ~/Games/v64recomp
tar -xzf goldeneye-us-steamdeck.tar.gz -C ~/Games/v64recomp
```

For built-in CPU profiling, from the extracted directory run:

```sh
./launch.sh --profile deck-profile --profile-delay 20 --profile-seconds 30
```

Play the scene during the capture window. The game writes `deck-profile/` after
30 seconds of recording and keeps running. Read `hotspots.txt` for CPU totals
and hot functions, `thread-hotspots.txt` for the functions within each thread,
and `stacks.folded` for caller paths. Each capture needs a new directory name.
No Python, `perf` installation, or sudo is required. See [CPU profiling](profiling.md)
for sampling limits and report formats.

Copy your own imported, normalized ROM to the extracted package before launch:

```text
~/Games/v64recomp/goldeneye-us-steamdeck/local/oot.us.v12.z64
```

An existing OOT save can be copied to `local/oot.us.v12.z64.sram`. Launch with:

```sh
~/Games/v64recomp/goldeneye-us-steamdeck/launch.sh
```


The shared SDL gamepad backend reads the left stick as **analog input**, with a
12% radial deadzone and continuous travel from gentle movement to full speed.
Keep Steam Input enabled and select its built-in **Gamepad** template for the
basic controls. Mapping the stick to arrow keys removes analog movement.

The package also includes a selectable **v64recomp - N64** Steam Input template.
From the extracted package in Desktop Mode run:

```sh
./install-controls.sh
```

Restart Steam, open the non-Steam game's **Controller settings → current layout
→ Templates**, select **v64recomp - N64**, then **Apply Layout**. The installer
detects native/Flatpak Steam and copies only our VDF into its templates directory.
Use `./install-controls.sh --steam-dir PATH` for a different Steam installation.
It saves a backup when replacing an edited copy of our template; it does not
modify other layouts. No sudo or Python is needed on the Deck. Steam updates
may remove custom templates; rerun the installer if the layout disappears.
Launch the game's shortcut through Steam in Desktop Mode or Game Mode so Steam
activates that layout for the Deck controls.

| Deck control | N64 control |
| --- | --- |
| Left stick | Analog stick |
| D-pad | C-buttons in OOT; N64 D-pad in other games |
| A / X | A / B |
| B / Y | C-down / C-left |
| Left trigger | Z (OOT targeting) |
| Right trigger / bumper | R (OOT shield) |
| Left bumper | L |
| Menu | Start |
| Right stick | Third-person camera in OOT; C-buttons in other games |
| Right trackpad directions | C-buttons |
| Right stick click | C-up |
| L4 / R4 | C-up / C-down |
| L5 / R5 | C-left / C-right |
| Hold View for 1.5 seconds | Exit normally |

The custom template sets Steam's stick inner deadzone to zero so the app applies
the deadzone once. C-buttons and triggers use separate press/release thresholds
to avoid flicker near their activation point. Controllers can be connected or
removed while running; keyboard controls remain available, and losing window
focus releases game input. The first recognized gamepad controls port 1.
See the shipped `steam-input/README.md` for layout details and customization.

The OOT package defaults `RECOMP_OOT_GAMEPAD_CAMERA=1` and `RECOMP_OOT_MOUSE=0`.
The left stick moves Link and navigates menus, the right stick orbits the
standard third-person follow camera, and the top-left D-pad supplies all four
C-buttons, including ocarina notes. The right stick has a 12% radial deadzone
and proportional camera speed; it operates independently of mouse capture.
Z-targeting, dialog, pause and special camera modes retain native behavior.
Set `RECOMP_OOT_GAMEPAD_CAMERA=0` before launching to restore the original
gamepad mapping. These settings take effect when the game starts.

The VDF format is checked against Steam's installed Deck Gamepad template;
installation follows [EmuDeck's template workflow](https://emudeck.github.io/community-creations/steamos/community-creations/#how-to-download-steam-input-profiles).
The Steam client must still load and apply the layout on the Deck; local tests
of SDL virtual controllers do not establish physical Deck gameplay acceptance.

The first run can spend time compiling GLES2 shaders. Check actual play,
audio, controls, save persistence and frame pacing on the Deck before treating
the build as accepted.

## Continuous audio gaps

To capture 30 seconds of Release audio timing, run this from the extracted
package directory and let the window close automatically:

```sh
RECOMP_AUDIO_TRACE="$PWD/audio.csv" ./launch.sh --frames 1800 > audio.log 2>&1
```

`audio.log` records the SDL backend, sample rate, device period and final
underrun/reset counts. `audio.csv` records PCM admission times and queue depth,
so sample-production timing can be compared with sound-device consumption.
For a backend comparison, prefix the command with `SDL_AUDIODRIVER=pulseaudio`.
These environment variables affect only that run.
