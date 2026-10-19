#!/usr/bin/env bash
# Build in an older userspace; never reuse the host's CMake or RT64 cache.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/build_steamdeck.sh [GAME] [JOBS]

Defaults: oot-us-v12, 4 jobs. Supports Podman, Docker and Bubblewrap.
Outputs: build/steamdeck/GAME/package and GAME-steamdeck.tar.gz
Set CONTAINER_ENGINE=podman, docker or bwrap to select the engine.
Automatic selection tries Podman, Docker, then Bubblewrap.
The package includes the selected ROM and excludes existing saves; see docs/steamdeck.md.
EOF
}
if [[ ${1:-} == --help || ${1:-} == -h ]]; then usage; exit 0; fi
if (( $# > 2 )); then usage >&2; exit 2; fi
game=${1:-oot-us-v12}
jobs=${2:-4}
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ! $game =~ ^[a-z0-9]+(-[a-z0-9]+)*$ || ! -f $root/games/$game/profile.toml ]]; then
    printf 'Unknown game: %s\n' "$game" >&2; exit 2
fi
if [[ ! $jobs =~ ^[1-9][0-9]*$ ]]; then
    printf 'JOBS must be a positive integer\n' >&2; exit 2
fi
if [[ $(uname -m) != x86_64 ]]; then
    printf 'Run this build on an x86_64 Linux machine.\n' >&2; exit 2
fi
engine=${CONTAINER_ENGINE:-}
if [[ -z $engine ]]; then
    if command -v podman >/dev/null; then engine=podman
    elif command -v docker >/dev/null; then engine=docker
    elif command -v bwrap >/dev/null; then engine=bwrap
    else
        printf 'Install Podman, Docker or Bubblewrap first. On Fedora: sudo dnf install bubblewrap\n' >&2
        exit 1
    fi
fi
if [[ $engine != podman && $engine != docker && $engine != bwrap ]]; then
    printf 'CONTAINER_ENGINE must be podman, docker or bwrap\n' >&2; exit 2
fi
command -v "$engine" >/dev/null
mkdir -p "$root/build/steamdeck" "$root/build/shared/rt64/steamdeck"
printf 'Steam Deck build engine: %s\n' "$engine"
if [[ $engine == bwrap ]]; then
    exec bash "$root/tools/steamdeck/bwrap.sh" "$root" "$game" "$jobs"
fi
image=localhost/v64recomp-steamdeck:bookworm
# Only build recipes are sent to the image builder, never the checkout or ROMs.
"$engine" build --platform linux/amd64 -t "$image" "$root/tools/steamdeck"
user_options=(--user "$(id -u):$(id -g)")
if [[ $engine == podman ]]; then user_options+=(--userns keep-id); fi
# Disable SELinux label separation for these explicit mounts, avoiding any
# relabel of the user's checkout. Container writes are restricted to the caches.
"$engine" run --rm --platform linux/amd64 "${user_options[@]}" \
    --security-opt label=disable --env HOME=/tmp \
    --volume "$root:/src:ro" \
    --volume "$root/build/steamdeck:/src/build/steamdeck:rw" \
    --volume "$root/build/shared/rt64/steamdeck:/src/build/shared/rt64/steamdeck:rw" \
    --workdir /src "$image" bash tools/steamdeck/build.sh "$game" "$jobs"
