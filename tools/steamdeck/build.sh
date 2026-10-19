#!/usr/bin/env bash
# Runs inside the Debian 12 environment, with /src mounted from the checkout.
set -euo pipefail
game=$1
jobs=$2
build=/src/build/steamdeck/$game
mkdir -p "$build"
# A private venv avoids the host .venv (which can require a newer glibc).
if [[ ! -x /src/build/steamdeck/python/bin/python ]]; then
    python3 -m venv /src/build/steamdeck/python
fi
python=/src/build/steamdeck/python/bin/python
"$python" -m pip install --disable-pip-version-check -r /src/requirements.txt
cmake -S /src -B "$build" -G Ninja \
    -DRECOMP_GAME="$game" -DCMAKE_BUILD_TYPE=Release -DRECOMP_OPT_LEVEL=3 \
    -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DPython3_EXECUTABLE="$python" \
    -DCMAKE_C_FLAGS='-march=x86-64 -mtune=generic' \
    -DCMAKE_CXX_FLAGS='-march=x86-64 -mtune=generic' \
    -DRECOMP_RT64_BUILD_ROOT=/src/build/shared/rt64/steamdeck \
    -DRECOMP_RT64_BUILD_TYPE=Release -DRECOMP_RT64_JOBS="$jobs"
cmake --build "$build" --parallel "$jobs"
"$python" /src/tools/steamdeck/package.py --game "$game" --build "$build"
