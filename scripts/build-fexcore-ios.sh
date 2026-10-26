#!/usr/bin/env bash
# Builds FEXCore (x86-64 -> ARM64 JIT) as static libraries for arm64 iOS.
#
# Source: the Apple-Silicon/iOS port of FEXCore that AetherPS4 keeps in
# runtime/sources/fexcore-darwin (upstream FEX f2b679f + Darwin/iOS patches, MIT). Only that
# directory is checked out, at a pinned commit.
#
# Output: build/fexcore-ios/out/lib/*.a and build/fexcore-ios/out/include (FEXCore's public
# headers plus the generated ones), for the app build to link against.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AETHER_URL="https://github.com/Leviidev/AetherPS4.git"
AETHER_REV="3347fc13f19860855ce91b66356940989ea6b6a6"
SRC="$ROOT/build/aetherps4"
FEX="$SRC/runtime/sources/fexcore-darwin"
BUILD="$ROOT/build/fexcore-ios/cmake"
OUT="$ROOT/build/fexcore-ios/out"
DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET:-17.0}"

if [[ ! -d "$FEX" ]]; then
    rm -rf "$SRC"
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$AETHER_URL"
    git -C "$SRC" config core.sparseCheckout true
    git -C "$SRC" sparse-checkout set --no-cone /runtime/sources/fexcore-darwin/
    git -C "$SRC" fetch -q --depth 1 --filter=blob:none origin "$AETHER_REV"
    git -C "$SRC" checkout -q FETCH_HEAD
fi
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$AETHER_REV" ]] || { echo "AetherPS4 checkout is not $AETHER_REV" >&2; exit 1; }

# CMAKE_SYSTEM_PROCESSOR must be set: FEX's architecture detection fails on an empty value when
# cross-compiling. BUILD_FEXCORE_ONLY leaves out FEX's Linux frontend (ELF loader, syscalls),
# which does not build for Darwin; UnityBridge supplies its own (Windows) frontend.
cmake -S "$FEX" -B "$BUILD" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_SYSTEM_PROCESSOR=arm64 \
    -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_FEXCORE_ONLY=ON \
    -DBUILD_FEXCORE_SMOKE=OFF \
    -DBUILD_TESTING=OFF \
    -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
    -DENABLE_FEX_ALLOCATOR=ON \
    -DENABLE_CCACHE=ON \
    -DENABLE_LTO=OFF \
    -DENABLE_WERROR=OFF \
    -DENABLE_STRICT_WERROR=OFF \
    -DTUNE_CPU=none \
    -DCMAKE_CXX_FLAGS=-DMETA_NO_STD_FORWARD_DECLARATIONS

# JemallocLibs is not a dependency of the static FEXCore target, but on Apple it carries the
# dual-mapped JIT allocator (FEXCore/Source/Utils/AllocatorHooks.cpp) the JIT calls into.
cmake --build "$BUILD" --target FEXCore Common CommonTools JemallocLibs

rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/include"
find "$BUILD" -name '*.a' -not -path '*/CMakeFiles/*' -exec cp -f {} "$OUT/lib/" \;
for lib in "$OUT"/lib/*.a; do
    lipo -info "$lib" | grep -q arm64 || { echo "$lib is not arm64" >&2; exit 1; }
done
cp -R "$FEX/FEXCore/include/." "$OUT/include/"
cp -R "$FEX/FEXHeaderUtils/FEXHeaderUtils" "$OUT/include/"
[[ -d "$BUILD/generated" ]] && cp -R "$BUILD/generated/." "$OUT/include/"
git -C "$SRC" rev-parse HEAD > "$OUT/SOURCE_REVISION"
ls -la "$OUT/lib"
