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
# Output in build/fexcore-ios/out: lib/*.a (FEXCore, its helpers, libhlslcc.a and libunitybridge_engine.a).
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
marker = "# UnityBridge engine hook v2"
assert "# UnityBridge engine hook" not in text or marker in text, \
    "FEX checkout has the old UnityBridge hook; delete build/aetherps4"
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
    if (UNITYBRIDGE_HLSLCC_DIR)
      file(GLOB UB_HLSLCC_SOURCES ${UNITYBRIDGE_HLSLCC_DIR}/src/*.cpp ${UNITYBRIDGE_HLSLCC_DIR}/src/cbstring/*.c)
      add_library(hlslcc STATIC ${UB_HLSLCC_SOURCES})
      target_include_directories(hlslcc SYSTEM PUBLIC ${UNITYBRIDGE_HLSLCC_DIR} ${UNITYBRIDGE_HLSLCC_DIR}/include
        PRIVATE ${UNITYBRIDGE_HLSLCC_DIR}/src ${UNITYBRIDGE_HLSLCC_DIR}/src/cbstring
        ${UNITYBRIDGE_HLSLCC_DIR}/src/internal_includes)
      target_compile_options(hlslcc PRIVATE -w)
      set_target_properties(hlslcc PROPERTIES CXX_STANDARD 11)
      target_link_libraries(unitybridge_engine PRIVATE hlslcc)
    endif()
  endif()
""" % marker
    open(path, "w").write(text.replace(anchor, hook))
PY

# Built with ENABLE_FEX_ALLOCATOR=OFF (below), FEXCore's allocations go through the system branch of
# AllocatorHooks.cpp, written for glibc. Darwin's aligned_alloc and posix_memalign refuse an
# alignment below sizeof(void*) (EINVAL, NULL), and aligned_alloc also a size that is not a multiple
# of the alignment; FEX asks for alignof(T) of 1, 2 or 4 (fextl::vector<uint8_t>, ...). Raise the
# alignment to 16 and round the size up. Idempotent.
python3 - "$FEX/FEXCore/Source/Utils/AllocatorHooks.cpp" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
marker = "// UnityBridge: Darwin alignment rules"
if marker not in text:
    edits = [
        ("  return ::posix_memalign(&ptr, align, s) == 0 ? ptr : nullptr;\n",
         "  return ::posix_memalign(&ptr, DarwinAlign(align), s) == 0 ? ptr : nullptr;\n"),
        ("  return ::posix_memalign(r, a, s);\n",
         "  return ::posix_memalign(r, DarwinAlign(a), s);\n"),
        ("  return ::aligned_alloc(a, s);\n",
         "  const size_t align = DarwinAlign(a);\n  return ::aligned_alloc(align, (s + align - 1) & ~(align - 1));\n"),
    ]
    for old, new in edits:
        assert text.count(old) == 1, "AllocatorHooks.cpp changed; update the UnityBridge patch: " + old
        text = text.replace(old, new)
    anchor = "#else\nvoid InitializeThread() {}\n"
    assert text.count(anchor) == 1, "AllocatorHooks.cpp changed; update the UnityBridge patch (anchor)"
    text = text.replace(anchor, anchor + """%s: at least 16, a power of two
static inline size_t DarwinAlign(size_t a) {
  size_t r = 16;
  while (r < a) r <<= 1;
  return r;
}
""" % marker)
    open(path, "w").write(text)
PY

# HLSLcc (M4: shaders converted on the device), built as its own library in the same project.
bash "$ROOT/scripts/fetch-hlslcc.sh"

AP="$ROOT/third_party/aetherps4/src"
ENGINE_SOURCES="$AP/core/fex/fex_guest_engine.cpp;$AP/core/guest_cpu/guest_cpu.cpp;$AP/core/guest_cpu/hle_call_adapter.cpp;$AP/core/ios/ios_jit_allocator.cpp;$ROOT/engine/probe_runner.cpp;$ROOT/engine/win32_device.cpp;$ROOT/engine/il2cpp_probe.cpp;$ROOT/engine/mach_fault.cpp;$ROOT/engine/il2cpp_bridge.cpp;$ROOT/engine/icall_bridge.cpp;$ROOT/engine/il2cpp_intercept.cpp;$ROOT/engine/unity_host.cpp;$ROOT/engine/shader_xlat.cpp;$ROOT/engine/shader_gles3.cpp;$ROOT/engine/unity_file.cpp;$ROOT/engine/unity_typetree.cpp;$ROOT/engine/shader_import.cpp;$ROOT/engine/unity_textures.cpp;$ROOT/engine/call_native_arm64.S"
# The PE loader and the mini Win32, the same sources the host tests build.
for f in loader/pe_image.cpp win32/hle.cpp win32/support.cpp win32/kernel32.cpp win32/other_dlls.cpp \
         win32/baselib.cpp win32/kernel32_sync.cpp win32/kernel32_files.cpp win32/kernel32_misc.cpp \
         win32/tls.cpp win32/ntdll_unwind.cpp win32/ntdll_exception.cpp; do
    ENGINE_SOURCES="$ENGINE_SOURCES;$ROOT/$f"
done
ENGINE_INCLUDES="$ROOT/compat;$AP;$ROOT"

# CMAKE_SYSTEM_PROCESSOR must be set: FEX's architecture detection fails on an empty value when
# cross-compiling. BUILD_FEXCORE_ONLY leaves out FEX's Linux frontend (ELF loader, syscalls),
# which does not build for Darwin; UnityBridge supplies its own (Windows) frontend.
# ENABLE_FEX_ALLOCATOR=OFF: FEXCore's internal allocations go to the system malloc (the Darwin
# branch of FEXCore/Source/Utils/AllocatorHooks.cpp) instead of rpmalloc, which maps a 256 MiB span,
# 256 MiB-aligned, per thread heap and size class. With the Unity engine in the process (many host
# threads running guest code) rpmalloc returned NULL for FEX's thread state on new threads (M3e).
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
    -DENABLE_FEX_ALLOCATOR=OFF \
    -DENABLE_CCACHE=ON \
    -DENABLE_LTO=OFF \
    -DENABLE_WERROR=OFF \
    -DENABLE_STRICT_WERROR=OFF \
    -DTUNE_CPU=none \
    -DCMAKE_CXX_FLAGS=-DMETA_NO_STD_FORWARD_DECLARATIONS \
    -DUNITYBRIDGE_ENGINE_SOURCES="$ENGINE_SOURCES" \
    -DUNITYBRIDGE_INCLUDE_DIRS="$ENGINE_INCLUDES" \
    -DUNITYBRIDGE_HLSLCC_DIR="$ROOT/build/hlslcc"

# JemallocLibs is not a dependency of the static FEXCore target, but on Apple it carries the
# dual-mapped JIT allocator (FEXCore/Source/Utils/AllocatorHooks.cpp) the JIT calls into.
cmake --build "$BUILD" --target FEXCore Common CommonTools JemallocLibs hlslcc unitybridge_engine

rm -rf "$OUT"
mkdir -p "$OUT/lib"
find "$BUILD" -name '*.a' -not -path '*/CMakeFiles/*' -exec cp -f {} "$OUT/lib/" \;
for lib in "$OUT"/lib/*.a; do
    lipo -info "$lib" | grep -q arm64 || { echo "$lib is not arm64" >&2; exit 1; }
done
git -C "$SRC" rev-parse HEAD > "$OUT/SOURCE_REVISION"
ls -la "$OUT/lib"
