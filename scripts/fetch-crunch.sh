#!/usr/bin/env bash
# Fetches Unity's fork of crunch (the "unity" branch, the format of DXT1Crunched/DXT5Crunched since
# Unity 2017.3) at a pinned commit into build/crunch (M8). ZLIB license, compatible with UnityBridge's
# GPL-3.0-or-later; only its header-only decoder inc/crn_decomp.h is used, unmodified, and nothing of it
# is committed here (see third_party/crunch/PROVENANCE.md).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CRUNCH_URL="https://github.com/Unity-Technologies/crunch.git"
CRUNCH_REV="8708900eca8ec609d279270e72936258f81ddfb7"
SRC="$ROOT/build/crunch"

if [[ ! -f "$SRC/inc/crn_decomp.h" ]]; then
    rm -rf "$SRC"
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$CRUNCH_URL"
    git -C "$SRC" fetch -q --depth 1 origin "$CRUNCH_REV"
    git -C "$SRC" checkout -q FETCH_HEAD
fi
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$CRUNCH_REV" ]] || { echo "crunch checkout is not $CRUNCH_REV" >&2; exit 1; }
echo "crunch $CRUNCH_REV"
