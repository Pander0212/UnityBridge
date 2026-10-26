#!/usr/bin/env bash
# Builds tests/testdll/ubtest.dll (x86-64 Windows, no CRT) with clang + lld-link.
# Usage: build.sh <output-dir>
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?output directory}"
mkdir -p "$OUT"
clang --target=x86_64-pc-windows-msvc -O2 -fno-stack-protector -mno-stack-arg-probe -ffreestanding \
    -c "$HERE/ubtest.c" -o "$OUT/ubtest.obj"
llvm-dlltool -m i386:x86-64 -d "$HERE/kernel32.def" -l "$OUT/kernel32.lib"
lld-link /dll /noentry /nodefaultlib /machine:x64 "$OUT/ubtest.obj" "$OUT/kernel32.lib" /out:"$OUT/ubtest.dll"
