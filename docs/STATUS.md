# Status

Labels: **[R]** researched (sources or files read), **[S]** seen in the simulator, **[i]** confirmed on iPhone.
The background study lives in EverPlay's `docs/UNITY.md` (branch `claude/unity-research`).

First target: **Fellworld**, Unity **2021.3.18f1**, IL2CPP, metadata v29, D3D11-only shaders.

| Step | Goal | State |
|---|---|---|
| M0 | Analyse `GameAssembly.dll`: imports, exports, metadata | done [R] |
| M1 | Analyse Android `libunity.so` 2021.3.18f1, match the API | done [R] |
| M2 | On iPhone: FEXCore + PE loader + mini Win32, `il2cpp_init` succeeds | in progress: FEXCore runs on iPhone [i]; loader + mini Win32 host-tested |
| M3 | Husk-style loader runs `libunity.so` with the stand-in `libil2cpp.so`; scene loads (log) | — |
| M4 | D3D11 shaders converted; first frame | — |

## M0: GameAssembly.dll (Fellworld) [R]

- x86-64 PE; `.text` 2.4 MB + `il2cpp` 14.7 MB code; TLS directory; 77,489 `.pdata` unwind entries.
- Imports: 206 functions from 10 DLLs (KERNEL32 158, OLEAUT32 10, ole32 6, USER32 5, WS2_32 4, ADVAPI32 3,
  IPHLPAPI 3, SHELL32 2, dbghelp 2, plus 13 from Unity's `baselib.dll`).
- Exports: 380, of which 235 `il2cpp_*`.
- Metadata v29: 49,597 methods, 45 P/Invokes (Win32 used by the .NET class library, resolved at run time),
  1,670 InternalCall names.
- Hard parts: x64 exceptions (`RtlVirtualUnwind`/`RtlUnwindEx` over `.pdata`), GC thread suspension
  (`SuspendThread`/`GetThreadContext` on translated threads), thunks in both directions.

## M1: Android libunity.so 2021.3.18f1 [R]

Source: Unity's Android module for editor 2021.3.18f1 (changeset `3129e69bc0c7`), read only.

- `libmain.so` `dlopen`s `libunity.so` and `libil2cpp.so`; `libunity` resolves the il2cpp API by name.
- 228 `il2cpp_*` names in `libunity`; **all 228 are exported** by Fellworld's `GameAssembly.dll`.
- Every `UnityEngine.*` InternalCall of the game is registered by `libunity` except 8 `UnityEngine.Windows.WebCam.*`
  (stub them). `System.*`/`Mono.*` icalls live inside `GameAssembly.dll`.
- Callback-taking API functions (need x64 → ARM64 thunks): `il2cpp_set_memory_callbacks`,
  `il2cpp_register_log_callback`, `il2cpp_set_find_plugin_callback`, `il2cpp_unity_install_unitytls_interface`,
  `il2cpp_gc_foreach_heap`, `il2cpp_gchandle_foreach_get_target`, `il2cpp_gc_set_external_allocation_tracker`,
  `il2cpp_gc_set_external_wbarrier_tracker` (`il2cpp_register_debugger_agent_transport`: leave out).
- `NEEDED`: libandroid, liblog, libz, libEGL, libm, libdl, libc. 4 KiB-aligned; the code/data shared 16 KiB page
  holds only `GNU_RELRO` data, which a Husk-style loader handles.

Open risks (device only): direct reads of il2cpp structures by `libunity` (MSVC vs Clang bitfield layout, `long`
size), struct-by-value calling conventions in thunks, the Windows platform tag in the data files, D3D11 shaders.

## M2 checklist

Building blocks chosen (2026-10-26) [R]:
- **FEXCore**: AetherPS4's `runtime/sources/fexcore-darwin` (FEX `f2b679f` + Darwin/iOS port, MIT,
  all externals in tree, `BUILD_FEXCORE_ONLY`, iOS dual-mapped JIT allocator via StikDebug's
  BreakpointJIT protocol). Fathom uses the same tree but does not publish it and states no license
  for its own code, so nothing is taken from Fathom. Madeira's FEX fork targets ARM64EC inside Wine.
- **Guest engine**: AetherPS4's `GuestEngine` (`src/core/fex/fex_guest_engine.*`, MIT): creates the
  FEXCore context, threads, `CallGuest(rip, args)`, and routes the guest's `syscall` to
  `GuestBridge::Invoke(HleCallFrame&)`. Win32 import stubs will be `mov eax, id; syscall; ret`.
  Its `RunControlledHarness()` is the device self-test. Copied under `third_party/aetherps4`
  (see `PROVENANCE.md`).
- **BreakpointJIT.framework**: written here (`jit/BreakpointJIT.S`), no entitlements.

