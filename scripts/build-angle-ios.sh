#!/usr/bin/env bash
# Builds ANGLE (EGL + GLES over Metal) for arm64 iOS as build/angle/libANGLE-shared.dylib: the EGL and
# GLES the Unity engine gets under Husk's runtime (M3f). iOS has no EGL of its own.
#
# The way Husk does it (its scripts/build_angle_ios.sh, GPL-2.0-or-later): ANGLE from WebKit's tree,
# which carries an Xcode project for it, so no depot_tools or gn; a blob-filtered sparse checkout of
# WebKit at a pinned commit. WebKit renames ANGLE's EGL entry points (EGL_ChooseConfig, ...), so a
# second link adds the standard egl* names as aliases (ld -alias_list); GL entry points are reached
# through eglGetProcAddress and need no aliases. Nothing from it enters the repository.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WEBKIT_URL="https://github.com/WebKit/WebKit.git"
WEBKIT_REV="7e68a669564b84a588003862554dc721c5f7aae2"
WK="$ROOT/build/webkit"
OUT="$ROOT/build/angle"
LOG="$ROOT/logs/angle-build.log"
DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET:-17.0}"
mkdir -p "$OUT" "$(dirname "$LOG")"

if [[ ! -d "$WK/Source/ThirdParty/ANGLE" ]]; then
    rm -rf "$WK"
    git init -q "$WK"
    git -C "$WK" remote add origin "$WEBKIT_URL"
    git -C "$WK" config core.sparseCheckout true
    # Configurations and Tools/ccache are required: ANGLE's xcconfigs include
    # ../../../../Tools/ccache/ccache.xcconfig.
    git -C "$WK" sparse-checkout set --no-cone /Source/ThirdParty/ANGLE/ /Configurations/ /Tools/Scripts/ /Tools/ccache/
    git -C "$WK" fetch -q --depth 1 --filter=blob:none origin "$WEBKIT_REV"
    git -C "$WK" checkout -q FETCH_HEAD
fi
[[ "$(git -C "$WK" rev-parse HEAD)" == "$WEBKIT_REV" ]] || { echo "WebKit checkout is not $WEBKIT_REV" >&2; exit 1; }

cd "$WK/Source/ThirdParty/ANGLE"
ALIASES="$OUT/angle-egl-aliases.txt"

# WK_AVAILABILITY_OVERLAY_FLAGS points at a clang VFS overlay that the rest of WebKit generates; this
# subset does not, so it is cleared, and the availability warnings it would have hidden (BC texture
# enums newer than the target, used only behind @available) are turned off rather than raising the
# target. The alias flag must reach xcodebuild as one argument, hence the array.
angle_build() {
    local ldflags=()
    if [[ -n "${1:-}" ]]; then ldflags=("OTHER_LDFLAGS=\$(inherited) -Wl,-alias_list,$1"); fi
    env -i PATH="$PATH" HOME="$HOME" DEVELOPER_DIR="${DEVELOPER_DIR:-}" xcodebuild archive \
        -archivePath "ANGLE" -scheme "ANGLE" \
        -sdk iphoneos -arch arm64 -configuration Release \
        WEBCORE_LIBRARY_DIR="/usr/local/lib" NORMAL_UMBRELLA_FRAMEWORKS_DIR="" \
        CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO \
        WK_AVAILABILITY_OVERLAY_FLAGS="" WK_AVAILABILITY_OVERLAY_SWIFT_FLAGS="" \
        IPHONEOS_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
        OTHER_CFLAGS='$(inherited) -Wno-unguarded-availability-new' \
        OTHER_CPLUSPLUSFLAGS='$(inherited) -Wno-unguarded-availability-new' \
        ${ldflags[@]+"${ldflags[@]}"} \
        >> "$LOG" 2>&1 \
      || { echo "ANGLE build failed; last errors:" >&2
           grep -a "error:\|^ld: " "$LOG" | head -20 >&2; tail -n 30 "$LOG" >&2; exit 1; }
}

DYLIB="ANGLE.xcarchive/Products/usr/local/lib/libANGLE-shared.dylib"
echo "==> ANGLE for iOS arm64, pass 1 (log: $LOG)"
angle_build ""
[[ -f "$DYLIB" ]] || { echo "no dylib at $DYLIB" >&2; exit 1; }

echo "==> EGL aliases"
nm -g "$DYLIB" | awk '$2=="T"{print $3}' | grep -E '^_EGL_' \
  | awk '{o=$1; a=$1; sub(/^_EGL_/,"_egl",a); print o, a}' > "$ALIASES"
echo "    $(wc -l < "$ALIASES") aliases"

echo "==> pass 2, relinking with the standard EGL names"
rm -rf ANGLE.xcarchive
angle_build "$ALIASES"
[[ -f "$DYLIB" ]] || { echo "no dylib at $DYLIB after pass 2" >&2; exit 1; }

cp "$DYLIB" "$OUT/libANGLE-shared.dylib"
# WebKit builds it for loading out of WebCore.framework; it is embedded in the app instead.
install_name_tool -id "@rpath/libANGLE-shared.dylib" "$OUT/libANGLE-shared.dylib"
echo "==> $(ls -lh "$OUT/libANGLE-shared.dylib" | awk '{print $5}') at $OUT, $(nm -g "$OUT/libANGLE-shared.dylib" | grep -c ' T _egl') standard egl* exports"
echo "$WEBKIT_REV" > "$OUT/WEBKIT_REV"
