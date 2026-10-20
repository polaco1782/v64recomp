#!/usr/bin/env bash
# Install only our named Steam Input template. Never edit another layout.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
steam_root=
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    printf 'Usage: %s [--steam-dir STEAM_INSTALL_DIRECTORY]\n' "$0"
    printf 'Installs v64recomp - N64 into Steam Input Templates. No sudo required.\n'
    exit 0
fi
if (( $# )); then
    if [[ $# != 2 || $1 != --steam-dir ]]; then
        printf 'Usage: %s [--steam-dir STEAM_INSTALL_DIRECTORY]\n' "$0" >&2
        exit 2
    fi
    steam_root=$2
else
    for candidate in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" \
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        if [[ -d $candidate/controller_base ]]; then steam_root=$candidate; break; fi
    done
fi
if [[ -z $steam_root || ! -d $steam_root/controller_base ]]; then
    printf 'Steam installation not found. Use --steam-dir with the directory containing controller_base.\n' >&2
    exit 1
fi
source_file="$here/steam-input/v64recomp_n64.vdf"
if [[ ! -f $source_file ]]; then
    printf 'Template is missing: %s\n' "$source_file" >&2
    exit 1
fi
directory="$steam_root/controller_base/templates"
mkdir -p -- "$directory"
target="$directory/v64recomp_n64.vdf"
if [[ ! -f $target ]] || ! cmp -s -- "$source_file" "$target"; then
    if [[ -e $target ]]; then
        backup=$(mktemp "$target.backup.XXXXXX")
        cp -p -- "$target" "$backup"
        printf 'Previous template saved as %s\n' "$backup"
    fi
    temporary=$(mktemp "$directory/.v64recomp_n64.XXXXXX")
    trap 'rm -f -- "$temporary"' EXIT
    install -m 644 -- "$source_file" "$temporary"
    mv -f -- "$temporary" "$target"
    trap - EXIT
fi
printf 'Installed: %s\n' "$target"
printf 'Restart Steam, open the game controller layout, then Templates.\n'
printf 'Select v64recomp - N64 and Apply Layout. The Gamepad template also works.\n'
