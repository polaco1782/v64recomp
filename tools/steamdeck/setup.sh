#!/usr/bin/env bash
# Shared Debian dependencies for every Steam Deck build engine.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    ca-certificates clang llvm llvm-14-linker-tools cmake ninja-build pkg-config \
    python3 python3-dev python3-venv libssl-dev libsdl2-dev libgles2-mesa-dev \
    libx11-dev libxrandr-dev
rm -rf /var/lib/apt/lists/*
