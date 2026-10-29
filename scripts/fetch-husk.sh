#!/usr/bin/env bash
# Fetches Husk's Android runtime (its "translation layer": ELF loader, bionic, JNI, Unity driver)
# at a pinned commit into build/husk. Husk is GPL-2.0-or-later, compatible with UnityBridge's
# GPL-3.0-or-later; it is compiled into the probe app as it is, unmodified, and never committed
# here (see third_party/husk/PROVENANCE.md). Only src/translation-layer{,-next} and the license.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HUSK_URL="https://github.com/Leviidev/Husk.git"
HUSK_REV="4ac655293f970e706af65dea013799ee11dfcd9d"
SRC="$ROOT/build/husk"

if [[ ! -d "$SRC/src/translation-layer-next" ]]; then
    rm -rf "$SRC"
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$HUSK_URL"
    git -C "$SRC" config core.sparseCheckout true
    git -C "$SRC" sparse-checkout set --no-cone /LICENSE /src/translation-layer/ /src/translation-layer-next/
    git -C "$SRC" fetch -q --depth 1 --filter=blob:none origin "$HUSK_REV"
    git -C "$SRC" checkout -q FETCH_HEAD
fi
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$HUSK_REV" ]] || { echo "Husk checkout is not $HUSK_REV" >&2; exit 1; }
echo "Husk $HUSK_REV: $(ls "$SRC"/src/translation-layer*/*.[cm] | wc -l) sources"
