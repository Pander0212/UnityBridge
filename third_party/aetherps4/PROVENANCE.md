# Files from AetherPS4

Copied from [Leviidev/AetherPS4](https://github.com/Leviidev/AetherPS4) at commit
`3347fc13f19860855ce91b66356940989ea6b6a6`. Each file keeps its own SPDX header:

| File | License |
|---|---|
| `src/core/fex/fex_guest_engine.{h,cpp}` | MIT |
| `src/core/guest_cpu/guest_cpu.{h,cpp}`, `src/core/guest_cpu/hle_call_adapter.{h,cpp}` | MIT |
| `runtime/probes/fexcore-guest-harness.cpp` (reference for `engine/probe_runner.cpp`, not built) | MIT |
| `src/core/ios/ios_jit_allocator.{h,cpp}` | GPL-2.0-or-later (shadPS4 Emulator Project) |
| `src/core/libraries/kernel/threads/exception.h`, `src/common/types.h` | GPL-2.0-or-later (shadPS4 Emulator Project) |

License texts: `LICENSES/MIT.txt`, `LICENSES/GPL-2.0-or-later.txt`. Both are compatible with
UnityBridge's GPL-3.0-or-later.

Not copied: FEXCore itself. `scripts/build-fexcore-ios.sh` checks out
`runtime/sources/fexcore-darwin` (FEX upstream `f2b679f` + Darwin/iOS port, MIT) at the commit above
and inserts a small CMake hook that builds UnityBridge's engine library inside FEX's project.

Local stand-ins: `compat/common/logging/log.h` replaces shadPS4's logging header;
`jit/BreakpointJIT.S` provides the `BreakpointJIT.framework` that FEXCore and
`ios_jit_allocator.cpp` `dlopen`.

## Local changes

- `src/core/fex/fex_guest_engine.{h,cpp}`: added `Win64Call` and `GuestEngine::CallWin64` (Microsoft x64
  calls on an idle thread or nested inside `GuestBridge::Invoke`, built on FEXCore's `HandleCallback`).
  Marked "UnityBridge addition" in the header; MIT like the rest of the file.
