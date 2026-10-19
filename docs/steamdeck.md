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

Populate the submodules and import your ROM as described in the root
[README](../README.md). For Ocarina of Time, the profile requires US V1.2 and the
normalized file `local/oot.us.v12.z64`. Then run:

```sh
./tools/build_steamdeck.sh oot-us-v12 4
```

The first argument is any supported game ID; the second is the number of build
jobs (default 4, also applied to RT64). The default game is `oot-us-v12`.
The script automatically selects Podman, Docker, then Bubblewrap, in that order.
Set `CONTAINER_ENGINE=podman`, `docker` or `bwrap` to choose explicitly:

```sh
CONTAINER_ENGINE=bwrap ./tools/build_steamdeck.sh oot-us-v12 4
```

Initial setup needs network access for Debian packages; generation dependencies
are installed from the repository's pinned `requirements.txt` inside Debian.
All three engines use the same dependency setup script and compilation steps.

Bubblewrap needs Linux user namespaces, `unshare --map-auto`, and working
`newuidmap`/`newgidmap` helpers. Your account must have subordinate UID and GID
ranges in `/etc/subuid` and `/etc/subgid` (normally configured when the account
is created). The script runs without sudo. It downloads an official Debian image
layer pinned to a Git revision and SHA-256 digest, verifies it before extraction,
then installs dependencies inside that filesystem. It caches the result under
`build/steamdeck/bwrap/`; subsequent runs reuse it. A lock serializes Bubblewrap
setup/builds, and a change to the shared dependency setup script reruns setup.
Only the Debian filesystem is writable during package installation. During
compilation it is mounted read-only, along with the checkout; the game and RT64
build directories and private temporary files are writable. Host package
installations and the host environment are excluded from the build.

The script builds the runtime, game and RT64 inside Debian 12. It explicitly
selects the container's Python interpreter, avoiding the host `.venv`, and uses
generic x86_64 compiler flags. It keeps game artifacts in
`build/steamdeck/<game>/` and RT64 in `build/shared/rt64/steamdeck/`, separate from
host builds. Both remain reusable between builds. All engines mount the
checkout read-only and write build output only to these two directories.
Podman/Docker image builds receive only the Dockerfile and dependency setup
script; ROMs never become image layers.

The results are:

```text
build/steamdeck/oot-us-v12/package/
build/steamdeck/oot-us-v12/oot-us-v12-steamdeck.tar.gz
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
This verifies loader/startup compatibility, without exercising Vulkan or gameplay.

The package includes the selected game's imported ROM under `local/`, after
checking its size and SHA-1 against the game profile. It excludes existing saves,
texture packs and generated CPU sources.
Keep the build tree for crash investigation. To package again, move the previous
`package/` directory aside first: the tool refuses to overwrite a directory that
may contain your saves.

## Copy and launch

Copy the archive to the Deck using your preferred transfer method. In Desktop
Mode, extract it to a writable folder, for example:

```sh
mkdir -p ~/Games/v64recomp
tar -xzf oot-us-v12-steamdeck.tar.gz -C ~/Games/v64recomp
```

The selected ROM is already included in the extracted package:

```text
~/Games/v64recomp/oot-us-v12-steamdeck/local/oot.us.v12.z64
```

An existing OOT save can be copied to `local/oot.us.v12.z64.sram`. Launch with:

```sh
~/Games/v64recomp/oot-us-v12-steamdeck/launch.sh
```

Keep the whole package together, including `lib/`. The launcher uses its own
directory for ROMs, saves, RT64 caches, textures and crash dumps regardless of
the current working directory. You can append the normal game arguments, such
as `--mute`, `--frames 300` or a different `--rom`/`--save` path.

Add `launch.sh` as a **non-Steam game** in Desktop Mode to launch it from Game
Mode. Leave forced compatibility tools disabled for this native build; Proton
is for Windows executables. This package does not target the older Sniper runtime.
The launcher selects X11/Xwayland, as used by the current RT64 adapter.

The current input backend reads keyboard keys. Configure Steam Input to send:

| Deck control | Keyboard key | N64 control |
| --- | --- | --- |
| Left stick / D-pad | Arrow keys | Stick |
| A | A | A |
| X | X | B |
| Right trigger | Z | Z |
| Menu | S | Start |
| Left / right bumper | C / V | L / R |
| Right stick directions | I / J / K / L | C up / left / down / right |
| A spare back button | Escape | Exit |

The first run can spend time compiling Vulkan pipelines. Check actual play,
audio, controls, save persistence and frame pacing on the Deck before treating
the build as accepted.

## Missing Vulkan surface extensions

If RT64 reports both `VK_KHR_surface` and `VK_KHR_xlib_surface` missing, inspect
Vulkan driver loading before changing renderer features:

```sh
VK_LOADER_DEBUG=error,warn ./launch.sh 2>&1 | tee vulkan-loader.log
```

An earlier package bundled Debian's X11/XCB/Wayland libraries. Those can shadow
the Deck's libraries and prevent its newer Mesa driver from loading. This was
reproduced with the AMD driver failing on the missing symbol
`wl_fixes_interface`; Vulkan then reported neither surface extension. The
packager now leaves these window-system libraries to the target OS.

Extract updated packages into a **fresh directory**. Extracting over an older
package leaves the incompatible libraries in `lib/`, because the updated
archive no longer contains those files. The ROM is included; copy any existing
saves from the old package's `local/` into the new package's `local/`.

## Why this baseline

[Valve's runtime documentation](https://github.com/ValveSoftware/steam-runtime)
recommends building in an isolated userspace to avoid development-machine library
dependencies. An official Steam Runtime SDK is useful when distributing for a
specific Steam runtime. This repository's bundled DXC shader compiler requires
glibc 2.34, while Sniper is based on glibc 2.31, so it cannot run there unchanged.
Debian 12 provides glibc 2.36, Python 3.11 and OpenSSL 3 together
([Debian release notes](https://www.debian.org/releases/bookworm/amd64/release-notes/ch-whats-new.en.html)).
The packaging audit enforces that baseline across the executable and its bundled
dependencies. A Deck with older glibc needs a different build baseline or a
matching Steam runtime; copying the host's libc into SteamOS is unnecessary.
