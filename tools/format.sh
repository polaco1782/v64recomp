#!/usr/bin/env bash
# Format project C/C++ sources and headers with the repository's .clang-format.
# Usage: tools/format.sh (from any directory)
# Set CLANG_FORMAT to select a different clang-format executable.
set -euo pipefail

project_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
formatter="${CLANG_FORMAT:-clang-format}"

if ! command -v -- "$formatter" >/dev/null 2>&1; then
    printf 'Error: %s was not found. Install clang-format or set CLANG_FORMAT.\n' "$formatter" >&2
    exit 1
fi

cd -- "$project_root"

# Skip dependencies, generated builds, private data, and tool environments.
find . \
    -type d \( -name .git -o -name third_party -o -name build -o -name local -o -name .venv \) -prune -o \
    -type f \( \
        -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.hh' -o \
        -name '*.cpp' -o -name '*.hpp' -o -name '*.cxx' -o -name '*.hxx' -o \
        -name '*.C' -o -name '*.H' -o -name '*.inl' -o -name '*.ipp' -o -name '*.tpp' \
    \) -exec "$formatter" -i --style=file -- {} +
