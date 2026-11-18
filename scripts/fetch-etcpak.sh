#!/usr/bin/env bash
# Fetches etcpak, the ETC1/ETC2 texture encoder (M8), at a pinned commit into build/etcpak. BSD-3-Clause,
# compatible with UnityBridge's GPL-3.0-or-later; built as it is, unmodified, and never committed here
# (see third_party/etcpak/PROVENANCE.md). The device transcodes desktop BC textures to ETC2 with it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ETCPAK_URL="https://github.com/wolfpld/etcpak.git"
ETCPAK_REV="3d716e1550023dc5ff8b602636af8b76584e0b66"
SRC="$ROOT/build/etcpak"

if [[ ! -f "$SRC/ProcessRGB.cpp" ]]; then
    rm -rf "$SRC"
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$ETCPAK_URL"
    git -C "$SRC" fetch -q --depth 1 origin "$ETCPAK_REV"
    git -C "$SRC" checkout -q FETCH_HEAD
fi
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$ETCPAK_REV" ]] || { echo "etcpak checkout is not $ETCPAK_REV" >&2; exit 1; }
echo "etcpak $ETCPAK_REV"