- [x] M2a: CI builds FEXCore + `libunitybridge_engine.a` for arm64 iOS (`scripts/build-fexcore-ios.sh`,
      workflow "Probe IPA"). Green on 2026-10-26 (run 40388559916, commit `6d2e902`): 188 objects, app links,
      artifact `UnityBridgeProbe-ipa`. Only availability warnings (`os_sync_*` needs iOS 17.4; deployment
      target is 17.0, harmless on iOS 26). Note: macOS runners did not start while the repo was private.
- [x] M2b: `UnityBridgeProbe` app (`app/`) **[i] confirmed on iPhone 15 (A16), iOS 26.6.2, 2026-10-27**:
      all 8 checks of FEXCore's controlled harness pass (x86-64 integer, flags, SSE, HLE bridge via `syscall`,
      guest threads, TLS via `fs:`, unaligned access, code invalidation). JIT memory from StikDebug:
      16 KiB + 16 MiB code buffer at 0x164bf0000 (high enough for the low-address FEX bug Madeira reports).
      - First attempt crashed with SIGTRAP in `BreakGetJITMapping`: JIT had been enabled from StikDebug's own
        app list, which leaves `CS_DEBUGGED` set but detaches. Fixed: the app opens
        `stikdebug://enable-jit?bundle-id&pid&script-data` with Madeira's debugger script
        (`third_party/madeira`) and waits for `P_TRACED`; without a debugger the probe refuses to start.
      - Open: FEX logs `Failed to mprotect last page of code buffer` (its guard page; iOS refuses mprotect on
        JIT memory). Not fatal, but an overrun of the code buffer would not be caught.
      - Signing: a free Apple ID (SideStore/Sideloadly) keeps `get-task-allow`; a distribution certificate
        ("without JIT") does not.
- [ ] M2c: PE loader and mini Win32, tested on the host (x86-64 Linux, CI "Host tests"):
  - [x] `loader/pe_image`: PE32+ x86-64 loader (sections, DIR64 relocations, imports incl. delay-load and
        ordinals as `#N`, exports, TLS directory, `.pdata`), bounds-checked. Reads Fellworld's
        `GameAssembly.dll` correctly (7 sections, 380 exports, 77,489 unwind entries).
  - [x] `win32/`: one x86-64 stub per import (`mov r10, rcx; mov eax, op; syscall; ret` for FEXCore's HLE
        bridge; a jump form for host tests), dispatch, module registry, UTF-16 helpers.
  - [x] Implemented: KERNEL32 142 of 158 (heap, virtual memory, TLS/FLS, critical sections, SRW locks,
        condition variables, events/mutexes/semaphores/waits, files/directories/find/mappings, paths,
        time, system info, code pages, locale, time zone, modules, console, process), OLEAUT32 (BSTR,
        SAFEARRAY), OLE32, ADVAPI32 (random), SHELL32 (folders), WS2_32/IPHLPAPI/DBGHELP/USER32
        (answered as unavailable), Unity's `baselib.dll` (13). **190 of 206 imports** of Fellworld's
        `GameAssembly.dll` (`tools/pe_inspect`).
  - [x] `tests/testdll/ubtest.dll`: our own Windows DLL (clang + lld-link, no CRT) run natively against the
        mini Win32 with 21 checks; ASan/UBSan clean.
  - [ ] Threads: `CreateThread`, `SuspendThread`/`ResumeThread`/`GetThreadContext` (GC stop-the-world),
        `SetThreadPriority`, `QueueUserAPC`. Need the guest-call hook into FEXCore.
  - [ ] x64 exceptions: `RaiseException`, `RtlUnwind(Ex)`, `RtlVirtualUnwind`, `RtlLookupFunctionEntry`,
        `RtlCaptureContext`, `RtlCaptureStackBackTrace`, `RtlPcToFileHeader`, unhandled-exception filters.
  - [ ] Path mapping: `C:\` is the host root; `C:\Users\Player` goes to the profile folder
        (`SetProfileRoot`). On iPhone: the app's Documents folder.
  - Known gaps: views of an anonymous (`INVALID_HANDLE_VALUE`) file mapping do not share memory;
        `EnumSystemLocalesW` makes no callbacks; baselib error codes unverified.
- [ ] PE loader: sections, relocations, imports, TLS callbacks, `.pdata` registration.
- [ ] Mini Win32 for the imports and P/Invokes above; `baselib.dll` implemented natively.
- [ ] ARM64 → x64 thunks for the 228 `il2cpp_*`; x64 → ARM64 for callbacks.
- [ ] iOS test app built by GitHub Actions (macOS), JIT via StikDebug/StikJIT; game files copied onto the device by
      the user.
