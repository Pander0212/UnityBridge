# Status

Labels: **[R]** researched (sources or files read), **[S]** seen in the simulator, **[i]** confirmed on iPhone.
The background study lives in EverPlay's `docs/UNITY.md` (branch `claude/unity-research`).

First target: **Fellworld**, Unity **2021.3.18f1**, IL2CPP, metadata v29, D3D11-only shaders.

| Step | Goal | State |
|---|---|---|
| M0 | Analyse `GameAssembly.dll`: imports, exports, metadata | done [R] |
| M1 | Analyse Android `libunity.so` 2021.3.18f1, match the API | done [R] |
| M2 | On iPhone: FEXCore + PE loader + mini Win32, `il2cpp_init` succeeds | in progress: FEXCore and a Windows DLL against the mini Win32 run on iPhone [i]; `GameAssembly.dll` not yet |
| M3 | Husk-style loader runs `libunity.so` with the stand-in `libil2cpp.so`; scene loads (log) | research done [R]: Husk's translation layer (GPL-2.0+) already runs Android Unity games; bridge measured |
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
- [x] M2d: `ubtest.dll` under FEXCore on iPhone ("Run Windows DLL test", `engine/win32_device.cpp`)
      **[i] iPhone 15 (A16), iOS 26.6.2, 2026-10-27: all 23 checks pass** (commit `26b3e28`) — PE loader,
      `syscall` stubs into the mini Win32, Microsoft x64 calls (4+ arguments, stack arguments, xmm0 in/out,
      callee-saved registers), TEB via `gs:`, files in the profile folder, sync objects, locale, OLEAUT32 by
      ordinal, unimplemented imports logged and answered with 0.
      - First run: 22 of 23; the missing-import check never reached its stub. Cause: every call mapped a fresh
        trampoline page and unmapped it afterwards; a later page landed where an earlier one had been, and
        FEXCore, which caches translated blocks by guest address, ran the earlier call's target again. Fix: one
        trampoline page for all calls (`call r11; hlt`, target in r11). Lesson for M2e: guest code addresses
        must not be reused without `GuestEngine::Invalidate`.
- [x] M2d+: calls in both directions (commit `907712f`) **[i] iPhone 15, iOS 26.6.2, 2026-10-27: 25 of 25**:
      `GuestEngine::CallWin64` (vendored engine, UnityBridge addition) runs a guest function with the Microsoft
      x64 convention to its return through FEXCore's `HandleCallback` — on one reusable guest thread from the
      host (what `libunity` calling `il2cpp_*` will need), or nested on the guest thread that is inside an
      import (callbacks). `Win32::CallGuest` lets imports call back: `FlsFree` runs the slot's callback,
      `EnumSystemLocalesW` reports en-US. `ubtest.dll`: 25 checks, two new ones for callbacks (one calls an
      import from inside the callback: guest → host → guest → host). Host tests pass (also ASan/UBSan).
