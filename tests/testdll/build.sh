#!/usr/bin/env bash
# Builds tests/testdll/ubtest.dll (x86-64 Windows, no CRT) with clang + lld-link.
# Usage: build.sh <output-dir>
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?output directory}"
mkdir -p "$OUT"
# Tools can be overridden (the macOS runner uses Homebrew's LLVM and LLD).
CLANG="${CLANG:-clang}"
DLLTOOL="${DLLTOOL:-llvm-dlltool}"
LLD_LINK="${LLD_LINK:-lld-link}"
"$CLANG" --target=x86_64-pc-windows-msvc -O2 -fno-stack-protector -mno-stack-arg-probe -ffreestanding \
    -c "$HERE/ubtest.c" -o "$OUT/ubtest.obj"
libs=()
for def in kernel32 oleaut32 other shell32 baselib; do
    "$DLLTOOL" -m i386:x86-64 -d "$HERE/$def.def" -l "$OUT/$def.lib"
    libs+=("$OUT/$def.lib")
done
"$LLD_LINK" /dll /noentry /nodefaultlib /machine:x64 "$OUT/ubtest.obj" "${libs[@]}" /out:"$OUT/ubtest.dll"
