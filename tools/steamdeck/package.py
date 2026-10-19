#!/usr/bin/env python3
"""Bundle dependencies from the build userspace and enforce its glibc ceiling."""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tomllib


# Core ABI libraries also serve the host's dynamically loaded graphics drivers.
# Keep its C++ runtime and window-system libraries: older bundled copies can
# prevent a newer Mesa driver from loading, hiding all Vulkan surface extensions.
SYSTEM_LIBRARY = re.compile(
    r"^(?:ld-linux.*|lib(?:c|m|pthread|dl|rt|resolv|util|anl|stdc\+\+|gcc_s)\.so.*|"
    r"lib(?:GL.*|EGL.*|GLES.*|OpenGL|GLX.*|vulkan|drm.*|gbm|"
    r"wayland-.*|xcb.*|X.*|xkbcommon.*)\.so.*)$"
)
GLIBC_VERSION = re.compile(r"\bGLIBC_(\d+(?:\.\d+)+)\b")
CXX_VERSION = re.compile(r"\bGLIBCXX_(\d+(?:\.\d+)+)\b")
ROOT = Path(__file__).resolve().parents[2]


def output(*args):
    return subprocess.check_output(args, text=True, env={**os.environ, "LC_ALL": "C"})


def glibc_requirement(path):
    versions = GLIBC_VERSION.findall(output("readelf", "--version-info", str(path)))
    return max((tuple(map(int, version.split("."))) for version in versions), default=(0,))


def package(game, build):
    defaults = (build / "generated" / game / "game_defaults.h").read_text()
    paths = dict(re.findall(r'^#define RECOMP_DEFAULT_(ROM|SAVE) "([^"]+)"', defaults, re.M))
    rom = Path(paths["ROM"]).relative_to("/src/local")
    save = Path(paths["SAVE"]).relative_to("/src/local")
    rom_source = ROOT / "local" / rom
    identity = tomllib.loads((ROOT / "games" / game / "profile.toml").read_text())["game"]
    if rom_source.stat().st_size != identity["rom_size"]:
        raise SystemExit(f"ROM size mismatch: {rom_source}")
    with rom_source.open("rb") as source:
        digest = hashlib.file_digest(source, "sha1").hexdigest()
    if digest != identity["rom_sha1"]:
        raise SystemExit(f"ROM SHA-1 mismatch: {rom_source}")
    destination = build / "package"
    # Preserve an existing package, including any user-added ROMs or saves.
    if destination.exists():
        raise SystemExit(f"Move {destination} aside before packaging again; it may contain saves.")
    destination.mkdir(parents=True)
    libraries = destination / "lib"
    libraries.mkdir()
    executable = destination / game
    shutil.copy2(build / "bin" / game, executable)
    dependencies = output("ldd", str(executable))
    if "not found" in dependencies:
        raise SystemExit(f"Unresolved build dependencies:\n{dependencies}")
    manifest = []
    for line in dependencies.splitlines():
        match = re.match(r"\s*(\S+) => (/\S+) \(", line)
        if not match:
            continue
        name, source = match.groups()
        if SYSTEM_LIBRARY.fullmatch(name):
            manifest.append(f"system\t{name}\n")
            continue
        shutil.copy2(source, libraries / name)
        manifest.append(f"bundled\t{name}\t{source}\n")

    report = []
    for path in [executable, *sorted(libraries.iterdir())]:
        version = glibc_requirement(path)
        if version > (2, 36):
            raise SystemExit(f"{path.name} needs glibc {version}, newer than the 2.36 target")
        cxx = max((tuple(map(int, item.split("."))) for item in CXX_VERSION.findall(
            output("readelf", "--version-info", str(path)))), default=(0,))
        if cxx > (3, 4, 30):
            raise SystemExit(f"{path.name} needs GLIBCXX {cxx}, newer than the GCC 12 target")
        report.append(f"{path.name}\tGLIBC_{'.'.join(map(str, version))}"
                      f"\tGLIBCXX_{'.'.join(map(str, cxx))}\n")
    (destination / "DEPENDENCIES.txt").write_text("".join(sorted(manifest)))
    (destination / "GLIBC-REQUIREMENTS.txt").write_text("".join(report))
    (destination / "local").mkdir()
    bundled_rom = destination / "local" / rom
    bundled_rom.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(rom_source, bundled_rom)
    launcher = destination / "launch.sh"
    launcher.write_text(f'''#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "${{BASH_SOURCE[0]}}")" && pwd)
export LD_LIBRARY_PATH="$here/lib${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"
export SDL_VIDEODRIVER=x11
export RECOMP_TEXTURE_REPLACEMENTS="${{RECOMP_TEXTURE_REPLACEMENTS-$here/local/textures/{game}/replacements}}"
export RECOMP_TEXTURE_SCAN_RAW="${{RECOMP_TEXTURE_SCAN_RAW-$here/local/textures/{game}/scan}}"
cd -- "$here"
exec "$here/{game}" --rom "$here/local/{rom}" \\
    --data "$here/local/rt64/{game}" --save "$here/local/{save}" \\
    --crash-dir "$here/local/crashes/{game}" "$@"
''')
    launcher.chmod(0o755)
    shutil.copy2(ROOT / "LICENSE", destination)
    shutil.copy2(ROOT / "THIRD_PARTY_NOTICES.md", destination)
    shutil.copytree(ROOT / "licenses", destination / "licenses")
    # Library copyright files ship with Debian's packages.
    for notice in sorted(Path("/usr/share/doc").glob("*/copyright")):
        target = destination / "library-notices" / notice.parent.name
        target.mkdir(parents=True, exist_ok=True)
        shutil.copy2(notice, target / "copyright")
    (destination / "README.txt").write_text(
        f"ROM included at local/{rom}. Run ./launch.sh.\n"
        "Keep the entire directory together. local/ holds private saves and caches.\n"
        "Requires x86_64 Linux with glibc 2.36+, GCC 12+ runtime (GLIBCXX_3.4.30),\n"
        "X11/Xwayland and a Vulkan driver.\n"
        "For Steam Deck, add launch.sh as a non-Steam game with compatibility tools disabled.\n"
        "Map Steam Input to keyboard keys: arrows = stick, A/X/Z = N64 A/B/Z,\n"
        "S = Start, C/V = L/R, I/J/K/L = C up/left/down/right, Escape = exit.\n"
        "The GLIBC-REQUIREMENTS.txt file records every bundled ELF's requirement.\n"
        "Existing saves are excluded. Hardware gameplay is not validated by packaging.\n"
    )
    # Verify loader resolution and startup without initializing Vulkan or a ROM.
    subprocess.run([str(launcher), "--help"], check=True)
    archive = build / f"{game}-steamdeck.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        tar.add(destination, arcname=f"{game}-steamdeck")
    print(f"Steam Deck package: {archive}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", required=True)
    parser.add_argument("--build", type=Path, required=True)
    options = parser.parse_args()
    package(options.game, options.build.resolve())
