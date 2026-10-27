#!/usr/bin/env bash
# Builds FEXCore (x86-64 -> ARM64 JIT) and UnityBridge's engine library for arm64 iOS.
#
# Source of FEXCore: the Apple-Silicon/iOS port AetherPS4 keeps in runtime/sources/fexcore-darwin
# (upstream FEX f2b679f + Darwin/iOS patches, MIT), checked out sparsely at a pinned commit.
#
# UnityBridge's engine sources (third_party/aetherps4 + engine/) need FEXCore's internal headers
# and compile flags, so they are built inside FEX's CMake project as one more static library,
# the way AetherPS4 builds its guest harness. The CMake hook is inserted below.
#
# Output in build/fexcore-ios/out: lib/*.a (FEXCore, its helpers and libunitybridge_engine.a).
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

# The hook: one static library from UNITYBRIDGE_ENGINE_SOURCES, placed before the
# BUILD_FEXCORE_ONLY block returns. Idempotent.
python3 - "$FEX/CMakeLists.txt" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
marker = "# UnityBridge engine hook"
if marker not in text:
    anchor = "  add_subdirectory(Source/Tools/CommonTools/ EXCLUDE_FROM_ALL)\n"
    assert text.count(anchor) == 1, "FEX CMakeLists.txt changed; update the UnityBridge hook"
    hook = anchor + """  %s
  if (UNITYBRIDGE_ENGINE_SOURCES)
    add_library(unitybridge_engine STATIC ${UNITYBRIDGE_ENGINE_SOURCES})
    target_compile_features(unitybridge_engine PRIVATE cxx_std_20)
    target_include_directories(unitybridge_engine PRIVATE
      ${UNITYBRIDGE_INCLUDE_DIRS}
      "${CMAKE_BINARY_DIR}/generated"
      "${CMAKE_SOURCE_DIR}/FEXCore/Source"
      "${CMAKE_SOURCE_DIR}/Source"
      "${CMAKE_SOURCE_DIR}/Source/Tools/CommonTools")
    target_link_libraries(unitybridge_engine PRIVATE FEXCore Common CommonTools JemallocLibs)
  endif()
""" % marker
    open(path, "w").write(text.replace(anchor, hook))
PY

AP="$ROOT/third_party/aetherps4/src"
ENGINE_SOURCES="$AP/core/fex/fex_guest_engine.cpp;$AP/core/guest_cpu/guest_cpu.cpp;$AP/core/guest_cpu/hle_call_adapter.cpp;$AP/core/ios/ios_jit_allocator.cpp;$ROOT/engine/probe_runner.cpp;$ROOT/engine/win32_device.cpp"
# The PE loader and the mini Win32, the same sources the host tests build.
for f in loader/pe_image.cpp win32/hle.cpp win32/support.cpp win32/kernel32.cpp win32/other_dlls.cpp \
         win32/baselib.cpp win32/kernel32_sync.cpp win32/kernel32_files.cpp win32/kernel32_misc.cpp; do
    ENGINE_SOURCES="$ENGINE_SOURCES;$ROOT/$f"
done
ENGINE_INCLUDES="$ROOT/compat;$AP;$ROOT"

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
    -DCMAKE_CXX_FLAGS=-DMETA_NO_STD_FORWARD_DECLARATIONS \
    -DUNITYBRIDGE_ENGINE_SOURCES="$ENGINE_SOURCES" \
    -DUNITYBRIDGE_INCLUDE_DIRS="$ENGINE_INCLUDES"

# JemallocLibs is not a dependency of the static FEXCore target, but on Apple it carries the
# dual-mapped JIT allocator (FEXCore/Source/Utils/AllocatorHooks.cpp) the JIT calls into.
cmake --build "$BUILD" --target FEXCore Common CommonTools JemallocLibs unitybridge_engine

rm -rf "$OUT"
mkdir -p "$OUT/lib"
find "$BUILD" -name '*.a' -not -path '*/CMakeFiles/*' -exec cp -f {} "$OUT/lib/" \;
for lib in "$OUT"/lib/*.a; do
    lipo -info "$lib" | grep -q arm64 || { echo "$lib is not arm64" >&2; exit 1; }
done
git -C "$SRC" rev-parse HEAD > "$OUT/SOURCE_REVISION"
ls -la "$OUT/lib"
