# Husk's Android runtime

Not copied into this repository. `scripts/fetch-husk.sh` checks out
[Leviidev/Husk](https://github.com/Leviidev/Husk) at commit
`4ac655293f970e706af65dea013799ee11dfcd9d` (sparse: `LICENSE`, `src/translation-layer/`,
`src/translation-layer-next/`) into `build/husk`, and the probe app (`app/project.yml`) compiles those
sources unmodified, with the build settings Husk's own app uses (ObjC ARC, gnu11, the same SDK
frameworks and libraries).

License: GPL-2.0-or-later (Husk's `LICENSE`; every source file carries
`SPDX-License-Identifier: GPL-2.0-or-later`, except `husk-console-abi.h` and `husk-jit-arena.h`, which
carry no tag and fall under the repository license, and `stb_vorbis.inc`, Sean Barrett's
public-domain/MIT decoder). Compatible with UnityBridge's GPL-3.0-or-later; the combined app is
distributed under GPL-3.0-or-later.

UnityBridge's side of the integration is its own code:
- `engine/unity_host.cpp`: `husk_ios_jit_get_mapping` (the executable region Husk's loader looks up by
  name; in Husk the QEMU library provides it), `ub_il2cpp_symbol`.
- `app/Sources/unity_loader.c`: the virtual `libil2cpp.so` (through Husk's `tl_ld_interpose`), the
  synthetic APK, and the call into Husk's Unity driver (`tl_unity_start`).

Nothing from Husk's app (Swift UI, QEMU, ANGLE, the JIT helper) is used.
