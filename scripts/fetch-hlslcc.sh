#!/usr/bin/env bash
# Fetches HLSLcc, the DXBC -> GLSL translator Unity itself uses for its GLES3 shaders (M4), at a
# pinned commit into build/hlslcc. MIT (bstring: BSD), compatible with UnityBridge's GPL-3.0-or-later;
# built as it is, unmodified, and never committed here (see third_party/hlslcc/PROVENANCE.md).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HLSLCC_URL="https://github.com/Unity-Technologies/HLSLcc.git"
HLSLCC_REV="3ea1fcd6bd0ac445bc078de0bf32f0950188577b"
SRC="$ROOT/build/hlslcc"

if [[ ! -f "$SRC/src/HLSLcc.cpp" ]]; then
    rm -rf "$SRC"
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$HLSLCC_URL"
    git -C "$SRC" fetch -q --depth 1 origin "$HLSLCC_REV"
    git -C "$SRC" checkout -q FETCH_HEAD
fi
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$HLSLCC_REV" ]] || { echo "HLSLcc checkout is not $HLSLCC_REV" >&2; exit 1; }
echo "HLSLcc $HLSLCC_REV: $(ls "$SRC"/src/*.cpp | wc -l) sources"