- [ ] M2e step 1, threads (commit `adcc45e`, CI green, **not yet run on iPhone**): `CreateThread` (incl. `CREATE_SUSPENDED`),
      `ResumeThread`, `GetExitCodeThread`, `GetThreadId`, `Set/GetThreadPriority` (accepted, not applied); a
      thread handle is waitable and signalled with its exit code. Each guest thread is a host thread with a
      FEXCore guest thread of its own (1 MiB stack, TEB with stack limits and thread id), started through
      `Win32::SetThreadRunner`. `ubtest.dll`: 27 checks; the new ones check that a thread has its own TLS
      values and last-error code. Still missing for threads: `SuspendThread`/`GetThreadContext` on a running
      thread (the garbage collector's stop-the-world), `QueueUserAPC`, FLS callbacks at thread exit.
- [ ] PE loader: sections, relocations, imports, TLS callbacks, `.pdata` registration.
- [ ] Mini Win32 for the imports and P/Invokes above; `baselib.dll` implemented natively.
- [ ] ARM64 → x64 thunks for the 228 `il2cpp_*`; x64 → ARM64 for callbacks.
- [ ] iOS test app built by GitHub Actions (macOS), JIT via StikDebug/StikJIT; game files copied onto the device by
      the user.

## M3 research: running Android's libunity.so on iPhone (2026-10-27) [R]

**Husk already does this part.** Husk (Leviidev/Husk, commit `b390dfa`, **GPL-2.0-or-later**, so it can
be combined with UnityBridge's GPL-3.0-or-later) has a "translation layer" that runs Android games'
arm64 libraries natively on iOS, with no Android system: its own ELF loader (`husk-tl-ld.c`: maps,
relocates, binds imports, Android's per-library symbol scope, which matters because Unity's libraries
each carry their own C++ runtime), bionic libc shims onto Darwin (`husk-tl-bionic*.c`), a JNI and a
small Dalvik interpreter for the app's Java side, EGL/GLES through ANGLE, OpenSL ES and FMOD's Java
glue. It has an engine driver for Unity (`husk-tl-unity.c`, `husk-tl-unity-app.c`): `libmain.so`
`JNI_OnLoad`, `NativeLoader.load`, then the `UnityPlayer` natives (`nativeRender` per frame, pause,
input). Its README lists Unity games running on the translation layer, and its screenshots show Beach
Buggy Racing 2 and Subway Surfers (both Unity). These are their claims; we have not run them yet. Husk
also already shares its JIT region with a FEXCore module (its PS4 mode, `husk-jit-arena.c`,
`husk-console-abi.h`), the same combination the hybrid needs. All 109 files in
`src/translation-layer-next/` carry GPL-2.0-or-later, two headers carry no tag; nothing from ATL or
AOSP.

So M3 becomes "Husk's Unity driver plus a stand-in `libil2cpp.so`" instead of writing a loader:

| Piece | Where from | Work |
|---|---|---|
| ELF loader, bionic, JNI, Dalvik, EGL (ANGLE), audio, input | Husk translation layer | take over, keep in sync |
| `libmain.so`, `libunity.so`, `classes.dex` for the game's exact Unity version (2021.3.18f1 for Fellworld) | Unity's Android player package (`Variations/il2cpp/Release/`) | **not redistributable**: the user fetches it from Unity, the app never ships it |
| `libil2cpp.so` | ours | a virtual library: Husk's `dlopen_impl` answers `libil2cpp.so` with it, `dlsym` returns our 228 bridge functions (about 20 lines in `husk-tl-bionic.c`) |
| the APK the driver expects | ours | built on the device from the pieces above plus the game's `*_Data` as `assets/bin/Data/` (or the driver taught to take a folder) |

**The bridge in both directions** (`tools/research/icall_abi.py`, data only, Fellworld):
- libunity → GameAssembly: the 228 `il2cpp_*` functions take only integers and pointers, so one
  generic thunk (x0–x7 → rcx, rdx, r8, r9 + stack) serves all of them, running on a guest thread per
  host thread via `GuestEngine::CallWin64` (built and confirmed on iPhone in M2d+). The 8 callback-taking ones
  (M1) need the reverse thunks below.
- GameAssembly → libunity: 1,689 InternalCall methods in the metadata, **1,371 of them engine functions**
  (`System.*`/`Mono.*` ones stay inside GameAssembly). Of these: **1,186 integer/pointer only**, 16 of them
  with more than 8 arguments (stack arguments), **166 with float/double** arguments or results, 2 with
  both, 1 generic. **No value type is passed or returned by value**: Unity's `_Injected` bindings pass
  vectors and matrices by reference. 73 distinct signatures in all, 50 with floats. So the reverse
  thunk is a mini Win32 stub per registered function (`il2cpp_add_internal_call` is intercepted: the
  ARM64 pointer is swapped for a stub) whose handler moves the Microsoft x64 slots into AAPCS64
  registers by the method's signature from the metadata; the float cases are a table of 50 shapes, not
  a general marshaller.

**Data and assets** (Fellworld):
- Serialized files say `targetPlatform 19` (StandaloneWindows64); an Android player expects 13. Whether
  the player refuses other targets for its main data is open; the field can be rewritten when the files
  are copied in.
- Textures: 148 DXT5 + 1 DXT1 (about 35 megapixels, most of the texture data), the rest uncompressed.
  Under GLES on iPhone (ANGLE over Metal) S3TC is only there if the GPU reports BC support; otherwise
  Unity's player decompresses unsupported formats on the CPU (load time and memory). To measure on device.
- Shaders: D3D11 only (M4 research below), the hardest remaining piece.

Next steps, in order:
- [ ] Build Husk's translation layer inside UnityBridge's app (or UnityBridge as a Husk engine module),
      with FEXCore sharing Husk's JIT arena.
- [ ] Virtual `libil2cpp.so` with the 228 generic thunks; reverse thunks for callbacks and InternalCalls.
- [ ] Synthetic APK from the user's Unity Android package + game data; platform field rewrite.
- [ ] Prerequisite from M2e: TLS and exceptions in the mini Win32 (needed before `il2cpp_init` gets far).
- [ ] M4: D3D11 shaders → GLES3.

## M4 research: D3D11 shaders → GLES3 (2026-10-27) [R]

Measured with `tools/research/*.py` (UnityPy, data only):

| | Fellworld (2021.3.18) | FantasyStudio (6000.0.60) |
|---|---|---|
| Shaders / passes | 32 / 77 (URP, TextMeshPro) | 55 / 139 (URP 2D) |
| Subprograms | 2,639 (245 vertex, 2,394 pixel) | 5,489 (2,317 vertex, 3,172 pixel) |
| Shader model | SM 4.0 (95 %), SM 5.0 | SM 4.0, few SM 5.0 |
| Geometry/hull/domain | none | none |
| Compute shaders | 0 | 1 |
| Unique DXBC programs | 506 | (not counted) |

- **DXBC in Unity player builds has no reflection** (`RDEF` is stripped): only `ISGN`/`OSGN` and
  `SHDR`/`SHEX`. Names and layouts live in Unity's own tables instead: per program
  `m_CommonParameters` (or per-subprogram parameters) with `m_TextureParams` (register `m_Index`,
  `m_SamplerIndex`, `m_Dim`), `m_ConstantBuffers` (name, size, members), `m_ConstantBufferBindings`
  (register), `m_Samplers`, `m_UAVParams`; names by `m_NameIndex`.
- **Target format, from Unity's own Android resources** (2021.3.18 Android module,
  `Data/unity default resources`: platforms 5, 9 = GLES3x, 18 = Vulkan; 153 GLES3 + 153 SPIR-V programs):
  a GLES3 program is **HLSLcc output text** (`hlslcc_mtx4x4unity_ObjectToWorld`, `u_xlat0`,
  `in_POSITION0`, `SV_Target0`, `UNITY_LOCATION`/`UNITY_BINDING` macros) with **vertex and fragment in
  one source**, split by `#ifdef VERTEX` / `#ifdef FRAGMENT`, stored as one vertex subprogram.
- **Converter**: [HLSLcc](https://github.com/Unity-Technologies/HLSLcc) (MIT) is the translator Unity
  itself uses for GLES3/Vulkan/Metal, so its output matches Unity's conventions. It reads reflection from
  `RDEF`; since that is stripped, the plan is to synthesise it from Unity's parameter tables (or feed
  HLSLcc's reflection structures directly). Alternative: dxbc-spirv (DXVK 3.0, MIT per mirror) for Vulkan,
  but then Unity's Vulkan binding scheme has to be rebuilt.
- **Plan (on device at import; game data never in CI):**
  1. For each unique D3D11 program pair (vertex + pixel), build reflection from the parameter tables,
     run HLSLcc → GLSL ES 3.0, join both into Unity's one-source layout.
  2. Add a GLES3x platform to each Shader: new LZ4 blob, parameter blob and `m_PlayerSubPrograms` group
     with `kShaderGpuProgramGLES3`; write the modified serialized file next to the original.
  3. Check with the Android player (M3) that it picks GLES3 and draws (ANGLE → Metal through Husk's layer).
- Open questions: how the GLES3 parameter blob maps uniforms/texture units; whether the Android player
  accepts data built for `StandaloneWindows64` at all (M3 risk); keyword/variant selection.
