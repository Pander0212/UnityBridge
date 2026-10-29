# Status

Labels: **[R]** researched (sources or files read), **[S]** seen in the simulator, **[i]** confirmed on iPhone.
The background study lives in EverPlay's `docs/UNITY.md` (branch `claude/unity-research`).

First target: **Fellworld**, Unity **2021.3.18f1**, IL2CPP, metadata v29, D3D11-only shaders.

| Step | Goal | State |
|---|---|---|
| M0 | Analyse `GameAssembly.dll`: imports, exports, metadata | done [R] |
| M1 | Analyse Android `libunity.so` 2021.3.18f1, match the API | done [R] |
| M2 | On iPhone: FEXCore + PE loader + mini Win32, `il2cpp_init` succeeds | **reached [i] (2026-10-28):** Fellworld's real `GameAssembly.dll` runs `il2cpp_init` to success on iPhone (returns 1, corlib + domain non-null, 0 unimplemented imports) |
| M3 | Husk-style loader runs `libunity.so` with the stand-in `libil2cpp.so`; scene loads (log) | research done [R]: Husk's translation layer (GPL-2.0+) already runs Android Unity games; bridge measured |
| M4 | D3D11 shaders converted; first frame | **reached [i] (2026-10-29):** Fellworld's main menu rendered on the iPhone with the GLES3 shaders converted on the device; 60 s, ~32 fps, no crash |

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
  - [x] x64 exceptions: `RaiseException`, `RtlUnwind(Ex)`, `RtlVirtualUnwind`, `RtlLookupFunctionEntry`,
        `RtlCaptureContext`, `RtlCaptureStackBackTrace`, `RtlPcToFileHeader`, unhandled-exception filters.
        Done host-side (M2e steps 3-4 below); guest language handlers (incl. GameAssembly's own
        `__C_specific_handler`/`__CxxFrameHandler*`) are driven through a callback, with the full C++
        catch flow to be confirmed against real `GameAssembly.dll`.
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
- [x] M2e step 1, threads (commit `adcc45e`) **[i] iPhone 15, iOS 26.6.2, 2026-10-27: 27 of 27**: `CreateThread` (incl. `CREATE_SUSPENDED`),
      `ResumeThread`, `GetExitCodeThread`, `GetThreadId`, `Set/GetThreadPriority` (accepted, not applied); a
      thread handle is waitable and signalled with its exit code. Each guest thread is a host thread with a
      FEXCore guest thread of its own (1 MiB stack, TEB with stack limits and thread id), started through
      `Win32::SetThreadRunner`. `ubtest.dll`: 27 checks; the new ones check that a thread has its own TLS
      values and last-error code. Still missing for threads: `SuspendThread`/`GetThreadContext` on a running
      thread (the garbage collector's stop-the-world), `QueueUserAPC`, FLS callbacks at thread exit.
- [x] M2e step 2, implicit TLS (`win32/tls.cpp`) **[i] iPhone 15 (A16), iOS 26.6.2, 2026-10-27:
      part of 31 of 31** (commit `a808fab`):
      each guest thread gets its own copy of the DLL's TLS directory, reached through
      `TEB.ThreadLocalStoragePointer` (`gs:[0x58]`), and the TLS callbacks run with
      `DLL_PROCESS_ATTACH`/`DLL_THREAD_ATTACH`/`DLL_THREAD_DETACH`. `Win32::RegisterTlsImage` assigns the
      module a TLS index and writes it to the directory's `AddressOfIndex`; `AllocThreadTls` copies the
      template (raw data + zero fill) per thread; `InitTeb` fills the shared x64 TEB layout. The host test
      sets `%gs` per thread with `arch_prctl` (x86-64 Linux keeps its own TLS in `%fs`, so `%gs` is free);
      the device sets `GsBase` as before and writes TEB+0x58. `ubtest.dll` builds its own TLS directory
      (no CRT, as MSVC's `tlssup.c` does: `.tls` bracketed by `_tls_start`/`_tls_end`, `.CRT$XL*` callbacks,
      `_tls_used`), and the new check proves a `__declspec(thread)` variable is a fresh per-thread copy of
      its template, a new thread does not see another thread's write, and the callback ran for each thread.
- [x] M2e step 3, x64 exceptions / SEH (`win32/ntdll_unwind.cpp`, `win32/ntdll_exception.cpp`)
      **[i] iPhone 15 (A16), iOS 26.6.2, 2026-10-27: all 31 of 31 pass** (commit `a808fab`):
      `RaiseException`, `RtlCaptureContext`,
      `RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlUnwindEx`, `__C_specific_handler`,
      `RtlRestoreContext` (incl. `STATUS_UNWIND_CONSOLIDATE` → consolidate callback, as MSVC's C++
      exceptions use it), `RtlPcToFileHeader`, `RtlCaptureStackBackTrace`, over the DLL's
      `.pdata`/`UNWIND_INFO` (`Win32::RegisterUnwindImage`). The unwinder, the epilogue decoder and
      `__C_specific_handler` are adapted from Wine's `dlls/ntdll` (LGPL-2.1-or-later, GPL-3.0
      compatible; `third_party/wine/PROVENANCE.md`).
      - **Model**: the dispatcher and unwinder run on the host over the guest's tables; the guest's
        funclets (`__except` filters, `__finally` handlers, consolidate callbacks) run through
        `CallGuest` and return normally. The one non-local transfer (`NtContinue`) is recorded as a
        pending resume that each SEH function returns through to the outermost HLE handler
        (`RaiseException`), which hands the target register file to the bridge: the device loads the
        FEXCore frame and lets the stub's `ret` land at the handler target; the host entry stub
        (`win32/host_entry_x64.S`, now also capturing the full register file for `RtlCaptureContext`)
        loads the registers and jumps. No `setjmp`/`longjmp` crosses a guest call, which the device's
        nested FEXCore callbacks could not survive. `CallContext` gained the full `x86[16]`/`xmmFull`
        state, `rip`, and the resume fields; `GuestEngine`'s register order is the standard x64 one.
      - `ubtest.dll`: three new checks — `__try`/`__except` catches `RaiseException` (filter inspects
        `GetExceptionCode`); an exception raised two frames down unwinds through a `__finally` into an
        outer `__except`; and the table primitives (`RtlCaptureContext` → `RtlLookupFunctionEntry` →
        `RtlVirtualUnwind` step out one frame, `RtlPcToFileHeader`, `RtlCaptureStackBackTrace`).
- [x] M2e step 4, guest language handlers (`win32/ntdll_exception.cpp`, `engine/win32_device.cpp`)
      **[i] iPhone 15 (A16), iOS 26.6.2, 2026-10-28: all 31 of 31 pass** (commit `64fc365`), now through
      the guest-handler path and the device veneer escape: GameAssembly's exception handlers are not
      ours — its statically linked CRT provides its own `__C_specific_handler` and `__CxxFrameHandler*`,
      guest code that the OS dispatcher names in the `.pdata`/`UNWIND_INFO` and that itself calls our
      (noreturn) `RtlUnwindEx`. The dispatcher now drives such a guest handler through `CallGuest`; the
      hard part is the transfer, because on the device the handler runs inside a FEXCore callback whose
      host frames must unwind before the guest can resume at the catch target.
      - **Resume realization** is split between host and device (`Win32::SetResumeRealizedByBridge`,
        `TakePendingResume`, `SetInvokeDepth`): the SEH code only records a pending resume with the
        HLE-call depth of the dispatch that owns it. On the host (guest handlers run as native calls on
        one stack) the entry stub applies it at the innermost call, discarding the handler and
        dispatcher frames in one `ret`. On the device the bridge tracks HLE-call depth: when a nested
        guest handler's `RtlUnwindEx` asks to resume, the bridge **escapes** — it overwrites that HLE
        call's return address with FEXCore's callback-return veneer, so the stub's `ret` makes the
        enclosing callback return exactly as if the handler had returned (same mechanism FEX uses for a
        normal callback return, balancing `SignalHandlerRefCounter`). The resume then propagates to the
        call the faulting code made and is applied there. One escape per guest handler that unwinds.
      - `RtlUnwindEx` during an active dispatch unwinds from the original raise context (the guest
        handler's own frame sits above our callback machinery, off the guest's `.pdata` chain), not
        from its own call site; a standalone `RtlUnwindEx` (manual unwind) still captures its own.
      - `ubtest.dll` now provides its **own** `__C_specific_handler` (like GameAssembly) instead of
        importing ours, so all three SEH checks exercise the guest-handler path and its `RtlUnwindEx`.
        The host validates everything but the device-only veneer escape (host needs none); the escape
        is the one piece that only the iPhone run exercises, and the 2026-10-28 device run confirmed it.
      - Still open: the full `__CxxFrameHandler4` C++ flow (catch-type matching, the consolidate
        callback running the catch funclet) is GameAssembly's own guest code; this step makes the
        engine able to host it, to be confirmed against real `GameAssembly.dll`.
- [x] M2f, first real IL2CPP probe (`engine/il2cpp_probe.cpp`, app button "Run il2cpp_init"):
      **[i] `il2cpp_init` succeeds on iPhone against Fellworld's real `GameAssembly.dll` (fifth run, below)**. Loads
      Fellworld's own `GameAssembly.dll` with the PE loader, binds its 206 imports to the mini Win32,
      runs the module's TLS callbacks + `DllMain(DLL_PROCESS_ATTACH)`, calls `il2cpp_set_data_dir`
      (pointing at the game's `<Game>_Data` folder through the `C:\` host-path mapping) and then
      `il2cpp_init("IL2CPP Root Domain")`, and reports the result, `il2cpp_get_corlib`/`il2cpp_domain_get`,
      and every unimplemented import hit. The FEXCore+mini-Win32 glue mirrors `win32_device.cpp`
      (duplicated, to keep the device-confirmed ubtest runner untouched). The game's files never enter
      the repo, CI or the build machine: the user copies `GameAssembly.dll` and `<Game>_Data` into
      `Files › On My iPhone › UB Probe › Game` and runs the probe, which logs line-by-line to
      `probe.log` (so even a fault mid-init shows how far it got).
      - **First device run (iPhone 15, iOS 26.6.2, 2026-10-28):** the real 24 MB `GameAssembly.dll`
        loaded, all **206 imports resolved** (0 unresolved), `DllMain` returned TRUE, and `il2cpp_init`
        ran all the way to metadata loading — TLS, SEH, the C runtime init and il2cpp's early init all
        held on the first try, and il2cpp's own logs reached us (`[libil2cpp] ERROR: …`). It failed at
        one thing only: the metadata **path**. il2cpp appends `\Metadata\global-metadata.dat` to the
        data dir, so the data dir must be `<Game>_Data\il2cpp_data` (not `<Game>_Data`); the app now
        points there. 11 imports were called but unimplemented, all non-fatal — now added:
        `FlsGetValue2` (kernelbase), `AreFileApisANSI`, `GetUserDefaultLocaleName`, `GetLocaleInfoEx`,
        `IsValidLocaleName`, `CoGetApartmentType`. The `api-ms-win-core-*` `LoadLibrary` misses are the
        UCRT probing optional API sets and falling back to the static imports; harmless.
      - **Second run (same device, path fixed):** with the metadata path corrected, `il2cpp_init`
        gets *further* and then the app dies **silently** — `probe.log` ends right after `calling
        il2cpp_init ...` and a few `api-ms-*` `LoadLibrary` lines, with no result line. A BSD crash
        catcher (SIGSEGV/BUS/ABRT/ILL/FPE, logs guest RIP via Bachata) was added and its presence
        confirmed on device (`crash catcher installed`) — yet **no `FATAL SIGNAL` ever fired**. Cause:
        under StikDebug the debugger holds the task-level Mach port and sets `QSetIgnoredExceptions:
        EXC_BAD_ACCESS;EXC_BAD_INSTRUCTION` (see `third_party/madeira/madeira-jit.js`), expecting the
        app to register its *own* thread-level Mach handler (as Wine does). The AetherPS4 FEX port
        registers none, so a hardware fault reaches no handler, never becomes a BSD signal, and the
        process vanishes with no trace.
      - **Own Mach exception handler → fault located.** A thread-level port on each guest thread
        catches `EXC_BAD_ACCESS`/`BAD_INSTRUCTION`/`ARITHMETIC` (never `BREAKPOINT`, so debugserver's
        BRK/JIT path is untouched); since the guest diagnostics are thread-local to the *faulting*
        thread, the service thread redirects it onto a trampoline that logs on its own thread. The
        **third device run reported the fault**: `EXC_BAD_ACCESS` at `0x30140004c`, host PC inside the
        JIT buffer, faulting instruction `0xD9408294` = **`LDAPUR Xt,[Xn,#8]`** (an ARM64 load-acquire)
        with `Xn=0x301400044` → target `0x...4c` is only 4-byte aligned. **Diagnosis: not an il2cpp bug
        and not a missing API** — it is the classic x86→ARM64 hazard: FEX translates an x86 *unaligned*
        atomic / acquire access (il2cpp's metadata structs are packed, so an 8-byte field sits on a
        4-byte offset) into an ARM64 load-acquire, which the CPU rejects when unaligned. x86 allows it;
        ARM64 does not.
      - **Fix (this commit): let FEX fix up the fault.** FEXCore already handles exactly this in
        `HandleGuestSignal` — on a `BUS_ADRALN` fault inside its code buffer it backpatches the JIT'd
        instruction to a non-atomic sequence (so that static site never faults again) and resumes one
        instruction on. That path needs the faulting thread's thread-local FEX state, so the trampoline
        now, *on the faulting thread*, synthesizes a `ucontext` from the fault and calls
        `HandleGuestSignal(SIGBUS, BUS_ADRALN)`; on success it `setcontext()`s straight back into the
        fixed-up guest (NEON state captured by the service thread beforehand so `setcontext` restores
        it intact). Only if FEX cannot handle a fault does the trampoline log the full report (guest
        RIP + offset, registers, exact ARM64 words, last Win32 import breadcrumb) and `_exit`. Because
        the backpatch makes each site fault just once, this is self-limiting, not a per-access trap.
        Only the il2cpp probe installs it; the device-confirmed ubtest runner (`win32_device.cpp`,
        31/31) is untouched. If this clears the alignment faults, `il2cpp_init` should get materially
        further — possibly all the way.
      - **Fourth run: FEX's fixup works, the resume did not.** The log shows
        `BACHATA_UNALIGNED_FIX … instr_before=0xd9408294 instr_after=0xf8408294`: FEX rewrote the
        `LDAPUR` into a plain `LDUR` (unaligned-safe) plus a `dmb ld` barrier, and `HandleGuestSignal`
        returned true (`code=0x101` is `EXC_ARM_DA_ALIGN`, confirming the alignment diagnosis). But the
        fatal report followed at the same PC with no second `UNALIGNED_FAIL`: `setcontext()` had
        *returned*. On Apple arm64 the ucontext functions are stubs that fail with `ENOTSUP`.
        **Fix (this commit):** after a successful fixup the trampoline touches a marker address in
        `__PAGEZERO`; the Mach service thread recognises that second fault and has the kernel load the
        fixed-up thread state plus the NEON state captured at the original fault (`thread_set_state`),
        then resumes the guest. No libc context functions, no assembly.
      - **Fifth run (2026-10-28, iPhone 15, iOS 26.6.2): `il2cpp_init` succeeded.** 31 unaligned
        atomic/acquire sites faulted once each, were backpatched by FEX (`LDAPUR`/`LDAPR`→`LDUR`,
        `STLUR`→`STUR`, 8/4/2-byte, `adj=0` re-execute or `adj=-4` for the store half-barrier) and
        resumed through the kernel. Then `il2cpp_init returned 1`, `il2cpp_get_corlib = 0x158e30000`,
        `il2cpp_domain_get = 0x157c73fc0`, **0 unimplemented imports**: the metadata, corlib and root
        domain are up, the GC initialised, all on our mini Win32 + FEXCore on the phone.
        One harmless line: `unhandled exception 0x406d1388` is MSVC's thread-naming exception
        (`RaiseException(MS_VC_EXCEPTION)`); our dispatcher found no guest handler and continued, as a
        debugger would. Follow-ups, before the real game runs:
        - check whether that raise sat inside a `__try` our SEH dispatcher missed (or whether il2cpp
          simply relies on a debugger/vectored handler to swallow it);
        - ~~the Mach fixup path is single-fault-at-a-time~~ — done, see the next entry.
      - **The app still crashed after that success** (the log ended at `RESULT: il2cpp_init succeeded`).
        Cause: the probe then tore everything down — `SetThreadRunner(nullptr)`, `engine->Shutdown()`,
        `g_engine = nullptr`, the main guest thread's stack and TEB unmapped — while the threads
        il2cpp_init had started (`CreateThread` → a detached host thread running guest code: the GC
        finalizer, …) kept running guest code, and the GC holds the main thread's stack. **Fix:**
        after `il2cpp_init` nothing is torn down any more (engine, main guest thread, loaded image,
        thread runner live for the process; the probe runs once per app launch), guest threads are
        logged as they start/end, and the probe logs a proof of life 5 s and 30 s later.
      - **Fault handling moved into a shared module, per thread** (`engine/mach_fault.{h,cpp}`, used
        by `il2cpp_probe.cpp` and now also the ubtest runner `win32_device.cpp`): each registered
        host thread has its own saved context, trampoline stack and breadcrumb (no global state shared
        between faulting threads); fixups are serialised and remembered per site, so a second thread
        faulting on an instruction another thread already patched just re-runs it; the port rights each
        exception message carries are released; a thread faulting while another writes the report
        parks, so the report is never cut short.
      - **Sixth run (2026-10-28): the app stays up after `il2cpp_init`** (user-reported; the log with the
        guest-thread count and the 5 s / 30 s proof-of-life lines is still to be attached here).
- [ ] PE loader: sections, relocations, imports, TLS callbacks, `.pdata` registration.
- [ ] Mini Win32 for the imports and P/Invokes above; `baselib.dll` implemented natively.
- [ ] ARM64 → x64 thunks for the 228 `il2cpp_*`; x64 → ARM64 for callbacks.
- [ ] iOS test app built by GitHub Actions (macOS), JIT via StikDebug/StikJIT; game files copied onto the device by
      the user.

## M2e alternative: Madeira's Wine for TLS and exceptions (2026-10-27) [R]

The two missing pieces of the mini Win32 (implicit TLS and x64 exceptions) are not written in this
project for now. Checked instead whether Madeira (willfaust/Madeira, commit `e3a88fd`, GPL-3.0-or-later,
Wine fork LGPL) could supply them:

- Madeira runs Wine 11.4 built for **ARM64EC** inside one iOS process (wineserver as a thread), with FEX's
  ARM64EC module (`xtajit64.dll`) translating only the x86-64 code and DXMT/its own D3D12 for graphics.
  Wine's ntdll brings both missing pieces, already working with FEX-translated x64 code on iPhone.
- Released: 0.1.0–0.1.3 (October 2026) as ad-hoc signed IPAs; requires iOS 26+, JIT (StikDebug or its
  built-in StikJIT). It is a finished app: it runs Windows games as they are, Unity ones included
  (its docs mention Unity's own "Failed to load il2cpp" box as a known failure screen).
- **Using Madeira's Wine inside the hybrid** (only `GameAssembly.dll` in Wine, `libunity.so` native through
  Husk's translation layer) is possible in principle but deep: every host thread that calls into
  GameAssembly (Unity's main, render, job and loading threads, and the GC) would have to be a Wine thread,
  so Husk's `pthread_create` would have to create threads through Wine, and libunity's calls would cross
  through Wine's unix-call interface. Madeira's own build needs LLVM built for iOS, llvm-mingw and its
  unpublished-at-the-time forks; its BUILDING.md (2026-09-16) says a clean checkout did not yet build.
  Rough size: larger than the mini Win32 so far.
- **Shortest way to a playing game today:** run the Windows build in Madeira itself (UnityPlayer.dll and
  GameAssembly.dll both translated, D3D11 through DXMT). Slower than the hybrid would be, but
  everything already exists. EverPlay already points Windows Unity games to Madeira.

Next step for the hybrid: test Fellworld in Madeira on the iPhone. If it runs, the hybrid's only gain is
speed; if it stops (log), that tells which part needs work.

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
      - [x] **M3a, the il2cpp bridge ARM64 → x64** (`engine/il2cpp_bridge.{h,cpp}`): host test green,
        **[i] all checks pass on iPhone (2026-10-28, run below)**. `Il2cppBridge::Symbol(name)` returns a native ARM64 function
        pointer for any `il2cpp_*` export of the loaded `GameAssembly.dll` — what `dlsym("libil2cpp.so",
        name)` will hand libunity. One template-instantiated thunk per bound function (512 slots, no
        run-time code generation) forwards x0–x7 to rcx, rdx, r8, r9 + four stack slots and returns
        rax; the runner's `GuestCall` runs it on the calling host thread's own guest thread (created
        on first use, TLS attach callbacks run) or nested, when the call comes from inside an HLE
        handler. The 10 functions that take a host callback (`il2cpp_add_internal_call`, the memory,
        log, plugin, unitytls, GC-heap and debugger-transport hooks) are bound but return 0 with a
        log line unless overridden (M3c replaces 8 of them).
        Host test `il2cpp-bridge-test` (CI "Host tests", ASan/UBSan): drives native `ms_abi`
        functions through the thunks — all eight argument slots in order (four on the stack), short
        signatures, int32 results with junk above them, pointer results, stable pointers, missing
        exports, the blocked APIs, slot exhaustion, re-init.
        Device test in the il2cpp probe, after `il2cpp_init`, only through those function pointers:
        lists every assembly with its class count and the first classes of `Assembly-CSharp`, then
        runs managed code — `Environment.ProcessorCount` (managed → il2cpp icall → our
        `GetSystemInfo`, compared with the device's CPU count), `Int32.Parse("1234")` (string
        argument, boxed result), `"Fellworld on iPhone".ToUpperInvariant()` (instance call, string
        result) and `Int32.Parse("not a number")`, which must come back as `System.FormatException`:
        il2cpp throws managed exceptions as C++ exceptions, so this exercises our SEH dispatcher and
        unwinder with GameAssembly's own C++ frame handler.
        **Device run (iPhone 15, iOS 26.6.2):** through the bridge's function pointers only —
        56 assemblies, 6,039 classes (491 in `Assembly-CSharp`); `Environment.ProcessorCount = 6`
        (device: 6); `Int32.Parse("1234") = 1234`; `ToUpperInvariant` → `"FELLWORLD ON IPHONE"`;
        `Int32.Parse("not a number")` → `System.FormatException: Input string was not in a correct
        format.`, caught and returned — the first C++ exception of the real binary through our SEH,
        clean on the first try. Nine more unaligned sites were fixed up along the way. `BRIDGE: all
        checks passed`, 0 unimplemented imports, 1 il2cpp guest thread running, alive 5 s and 30 s
        later; the app stays up indefinitely after that (user-confirmed). After a fresh launch the
        ubtest runner, now with `MachFault`, still passes every check.
      - [x] **M3b, the InternalCall reverse thunks x64 → ARM64** (`engine/icall_bridge.{h,cpp}`,
        `engine/call_native_{arm64,sysv}.S`): host test green, **[i] confirmed on iPhone (2026-10-28, below)**. The game's
        x86-64 code calls engine functions that libunity provides as native ARM64 (AAPCS64);
        `IcallBridge::Make(fn, signature)` returns a guest x86 stub (through the new
        `Win32::Runtime::BindCallback`) that, called with the Microsoft x64 convention, re-lays the
        arguments for AAPCS64 and calls `fn`. Microsoft x64 gives each argument a positional slot;
        AAPCS64 fills x0–x7 and d0–d7 by two independent counters, so the thunk walks the arguments
        and, per the signature (`Int`/`Float` per arg + return), appends each to the next integer or
        floating register, then calls through a tiny trampoline (`ub_call_native`: loads the argument
        registers, returns the integer and floating results together). Up to 8 of each; the 16
        engine functions with more arguments (M1) and real stack arguments are not handled yet.
        Signatures are supplied explicitly here; M3c (below) derives them from the metadata.
        Host test `icall-bridge-test` (CI "Host tests", ASan/UBSan) drives native System V functions
        through the thunks: integer, pointer, 32-bit float and double arguments, their interleaving
        (independent register counters), integer vs floating returns, and six integer arguments (two
        off the Microsoft stack). Device self-test in the probe after `il2cpp_init`: registers native
        ARM64 stand-ins and calls their stubs through `CallWin64` with real Microsoft x64 placement
        (ints in rcx/rdx/r8/r9, floats in xmm0–3) — `AddInts`, `MixSum` (interleaved int/double,
        double return) and `ScaleFloat` (a true 32-bit float argument).
        **[i] on iPhone (2026-10-28): `ICALL: all checks passed`** — `AddInts(7,3,5)=50307`,
        `MixSum(1,2.0,3,4.0)=4321`, `ScaleFloat(2.5)=5`; both bridge directions confirmed in one run.
        Running the Windows DLL test in the same app session *after* il2cpp crashes: both runners share
        the mini Win32 (module list, TLS and unwind registries, thread runner, guest caller), and the
        ubtest runner starts a second FEX engine next to il2cpp's live one, then resets those globals
        while il2cpp's thread still runs. The product runs one game per process, so instead of
        supporting that: each runner refuses to start once the other has run in this process
        (`ub_il2cpp_probe_started`, `ub_win32_device_has_run`), and the app greys out the other button
        and says to reopen it.
      - [x] **M3c, InternalCall signatures from the metadata; the host-callback APIs intercepted**
        (`engine/il2cpp_intercept.{h,cpp}`, 2026-10-28): host test green (`il2cpp-intercept-test`,
        CI "Host tests", ASan/UBSan; also clean under clang), **[i] confirmed on iPhone (2026-10-28,
        commit `aa5b781`, run below)**.
        - `Il2cppBridge::Override(name, native)` makes `Symbol(name)` hand out a native replacement;
          `Il2cppBridge::Forwarder(name)` is an always-forwarding thunk to the real guest export (also
          for the refused host-callback names), which the replacements call.
        - **`il2cpp_add_internal_call`** is replaced: the registered name (`Ns.Class::Method`,
          `Ns.Outer/Inner::Method`, optionally with a `(Type,Type)` list, commas inside `<>`/`[]` not
          counted) is looked up through the metadata API (`il2cpp_domain_get_assemblies` → every image
          → `il2cpp_class_from_name`, nested types via `il2cpp_class_get_nested_types`, then
          `il2cpp_class_get_methods` filtered by name, the InternalCall impl flag `0x1000` and, if
          given, the parameter count). `il2cpp_method_get_flags` (static or not: `this` first),
          `_get_param`/`_get_return_type`, `il2cpp_type_get_type`/`_is_byref`,
          `il2cpp_class_is_enum`/`_enum_basetype`/`_is_valuetype` classify each type: by-ref,
          pointers, objects, strings, arrays, 32/64-bit integers → `Int`; `R4`/`R8` → `Float`;
          `bool`/`u1` → `UInt8`, `i1` → `Int8`, `char`/`u2` → `UInt16`, `i2` → `Int16`; enums by
          their underlying type; generic instances of classes → `Int`. Refused with a logged reason:
          value types by value, generic value types, generic parameters, more than 8 integer or 8
          floating arguments (AAPCS64 stack). Overloads with different ABIs are logged and the first
          is used. The result goes to `IcallBridge::Make`, and the stub to the real
          `il2cpp_add_internal_call`. Names not in the metadata (stripped) are counted, not logged.
        - Registrations before `il2cpp_init` (metadata not loaded) are kept; `il2cpp_init` and
          `il2cpp_init_utf16` are replaced too and register them once the real one returns success.
        - `IcallBridge` gained the narrow kinds `Int8/UInt8/Int16/UInt16`, sign/zero-extended in both
          directions (Microsoft x64 leaves the upper bits of a narrow argument or result undefined;
          Apple arm64 callees assume arguments extended to 32 bits), up to 16 arguments (8 + 8), and
          `FitsRegisters`, which `Make` checks.
        - **Host callbacks** (all integer/pointer, each wrapped once per function): `il2cpp_set_memory_callbacks`
          (the seven allocator functions, the wrapped table kept for the process),
          `il2cpp_register_log_callback`, `il2cpp_set_find_plugin_callback`,
          `il2cpp_gc_set_external_allocation_tracker`, `il2cpp_gc_set_external_wbarrier_tracker`,
          `il2cpp_gc_foreach_heap` and `il2cpp_gchandle_foreach_get_target` (callbacks declared with
          three integer slots, which covers both the public `(data, userData)` and Boehm's
          `(userData, start, end)` shape). **Still refused:** `il2cpp_unity_install_unitytls_interface`
          (a version-specific table of ~60 TLS functions, some with structs by value) and
          `il2cpp_register_debugger_agent_transport` (left out since M1). So 8 of the 10, plus
          `il2cpp_init*`. Open: the plugin path the find-plugin callback returns is an Android path.
        - Host test: a fake metadata API (results with junk above bool/int, as from MSVC) and native
          stand-ins; registration before/after `il2cpp_init`, int/float/bool/ushort/enum-in-nested-class,
          an instance method, a second image, overload by parameter list, the refusals and counters,
          the derivation alone, memory/log/heap-walk callbacks called back as Microsoft x64.
        - Device test in the probe after the M3b test (only through the bridge's function pointers,
          as libunity will hold them): registers native ARM64 functions as the real game's
          `UnityEngine.Mathf::ClosestPowerOfTwo`, `IsPowerOfTwo`, `GammaToLinearSpace`, `HalfToFloat`,
          `FloatToHalf`, `PerlinNoise` (each skipped if stripped from Fellworld's metadata) and the
          instance `UnityEngine.GameObject::get_activeSelf` (object from `il2cpp_object_new`, kept
          by a GC handle), runs each through `il2cpp_runtime_invoke` and checks our function was hit
          with the right values; then `il2cpp_gchandle_foreach_get_target` (must see that object)
          and `il2cpp_gc_foreach_heap` with an ARM64 callback.
        - **Device run (iPhone 15, iOS 26.6.2): `INTERCEPT: all checks passed`**, along with BRIDGE and
          ICALL in the same run. Signatures derived from Fellworld's real metadata:
          `Mathf::IsPowerOfTwo` `u8(i)` (64 → 1, 63 → 0), `GammaToLinearSpace` `f(f)` (1.5 → 4.5),
          `FloatToHalf` `u16(f)` (21 → 42), instance `GameObject::get_activeSelf` `u8(i)`, where
          our function got the managed object as `this`. Each ran managed wrapper → il2cpp icall
          lookup → our stub → AAPCS64. `ClosestPowerOfTwo`, `HalfToFloat`, `PerlinNoise` are stripped
          from this game's metadata and were skipped. `gchandle_foreach_get_target` called our
          ARM64 callback with `(target, userData)` and saw our object; `gc_foreach_heap` called it
          twice with `(section, userData, 0x40000)`, so userData is the second argument and a third
          (size) follows. The three integer slots covered that. 4 registered, 0 failed. 23 more
          unaligned sites were fixed up on the way; 0 unimplemented imports; alive 5 s later (the
          log ends there, as it was sent before the 30 s line).
        - Also: CI "Host tests" now also runs on changes under `engine/` (it did not before).
- [x] **M3d, Unity's Android engine under Husk's runtime, next to GameAssembly under FEXCore**
      **[i] reached on iPhone 15, iOS 26.6.2, 2026-10-28 (third run, commit `7ce09d3`)**
      (2026-10-28, in progress: CI "Probe IPA" green on the first try, run 40719413995, commit
      `22dd07c`: all of Husk's C/ObjC sources compiled and linked, IPA 0.86 → 1.58 MB;
      **[i] on iPhone pending**).
      - Husk's translation layer (`src/translation-layer{,-next}`, ~59k lines C/ObjC, GPL-2.0-or-later)
        is fetched at a pinned commit (`scripts/fetch-husk.sh`, Husk `4ac6552`) and compiled into the
        probe app unmodified, with Husk's own build settings (`app/project.yml`; `third_party/husk/PROVENANCE.md`).
        Not in the repository. Its loader core alone pulls in most of it (bionic's tables reach JNI,
        Dalvik, EGL, audio), so the whole C side is built; nothing from Husk's app (QEMU, ANGLE, UI).
      - Executable memory: Husk's loader asks for a StikDebug region by name
        (`husk_ios_jit_get_mapping`, which Husk's QEMU library exports); `engine/unity_host.cpp`
        provides it from the same BreakpointJIT allocator FEXCore uses, one 128 MiB region on first use.
      - **The virtual `libil2cpp.so`** (`app/Sources/unity_loader.c`): `dlopen`, `android_dlopen_ext`
        and `dlsym` of every Android library bind (Husk's `tl_ld_interpose`) to ours: `libil2cpp.so`
        by any path is our handle, `dlsym` on it is `Il2cppBridge::Symbol` (with the M3c overrides),
        a global `dlsym` of an `il2cpp_*` name too; everything else goes on to Husk's bionic.
      - The user copies `libmain.so` and `libunity.so` from Unity's Android player for the game's
        version (2021.3.18f1: `Editor/Data/PlaybackEngines/AndroidPlayer/Variations/il2cpp/Release/Libs/arm64-v8a/`)
        into `Files › On My iPhone › UB Probe › Unity`; the app writes `base.apk` there (stored zip,
        both libraries under `lib/arm64-v8a/`, data 16 KiB-aligned with Android's `0xd935` extra
        field; checked on the host with Python's `zipfile`: valid, CRCs, alignment).
      - Device step (after the il2cpp tests, same button): checks the virtual library
        (`il2cpp_domain_get` through `dlopen`/`dlsym`), then Husk's `tl_unity_start` with the APK:
        JNI and Husk's Java HLE world, `libmain.so` loaded and its `JNI_OnLoad`, then
        `NativeLoader.load` (libmain loads `libunity.so`: map, relocate, constructors, `JNI_OnLoad`).
        Logs every `libil2cpp.so` open/lookup and the unresolved imports. Stops there: the engine's
        own start (`initJni`, `nativeRender`) would run `il2cpp_init` a second time and needs EGL.
      - **First device run (2026-10-28, iPhone 15, iOS 26.6.2):** the il2cpp part all green as before,
        the APK written (25.7 MB), the virtual `libil2cpp.so` answered (`il2cpp_domain_get` through
        `dlopen`/`dlsym` = the domain). Husk's loader then refused to map anything: `the writable and
        executable views are 8161 MiB apart: too far for adrp`. The allocator places the region's
        writable alias anywhere (`VM_FLAGS_ANYWHERE`, here 8 GiB above the executable view); Husk
        retargets code at the writable view with `adrp` and needs them within ±3 GiB, and its own
        search (16 MiB steps out to 2 GiB) found nothing near: below the region the address space was
        taken, above it is the shared cache. **Fix:** `engine/unity_host.cpp` walks the address space
        (`vm_region_64`) within ±3 GiB of the executable view, logs the free gaps, and remaps the
        writable view into the nearest one that fits (128 MiB, shrinking to 48 MiB if needed); and
        the app requests Husk's region at the start of the run, before il2cpp_init fragments the
        address space, when `Unity/libunity.so` is present.
      - **Second run:** the region fix works (Husk's region taken at launch: rx `0x1137e8000`, rw 128 MiB
        above it, no move needed). Husk then **loaded, relocated and initialised `libmain.so`** (20
        relocations, 32 `adrp` and one `tpidr` patched), and libmain's `JNI_OnLoad` stopped at
        `FindClass("com/unity3d/player/NativeLoader")`: NoClassDefFoundError → FatalError → abort.
        Husk runs Unity's Java side for real in its own Dalvik interpreter and takes the classes from
        the APK's `classes.dex`, which our APK lacked. **Fix:** Unity's Android player ships a
        ready `Variations/il2cpp/Release/Classes/classes.dex` (DEX 035, 119 KB); the user copies it
        into the Unity folder with the two libraries, and the APK carries it (rewritten on every run).
      - **Third run: `UNITY: engine loaded (NativeLoader.load succeeded)`.** With `classes.dex`
        (120 classes) in the APK: libmain's `JNI_OnLoad` → `0x10006` (JNI 1.6); `NativeLoader.load`
        made libmain load **`libunity.so` (25.4 MiB): 54,766 relocations, 3,880 `tpidr` and 50,632
        `adrp` sites patched, 5,346 `x18` instructions rewritten, all 424 constructors run, its
        `JNI_OnLoad` called**; `NativeLoader.load -> 1`, **0 unresolved imports**. libmain opened
        the virtual `libil2cpp.so` itself (as on Android) and asked it for `JNI_OnLoad`, which
        GameAssembly does not export (NULL, harmless: on Android libil2cpp has none either worth
        calling here). In the same process: GameAssembly under FEXCore with il2cpp up and every
        BRIDGE/ICALL/INTERCEPT check passing. Husk's region at launch: rx `0x1174e8000`, rw 128 MiB
        above (no move needed).
      - Next (M3e): let libunity start il2cpp itself (probe without our own `il2cpp_init`), then
        `initJni`/first `nativeRender` without graphics (probe without our own `il2cpp_init`), then
        `initJni`/first `nativeRender` without graphics; the game's data as the APK's
        `assets/bin/Data`; ANGLE for EGL/GLES.
- [ ] **M3e, the engine starts il2cpp itself** (2026-10-28, built, **[i] on iPhone pending**). App
      button "Run Unity engine (libunity starts il2cpp)":
      - `ub_il2cpp_probe_set_engine_mode`: the probe loads GameAssembly, runs its TLS callbacks and
        `DllMain`, sets the data/config dirs, wires `Il2cppBridge`, `IcallBridge` and the intercept
        (`runtimeUp = false`, so InternalCalls registered before `il2cpp_init` wait and are made
        when the intercepted `il2cpp_init` succeeds), and stops: no `il2cpp_init` of ours, no tests.
        libunity's `il2cpp_set_data_dir`/`il2cpp_set_config_dir` (an Android `jar:` path GameAssembly's
        Windows file I/O cannot open) are redirected to the device's `<Game>_Data/il2cpp_data`.
      - The APK now also carries the game's `<Game>_Data` as `assets/bin/Data/` (without
        `il2cpp_data` and the Windows `Plugins`), streamed (CRC pass, then copy; any file size within
        zip32), rewritten only when the inputs (name, path, size, mtime) change. A `unity default
        resources` in the Unity folder replaces the game's Windows copy. Host-checked with Python's
        `zipfile` (nested folders, skips, a file larger than the copy buffer, the cache).
      - `pthread_create` of the Android libraries is interposed so each of Unity's threads registers
        with `MachFault` (fault reports instead of a silent exit).
      - Then Husk's `tl_unity_run`: `initJni`, `nativeInitWebRequest` and the helper classes, the
        UnityMain thread (`nativeRecreateGfxState`, `nativeResume`, `nativeRender` per frame), with
        `TL_FILE_TRACE`/`TL_DL_TRACE` on; the probe reports frames, the libil2cpp.so lookups and the
        InternalCall counts (registered / absent / not bridgeable) at 1, 5, 10 and 30 s. Expected
        stops: no EGL display (no ANGLE yet), the `StandaloneWindows64` platform tag in the data, the
        D3D11-only shaders (M4).
      - **First device run (2026-10-28):** the APK with the game's data (15 files, 77 MiB; APK
        107 MB), libmain/libunity loaded as in M3d, `initJni` done, UnityMain started,
        `nativeRecreateGfxState` and `nativeResume` returned, and the engine's own start-up ran:
        `MemoryManager: Using 'Dynamic Heap' Allocator`, `SystemInfo CPU = ARM64, Cores = 6,
        Memory = 11447mb`, `Version '2021.3.18f1 (3129e69bc0c7)' ... Scripting Backend 'il2cpp'`.
        **libunity opened the virtual libil2cpp.so and resolved 229 il2cpp_* functions** through the
        bridge (one miss: `JNI_OnLoad`). It then stopped with the dialog `Not enough storage space to
        install required resources` after `stat(.../base.apk/assets/bin/Data/Managed) -> -1`: an
        Android build carries il2cpp's data (`Metadata/`, `Resources/`) as `assets/bin/Data/Managed`
        and copies it to `files/il2cpp` before starting il2cpp; the Windows build has it in
        `<Game>_Data/il2cpp_data`, which the APK left out. Also missing (logged, not fatal):
        `assets/bin/Data/unity_app_guid`. No `il2cpp_init` yet, so 0 InternalCalls. **Fix:**
        `il2cpp_data` goes into the APK as `assets/bin/Data/Managed`, plus a generated
        `unity_app_guid`. Harmless on the way: OBB lookups (Java `ZipFile` not implemented by
        Husk), `nativeInitWebRequest` not registered, the engine's fault-signal handlers recorded
        but not installed by Husk.
      - **Second device run:** with `Managed/` in the APK the engine copied il2cpp's data to
        `files/il2cpp` (`Metadata/global-metadata.dat`, `Resources/mscorlib.dll-resources.dat`,
        `unity.ver`), then called `il2cpp_set_config_dir` and `il2cpp_set_data_dir` (both redirected
        to the device's `il2cpp_data`) and **started il2cpp itself**: GameAssembly's start-up ran as
        in M2f (the same unaligned-atomic fixups, il2cpp's first thread). Then a fatal
        `EXC_BAD_ACCESS` in native code of the app binary (`host_pc=0x102768a2c`, not a FEX guest
        thread), `fault_addr=0x328`: a function writing fields of a struct at `x0 = NULL`. The report
        had no names, so `engine/mach_fault.cpp` now adds the registers (x0-x28, fp, lr, sp), the
        PC and LR as `image!symbol+offset` (dladdr; Husk's `tl_ld_symbol_at` for the Android
        libraries) and the frame-pointer chain (read with `vm_read_overwrite`, so a broken frame
        ends the walk instead of faulting in the report).
      - **Third device run, named:** `pc = FEXCore::Context::ContextImpl::CreateThread+0x34`, on the
        host thread il2cpp started with `CreateThread` (its finalizer) right after the engine's
        `il2cpp_init` began: `new InternalThreadState` (an aligned allocation,
        `aligned_alloc(4096, 0x2000)` → the system `malloc`, FEX's own allocator is off on Darwin)
        returned NULL and FEX wrote through it. The same path never failed while only GameAssembly
        ran (M2f, M3a-c); with the Unity engine in the process the system allocator ran out:
        memory (the iOS limit) or address space. Unity also reports `Memory = 11447mb` on a 6 GB
        phone, so it may size its heaps for twice the RAM. Not guessed further: `MachFault::DumpMemory`
        (footprint, `os_proc_available_memory`, mapped address space, the 8 largest map entries
        with their VM tags) now goes into every fault report and is logged before the engine,
        after `NativeLoader.load`, before the engine's `il2cpp_init` and at 1/5/10/30 s.
      - **Fourth device run: not memory.** Footprint 276 MiB with 2795 MiB still granted, address
        space ample, at the same fault. FEXCore allocates its thread state through its own allocator,
        **rpmalloc** (`ENABLE_FEX_ALLOCATOR` is on for Darwin; `FEXAllocOperators::operator new` →
        `rpaligned_alloc`), which keeps a heap per host thread and sets it up on a thread's first
        allocation; a failed mapping would have logged `Failed to map VMA region.`, which did not
        appear, so the per-thread heap set-up returned nothing. Step taken: every new host thread
        that runs guest code (il2cpp's threads in `RunGuestThread`, the engine's threads in
        `BridgeCall`) now calls `FEXCore::Allocator::InitializeThread()` (`rpmalloc_thread_initialize`)
        and checks the heap with an allocation of the same shape (`aligned_alloc(4096, 8192)`) before
        FEX's `CreateThread`; if that still fails the thread is not started (logged) instead of FEX
        writing through NULL.
      - **Fifth device run:** `FEX thread heap ready` on both new threads and `CreateThread` got
        further, then the next rpmalloc allocation on the same thread returned NULL
        (`OpDispatchBuilder::OpDispatchBuilder+0x28` with `this = NULL`, from
        `ContextImpl::InitializeCompiler`), again with no map failure logged. rpmalloc maps one
        **256 MiB span, 256 MiB-aligned (512 MiB reserved), per thread heap and page type**; with the
        Unity engine in the process many host threads run guest code. **Fix:** FEXCore for iOS is
        now built with `ENABLE_FEX_ALLOCATOR=OFF` (`scripts/build-fexcore-ios.sh`): its internal
        allocations go to the system malloc through the Darwin branch AetherPS4's tree already has
        (`posix_memalign`, `malloc_size`); rpmalloc is no longer built. This changes every FEX run,
        so the earlier device checks (il2cpp probe: BRIDGE/ICALL/INTERCEPT; the Windows DLL test)
        are to be repeated with it.
      - **Sixth device run (both buttons): the app died silently right after loading GameAssembly**,
        before FEXCore's first JIT region, with no report (the fault catchers were only installed
        after the engine was created). The system branch of FEX's `AllocatorHooks.cpp` is written
        for glibc: Darwin's `aligned_alloc`/`posix_memalign` return NULL (EINVAL) for an alignment
        below `sizeof(void*)`, and `aligned_alloc` for a size that is not a multiple of it, and FEX
        asks for `alignof(T)` of 1, 2 or 4 (`fextl::vector<uint8_t>`, ...). **Fixes:**
        `scripts/build-fexcore-ios.sh` patches that branch at build time (idempotent, like the CMake
        hook; checked on a copy of the real file: three call sites, a second run changes nothing):
        alignment raised to at least 16 (a power of two), the size rounded up; and the il2cpp probe
        now installs the crash catcher and `MachFault` before `GuestEngine::Create`, so a fault in
        FEXCore's own start-up is reported.
      - **Seventh device run (2026-10-28): no crash on either button.** "Run il2cpp_init": BRIDGE,
        ICALL and INTERCEPT all pass again with FEX on the system malloc. **"Run Unity engine": the
        engine started il2cpp itself** (`il2cpp_init` through the virtual libil2cpp.so, 230 symbols
        looked up, 2 not exported: `JNI_OnLoad`, `end`), and **registered its InternalCalls through
        the M3c intercept: 1,341 registered with signatures from the metadata, 18 refused**
        (more than 8 arguments of one register kind, e.g. `Graphics::Internal_DrawMesh_Injected`,
        `Mesh::SetIndicesImpl`, `TextGenerator::Populate_Internal_Injected` with 23), 7,063 names
        not in Fellworld's metadata (stripped). `il2cpp_unity_install_unitytls_interface` refused
        as planned. Footprint ~250 MiB. Then, as expected, the engine refused the data:
        `globalgamemanagers` was `created for another build target ... File's Build target is: 19`
        → `Unable to initialize the Unity Engine`. **Fix:** the APK copy of every serialized file
        built for a desktop target (2, 5, 19, 24) now says 13 (Android): the field after the Unity
        version string in the metadata (header: 20 bytes before v22, 48 from v22; endianness from
        the header byte); the game's own files are untouched. Host-checked with synthetic headers
        (v22 LE, v17, v22 BE, an Android file and a JSON file left alone; zip CRCs valid). The APK
        input stamp got a format number so existing APKs are rewritten.
      - Open for the stack-argument InternalCalls (18): `ub_call_native` and `Signature` need
        AAPCS64 stack slots (as the 16 seen in M1).
      - **Eighth device run: the engine accepted the game's data.** 7 serialized files rewritten
        to build target 13; `globalgamemanagers` loaded (`Company Name: FellworldDev`, `Product
        Name: Fellworld`), il2cpp started by the engine and the 1,341 InternalCalls registered as
        before. Then `E/Unity: Unable to retrieve the default EGL display!` and the engine ended
        the process itself (no fault, no report): **the expected stop, M3e's goal reached** — the
        engine runs the game's start-up up to graphics initialisation.
- [ ] **M3f, graphics: EGL/GLES for the engine.** Husk gives the engine EGL and GLES through
      **ANGLE** (GLES over Metal), passed to `tl_unity_start` as `angle_egl`/`angle_gles` with a
      `CAMetalLayer`. Husk builds it from WebKit's ANGLE tree with its Xcode project
      (`scripts/build_angle_ios.sh` in Husk: sparse checkout of `Source/ThirdParty/ANGLE`,
      `Configurations`, `Tools/Scripts`, `Tools/ccache`, ~260 MB; `xcodebuild archive`, two passes
      for the entry-point names), no depot_tools/gn. Plan: build it the same way in CI "Probe IPA"
      (cached), embed `libANGLE-shared.dylib` in the probe app, give the run a Metal layer view,
      and see the first `nativeRender`. Then the D3D11-only shaders (M4) decide what is drawn.
      - **Built (2026-10-28), [i] on iPhone pending:** `scripts/build-angle-ios.sh` (after Husk's,
        WebKit pinned at `7e68a669`, `DEVELOPER_DIR` passed through `env -i` so the workflow's Xcode
        is used, deployment target 17.0) writes `build/angle/libANGLE-shared.dylib` (install name
        `@rpath/...`); CI "Probe IPA" caches it by the script's hash (`angle-ios-v1-...`), builds it
        only on a miss (job timeout raised to 180 min for that), embeds and signs it in the app's
        `Frameworks/`. The engine run passes it to `tl_unity_start` (`angle_egl`, found next to the
        executable with `dladdr`) with Husk's off-screen mode: no Metal view yet, the engine draws
        into a pbuffer and Husk saves frames 1-3 and then every 60th as `latest.bmp` into
        `Documents/Frames` (viewable in the Files app; they stay on the device). The run reports
        frames rendered and presented at 1/5/10/30/60 s.
      - **First device run with ANGLE:** ANGLE loaded, the EGL display was created (ANGLE over
        Metal), the engine got past the old stop. Then Unity opened a dialog "Warning: Your device
        does not match the hardware requirements of this application." and rendered 0 frames in
        60 s, no fault: it waits for the dialog's answer (Husk only logs dialogs).
      - **Next run, [i] pending:** a JNI HLE table (`g_dialog_hle` in `unity_loader.c`, registered
        before `tl_unity_start`) answers dialogs: `AlertDialog.Builder.setPositiveButton` keeps the
        listener, `show()` logs `DIALOG ...` and 500 ms later calls its `onClick(dialog, -1)` (OK).
        `TL_EGL_TRACE=1` logs EGL configs and contexts, to see which hardware check Unity fails
        (GLES version or config attributes suspected).
      - **Device run:** no crash; `DIALOG` logged and answered, but the listener is the engine's
        own `java.lang.reflect.Proxy` (from `ReflectionHelper.newProxyInstance`), so the plain
        `onClick` call went nowhere (`UNIMPLEMENTED ... Proxy.onClick`) and the engine kept
        waiting: 0 frames, `Frames` empty. The EGL trace shows the display and then nothing: no
        `eglChooseConfig` before the dialog, so the hardware check is not about EGL configs.
      - **Fix, [i] pending:** a Proxy listener is called through Husk's `tl_proxy_call`, i.e.
        Unity's native `ReflectionHelper.nativeProxyInvoke(handle, "onClick", [dialog, -1])`.
      - **Device run: past the dialog, first GLES context.** The click reached the engine (the
        dialog came a second time, OK again), then: `eglChooseConfig` (ES3 bit), off-screen
        surface 2556x1179, `eglCreateContext` (client version 2), `eglMakeCurrent` on UnityMain
        and UnityGfxDeviceWorker; `GL_RENDERER = ANGLE (Apple, ANGLE Metal Renderer: Apple A16
        GPU ...)`, `GL_VERSION = OpenGL ES 3.0 (ANGLE ...)`, the extension list. Then
        `E/Unity: The resource Internal-ErrorShader.shader could not be loaded from the resource
        file!` three times and a null-pointer read in libunity (fault address 0x38): **the APK had
        no `assets/bin/Data/unity default resources`** — a Windows build keeps it in
        `<Game>_Data/Resources/`, and Unity's Android copy was not in `Documents/Unity`.
      - **Fix, [i] pending:** the APK always gets `assets/bin/Data/unity default resources`:
        Unity's Android copy from `Documents/Unity` if there (GLES3 programs), else the game's
        Windows copy from `Resources/` (loads, but D3D11 programs only).
      - **Device run with Unity's Android copy:** no more shader errors; the engine loaded its
        built-in resources, reused its shader cache, started FMOD (`24000 Hz stereo`) and read
        `globalgamemanagers.assets`. Then FEX: `NoExec instruction in entry block` — guest code
        called `call rax` into libunity.so+0x613f1c, ARM64 code. That function fills
        `{count, max, buffer}` from the engine's unwinder: the `size_t (*)(Il2CppMethodPointer*,
        size_t)` the engine hands to **`il2cpp_override_stack_backtrace`**, an API not on the
        host-callback list, so it went to the guest unwrapped.
      - **Fix, [i] pending:** `il2cpp_override_stack_backtrace` is intercepted and not passed on
        (a host unwinder cannot walk the guest's x86 stack; il2cpp keeps its own). Also
        intercepted, with wrapped callbacks: `il2cpp_class_for_each`,
        `il2cpp_unity_liveness_allocate_struct` (both callbacks), and
        `il2cpp_unity_set_android_network_up_state_func`. All four are on the bridge's
        host-callback list. Host tests cover the refusal and the liveness wrap.
      - **Device run:** the backtrace override was refused as planned (`UB_IL2CPP_INTERCEPT ...
        not installed`) and the engine got further: it loaded the game's scene data, and now
        reports `Desired shader compiler platform 5 is not available in shader blob` for each of
        the game's shaders (5 = GLES3x; the game's shaders are D3D11 only, **as expected: M4**).
        Then `mm: mmap FAILED len=0x1fffc000 prot=0 flags=0x22 errno=12`, a 512 MiB address-space
        reservation by the engine refused with ENOMEM, and a null-pointer write in libunity right
        after (its allocator did not expect the failure). Address space mapped (without the
        kernel's reserved ranges at 0xfc0000000 and 0x1000000000): ~7 GiB before the engine
        (mostly the shared cache), ~12.5 GiB at the fault.
      - **Next, [i] pending:** the memory report now also walks the address space below the
        first kernel-reserved range: mapped/reserved/free totals, the largest free gap, how many
        gaps of 512 MiB or more are left, and the totals by VM tag. That shows whether the
        reservation failed for lack of a large enough gap (fragmentation, or a low ceiling for an
        app without the extended-virtual-addressing entitlement) and who holds the space.
      - **Device run: the address space is full.** Below the first kernel range:

        | when | mapped | of it reserved, no access | free in gaps | largest gap |
        |---|---|---|---|---|
        | before the engine | 7,146 MiB | 4 MiB | 1,071 MiB | 721 MiB |
        | after NativeLoader.load | 7,524 MiB | 259 MiB | 693 MiB | 292 MiB |
        | before il2cpp_init | 10,750 MiB | 3,006 MiB | 785 MiB | 230 MiB |
        | at the fault | 12,814 MiB | 3,984 MiB | 513 MiB | 207 MiB |

        Tag 32 (the shared cache) is 5,472 MiB throughout; tag 0 (plain mmap) grows from 1,210 to
        6,368 MiB, ~3 GiB of it reserved without access between `NativeLoader.load` and the
        engine's `il2cpp_init` (the engine's start-up). The highest mappings are near 0x3e3000000:
        the app's usable range ends around 16 GiB, not at 0xfc0000000, so ~8 GiB are left after
        the shared cache, and no gap of 512 MiB remains for the allocator's reservation.
      - **Next, [i] pending:** `TL_MM_TRACE` on (Husk logs every guest mmap; one of 64 MiB or more
        with the library function that asked), large `VirtualAlloc` reservations from the game
        side logged (`UB_WIN32 VirtualAlloc(...) N MiB`), the report reads the real end of the
        address space from `TASK_VM_INFO.max_address` and counts the space above the last mapping.
        The entitlement `com.apple.developer.kernel.extended-virtual-addressing` is requested;
        whether the signing grants it shows in that end address. `TL_EGL_TRACE` off again.
      - **Device run (signed with a free developer account in Sideloadly):** the address space
        ends at `0x458000000` (~17.4 GiB; the entitlement is not granted, as expected for a free
        account). The trace names the holder: **libunity's low-level virtual-memory allocator**
        (vtable at libunity+0x1804960, reserve at +0x36595c) reserves a 256 MiB region aligned to
        256 MiB per allocator (alignment hard-wired, `mov w8, #0x10000000`; ownership keyed by
        address >> 28) through Baselib's reserve (+0xeec74): `mmap(NULL, 0x1fffc000, PROT_NONE)`,
        then `munmap` of the edges outside the aligned window. Ten regions at start-up, more while
        running, sixteen (4 GiB) at the fault; the guest's mmaps net 4.6 GiB, the footprint is
        ~300 MiB. The seventeenth found no 512 MiB gap (880 MiB free in pieces). No large
        `VirtualAlloc` on the game side.
      - **Fix, [i] pending:** `mmap`/`mmap64`/`munmap` of the Android libraries are interposed in
        front of Husk's (`unity_loader.c`): a reservation of the form `2R - page` (R a power of
        two >= 64 MiB, anonymous, PROT_NONE, no address) gets the aligned window directly from
        `vm_map` with an alignment mask, so Baselib finds it aligned and skips the front unmap; the
        tail it then unmaps was never mapped and is ignored. Needs an aligned 256 MiB slot instead
        of a 512 MiB gap and leaves no trimmed holes. The report counts these reservations. If
        Unity wants more regions than ~8 GiB hold, the next lever is fewer or smaller regions.
      - **Device run:** the aligned reservations work (`13 aligned reservations (0 not)` after
        1 s, no trimmed holes), but the engine still wants more: at the fault ~16 regions (3,984
        MiB reserved), 856 MiB free in pieces, the largest 368 MiB, and no aligned 256 MiB slot
        left, so the fallback through Husk failed too (`mmap FAILED`, same null write). Thread
        stacks (tag 30) grew from 106 to 528 MiB while the engine ran.
      - **Next, [i] pending:** the Android side sees 2 cores (`TL_NCPU=2`, and a
        `sched_getaffinity` in front of Husk's, which ignores it; Unity read its big.LITTLE mask
        `0x3f` there): fewer job workers, so fewer threads, stacks and per-thread allocators with
        their regions. The report counts the threads the Android libraries start.
      - **Device run:** `Cores = 2` took effect (stacks before `il2cpp_init` 39 MiB instead of
        106; 20 threads after 1 s), but the engine still wants ~16 regions (3,985 MiB reserved at
        the fault, same crash). So the regions are per allocator, not per worker. The mm trace
        shows how little of them is used: summing the commits (`mprotect` rw) per 256 MiB region
        over three runs, **the highest committed offset is 1-16 MiB in every region, always from
        its start** (16 MiB = one allocator block). Also found: since the aligned reservations
        came from `vm_map`, Husk's `MADV_DONTNEED` (zero-fill, only for its own mappings) did
        nothing on them.
      - **Fix, [i] pending:** a region request gets its 256 MiB-aligned window with only the
        **first 64 MiB reserved**; the rest stays unmapped and is mapped in 16 MiB steps when the
        engine commits past the mapped end (`mprotect`, `vm_map` at a fixed address, which fails
        as ENOMEM if something else is there). `munmap`/`madvise` act only on the mapped part;
        `MADV_DONTNEED` zero-fills it. Other mappings of the Android libraries that land in a
        window's unmapped part are recorded at `mmap` and passed through untouched. 16 regions:
        1 GiB of address space instead of 4. The report counts regions and extensions.
      - **Device run: the address space holds.** 13 regions after 1 s with 64 MiB each reserved,
        none grown; at the fault 3,939 MiB free, largest gap 1,088 MiB, no `mmap FAILED`. **The
        out-of-address-space stop is gone.** New stop, in the game's code: `UB_WIN32 baselib:
        cannot open B` (il2cpp loading a native library: `Baselib_DynamicLibrary_OpenUtf16`),
        then `unhandled exception 0xe06d7363` (an MSVC C++ exception, il2cpp's form of a managed
        one) twice, then a null-pointer fault in GameAssembly+0x1bf5fd right after the
        `RaiseException` that should not have returned. C++ exceptions were caught before (M2f,
        `FormatException` through `il2cpp_runtime_invoke`); here the search found no handler.
      - **Next, [i] pending:** an unhandled exception (other than MSVC's thread-naming one) logs the
        frames the search walked (module offset, unwind info or not, handler, frame, why it
        stopped) and, for il2cpp's C++ exceptions, the managed exception's class and message; the
        failed library open logs the name's first bytes (to rule out a UTF-8/UTF-16 mix-up).
      - **First frame (user-confirmed, same run):** `Documents/Frames` held a frame: a plain pink
        image, Unity's error shader, which is what every material without a GLES3 program draws
        (the game's shaders are D3D11 only; M4). **The engine renders through ANGLE on Metal.**
      - The game's `Plugins/x86_64/` holds `lib_burst_generated.dll` (Burst-compiled x86-64 code,
        Windows, like GameAssembly.dll): likely the library il2cpp failed to open. Being x86-64 PE
        code, it can run under FEXCore like GameAssembly once the PE loader loads it as a module.
      - **Device run with the exception report:** not Burst. The managed exception is
        `System.EntryPointNotFoundException: Unable to find an entry point named 'BCryptGenRandom'
        in 'BCrypt.dll'` (.NET's random numbers / `Guid.NewGuid` P/Invoke it; the failed open of
        "B", bytes `42 00 00 00`, is il2cpp's first try at the name). Our `bcrypt.dll` was a
        built-in with no functions. The search walked 4 and 16 guest frames (`__CxxFrameHandler`
        frames among them, none catching this) up to `0x1031c0000 (no module, no unwind info)`, the
        bridge where libunity called into the game: the exception would have to cross host frames,
        which it cannot, so `RaiseException` returned and the code after it faulted.
      - **Fix, [i] pending:** `bcrypt.dll` gets `BCryptGenRandom` (system-preferred RNG or the RNG
        handle, `arc4random_buf`), `BCryptOpenAlgorithmProvider` ("RNG" only) and
        `BCryptCloseAlgorithmProvider`. Open: what to do with a managed exception that reaches the
        bridge boundary uncaught (on Android it would reach libunity's own C++ frames).
      - **Next device run:** the same `EntryPointNotFoundException` for `BCryptGenRandom`, frame for
        frame. The CI build of the fix had succeeded, but the log could not tell which IPA ran.
        Now the launch line carries the build (`commit …, Probe IPA run N`, a `build-info.txt`
        that CI writes into the bundle), and a function that `Baselib_DynamicLibrary_GetFunction`
        or `GetProcAddress` does not find is logged with its module.
      - **Device run (commit b081acd, run 47, the right build):** the BCrypt functions are there,
        but il2cpp never got to them: `cannot open B`, then `no function BCryptGenRandomW /
        BCryptGenRandom in (unknown module)`. So opening the library failed, and il2cpp went on
        as if it had not (no `DllNotFoundException`): the error state we set was not where it
        looked. `Baselib_DynamicLibrary_OpenUtf16` returns a `Baselib_DynamicLibrary_Handle` by
        value, and the evidence fits a hidden return slot in RCX (the "name" read from RCX was
        the caller's uninitialised slot, our error landed in the real name). Then
        `Baselib_SystemSemaphore_CreateInplace`, the same shape, has keyed every semaphore by the
        slot and never written the handle: il2cpp's baselib semaphores have been no-ops.
      - **Fix, [i] pending:** the three functions returning a handle by value detect the
        convention per call (a return slot is a local just above the guest's `rsp`; a semaphore's
        memory or a name is not), log which one they saw (first four calls), and handle both:
        the handle into the slot and its address in RAX, or the handle in RAX. `OpenUtf16` also
        opens built-in DLLs not yet used (`bcrypt.dll`). `ubtest.dll` checks both conventions.
      - **Device run (run 48):** the semaphores return their handle **in RAX** (so they were
        right before; the hidden-slot guess was wrong for them), and `OpenUtf16` still gets the
        name "B" (its convention line was not logged: the four lines were shared and the
        semaphores used them up). So il2cpp really passes "B" and ignores the error state; where
        the "B" comes from is open (our `MultiByteToWideChar`/`LCMapString` check out).
      - **Next, [i] pending:** a failed `OpenUtf16` logs RCX/RDX/R8, the call site in GameAssembly
        and the 32 bytes behind RCX and RDX; each function logs its convention on its own. And a
        robust way past it: `GetFunction` on a handle we never gave out looks the name up among
        the functions the built-in DLLs implement (`BCryptGenRandom` is unique) and logs that.
      - **Device run (run 49):** the fallback works (`BCryptGenRandom from an unknown handle 0:
        bcrypt.dll's`), the `EntryPointNotFoundException` is gone. (The open gets "B" followed by
        zeros, with `0x17` 24 bytes on: the open question stays.) Next stop: the GC stops the world
        with `SuspendThread` and `GetThreadContext`, both unimplemented, then `MessageBox "Fatal
        error in GC": ResumeThread failed` and a fault.
      - **Fix, [i] pending: SuspendThread/GetThreadContext for guest threads.** A thread stops only
        outside guest code. The device bridge reports every crossing (`GuestLeft` with the guest's
        registers at each import/InternalCall, `GuestReturning` before going back,
        `GuestEntering`/`GuestExited` around every `CallWin64`: thread start, bridge, nested and
        probe calls, TLS callbacks); returning or entering blocks while suspended, checked against
        the suspend count with sequentially consistent atomics. `SuspendThread` waits until the
        thread is outside guest code (2 s at most, then logs); `GetThreadContext` returns the
        registers of its last exit (integer and control), or its stack base when no guest frames
        are live. `DuplicateHandle(GetCurrentThread())`, how the GC registers a thread, now gives
        a real handle per thread (it returned the pseudo handle). `ubtest.dll` checks it (bit 29).
      - **Device run (run 50):** the GC's stop-the-world goes through (no unimplemented
        `SuspendThread`, no "Fatal error in GC"). Next: another P/Invoke, `ntdll.dll!
        NtQueryDirectoryFile` (.NET's `Directory`/`FileSystemEnumerator` on Windows), not there:
        `EntryPointNotFoundException`, uncaught up to the bridge, the same fault after it. (The
        failed opens show what reaches `OpenUtf16`: a buffer whose first character is right and
        the rest is not a string: still open, the by-name fallback covers it.)
      - **Fix, [i] pending:** `NtQueryDirectoryFile` on a directory opened with `CreateFileW`
        (`FILE_FLAG_BACKUP_SEMANTICS`): classes 1, 2, 3 and 12, entries 8-byte aligned and
        chained, `ReturnSingleEntry`, `RestartScan`, a search pattern, an entry that does not fit
        kept for the next call, NT status codes. `ubtest.dll` imports it from its own `ntdll.def`
        and checks listing, end, restart with pattern and a wrong class.
      - **Device run (run 51): the engine renders.** `NtQueryDirectoryFile` resolved (by name, as
        BCrypt), and then **`after 5 s: 500 frames (498 presented)`**: the engine draws ~100
        frames a second through ANGLE on Metal (pink: the game's shaders are D3D11 only, M4).
        Then `System.MissingMethodException: UnityEngine.Mesh::SetArrayForChannelImpl(...)`: one of
        the 18 InternalCalls never registered because they need AAPCS64 stack arguments (more than
        8 integer or 8 floating arguments), uncaught up to the bridge, the same fault after it.
      - **Fix, [i] pending: InternalCalls with stack arguments.** `IcallBridge::LayOut` places the
        arguments past 8 of a kind on the stack the way AAPCS64 does (libunity is an Android
        library: one 8-byte slot each, in argument order, integer and floating alike, narrow
        integers extended), up to 32 arguments and 24 slots; `ub_call_native_stack`
        (`call_native_arm64.S`) puts them at the callee's sp, 16-byte aligned, and frees them
        through x29. The host build has no such layout to run it with: host tests check `LayOut`
        (24 alternating Int/Float arguments, a narrow one on the stack), the 33-argument refusal,
        and the derivation of the 9-argument `Mathf::Nine`, which now registers.
      - **Device run (run 52): 60 s without a crash, the game's main menu starting.** All 1,359
        InternalCalls registered (`0 not bridgeable`); 510 frames after 5 s, 651 after 10 s,
        1,205 after 30 s, 2,029 after 60 s (~27 a second after the first burst), all presented;
        17 regions, none grown; `UNITY: engine running`. The game's own C# logs: `game manager
        is online`, `Instantiating Event`, `Event state change of Event mainMenu is initiated`.
        **M3f's goal is reached: the engine runs the game and draws** (pink, M4).
      - **The "B" explained:** the same run shows `UnityPlayer.loadLibrary` failing for `libB.so`
        and `libn.so`. il2cpp asks the engine's find-plugin callback (`il2cpp_set_find_plugin_
        callback`) for a P/Invoke library, and the Windows GameAssembly passes UTF-16 where
        Android's libunity expects UTF-8 (it read "BCrypt.dll" as "B") and returns UTF-8 that
        il2cpp then reads as UTF-16 (the "B" `OpenUtf16` got). **Fix, [i] pending:** the callback
        gets an adapter (a stub of our own, not the generic integer thunk) that converts the name
        to UTF-8 for the engine and its answer back to UTF-16, kept valid for the process; host
        test. The by-name `GetFunction` fallback stays for answers that are Android paths.
        `TL_FILE_TRACE` is off (the engine polls the CPU frequency files every frame: 7,500 of
        8,200 log lines).
      - **The frame (user-checked):** `Documents/Frames/latest.bmp`, ~9 MB (2556x1179, 24-bit, the
        full surface), plain pink all over. Expected with URP: the final blit to the surface is a
        shader pass too, D3D11-only like the rest, so the error shader covers the whole frame;
        whether menu geometry is drawn below it cannot be seen until M4 gives the shaders GLES3.
- [ ] Synthetic APK from the user's Unity Android package + game data; platform field rewrite.
- [x] Prerequisite from M2e: TLS and exceptions in the mini Win32 — done, `il2cpp_init` succeeds on iPhone (M2f).
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

### M4a: the compiled-shader formats of 2021.3, decoded (2026-10-29) [R]

Reference corpus, local only (scratchpad, never in the repo or CI): `unity default resources` of
Unity 2021.3.18f1's own players. The **Windows** player's copy carries every platform of the built-in
shaders (`platforms` 4 D3D11, 5 GLES2, 9 GLES3x, 14 Metal, 15 GLCore, 18 Vulkan), so the same shader
is there as D3D11 and as Unity's own GLES3 translation: a pair per variant to test M4b against; the
Android copy has 5, 9, 18. Reader: `tools/research/shader_blob.py` (UnityPy for the serialized part;
the record layouts after USCSandbox, GPL-3.0, github.com/nesrak1/USCSandbox).

- **Per platform** one LZ4 blob (`offsets`/`compressedLengths`/`decompressedLengths`, one segment
  each here): `int count`, then `count` x `(offset, length, segment)`, then the records.
- **Per pass and stage**, `m_PlayerSubPrograms` (4 groups, one filled here) lists every platform's
  subprograms one after another (type, keyword indices, `m_BlobIndex` into that platform's blob,
  requirements); `m_ParameterBlobIndices` runs alongside with the parameter record of each. A
  subprogram's platform follows from its GPU program type (DX11* 4, GLES3 9, ...).
- **Code record** (version 202012090): type, 4 stats, keywords (`int n`, strings), code (`int len`
  + bytes, 4-aligned), bind channels (`sourceMap`, `int n`, pairs). D3D11 code is a Unity header (6
  bytes, + 0x20 when its first byte is >= 2: 38 bytes here) then **DXBC with only ISGN/OSGN/SHDR**
  (no RDEF). **GLES3 code is one HLSLcc source for both stages** (`#ifdef VERTEX` ... `#ifdef
  FRAGMENT`, `#version 300 es`), on the vertex subprogram only; the fragment stage has no GLES3 entry.
- **Parameter record**: version, `int 1 + nCB`, a base (unnamed) cbuffer then the cbuffers, each
  `name, size, int n` x `(name, type, rows, columns, isMatrix, arraySize, offset)`, `int nStruct`; then
  `int n` typed bindings `name, kind` + kind 0 texture `(register, sampler, dim<<1|msaa)`, 1 cbuffer /
  2 buffer `(register, arraySize)`, 3 UAV `(register, original)`, 4 sampler `(bind, sampler)`.
  Textures shared by all variants may sit in the pass's `m_CommonParameters` instead (D3D11 pixel
  shaders here), names through `m_NameIndices`.
- **GLES3 differs from D3D11 in its tables**: `$Globals`, `UnityPerDraw` and `UnityPerFrame` are
  flattened into one `$Globals` of plain uniforms (offsets by declaration order: e.g.
  `unity_ObjectToWorld@0, unity_MatrixVP@64, _MainTex_ST@128, _Color@144`), while e.g.
  `UnityStereoGlobals` stays a uniform block (`UNITY_BINDING`); textures are listed per program with
  their unit. So a GLES3 table is derived from the translated source, not copied from D3D11's.

### M4b: translation with HLSLcc and a synthesized RDEF (2026-10-29)

- `scripts/fetch-hlslcc.sh`: HLSLcc (Unity-Technologies, MIT) at `3ea1fcd6`, into `build/hlslcc`, built
  unmodified (`third_party/hlslcc/PROVENANCE.md`); the host CMake builds it as a static library when
  it is there (CI's host tests do not fetch it).
- `engine/shader_xlat.{h,cpp}`: `BuildRdef` writes a D3D11 reflection chunk from Unity's tables
  (cbuffers with variables and type descriptors, bindings for cbuffers, textures and their samplers;
  half is float in a D3D11 cbuffer, precision comes from the bytecode's min-precision markers),
  `WithRdef` rebuilds the DXBC container with it, `Translate` runs HLSLcc on the vertex and the pixel
  program with a shared `GLSLCrossDependencyData` (matching varyings) and joins them in Unity's layout
  (`#ifdef VERTEX` ... `#ifdef FRAGMENT`), collecting HLSLcc's reflection callbacks (`OnConstantBuffer`,
  `OnConstant`, `OnTextureBinding`, `OnInputBinding`): the GLES3 parameter table's contents.
- Unity's flattening of `UnityPerDraw`/`UnityPerFrame` into `$Globals` in the built-in shaders happens
  in its HLSL headers (`CBUFFER_START` empty for legacy GLES3), before HLSLcc; from D3D11 bytecode the
  cbuffers stay uniform blocks, which is what URP's GLES3 shaders (SRP Batcher) use anyway.
- `tools/shader_xlat/main.cpp` (`ub-shader-xlat <case>`) and `tools/research/export_xlat_cases.py`: 51
  cases from the corpus (D3D11 VS + PS of each variant, Unity's tables, Unity's GLES3 source as the
  expected output), local only.
- **Result (run with the user's permission): all 51 cases translate**, and the output matches Unity's
  own GLES3 programs instruction for instruction in naming and structure (`hlslcc_mtx4x4...`,
  `in_POSITION0`, `vs_TEXCOORD0`, `UNITY_LOCATION(0) uniform sampler2D _MainTex`, `SV_Target0`, the
  `HLSLCC_ENABLE_UNIFORM_BUFFERS`/`UNITY_BINDING` wrappers). `GLOBAL_CONSTS_NEVER_IN_UBO` keeps
  `$Globals` plain uniforms as Unity does. What differs, by design: precision (Unity's `mediump` came
  from `fixed`/`half` in its HLSL; desktop DXBC has no min-precision, so highp), `UnityPerDraw`/
  `UnityPerFrame` stay uniform blocks (see above), and the register allocation of code compiled for
  D3D11 (equivalent). HLSLcc's `OnConstant` reports offset 0 for every member: the GLES3 table takes
  the offsets from the D3D11 tables (std140 and D3D11 packing agree for Unity's vec4-based layouts).
- Neither file carries type trees (`enableTypeTree` false, as in player builds): rewriting a Shader
  object (M4c) needs the 2021.3 layout of class 48 built in.

### M4c: GLES3x platform into the Shader objects (2026-10-29, done on host)

- **Prototype** `tools/research/add_gles3.py` (UnityPy writes through the 2021.3.18 type tree it carries,
  2,304 nodes for class 48): per pass and D3D11 keyword variant (VS and PS matched by keyword set) it
  translates with `ub-shader-xlat`, writes a GLES3 code record (type 4, stats 0, the D3D11 VS's
  `sourceMap`, no bind channels) and a parameter record (empty base, `$Globals` with the plain
  uniforms, one cbuffer per uniform block with a cbuffer binding at HLSLcc's binding point, textures
  with HLSLcc's unit; members and sizes from the D3D11 tables), packs them as a platform-9 blob (an
  LZ4 block of literals: valid for any LZ4 decoder), and appends the subprogram entries (type 4, the
  D3D11 VS's keywords and requirements, blob and parameter indices) to `progVertex`'s groups.
- **Check on the corpus**: Unity's own GLES3 platform removed from the Windows `unity default
  resources`, ours added: 51 programs in 6 shaders, none failed; the file reads back with UnityPy and
  its tables have Unity's layout.
- **C++ port** (what the device runs), host tool `ub-shader-convert <in> <out> [--replace]`:
  - `engine/unity_file.{h,cpp}`: serialized file format 22 without type trees; keeps the metadata it
    does not need as bytes, rebuilds with replaced objects (later objects move by the size change
    rounded to 8, gaps kept, same data offset, header file size updated).
  - `engine/unity_typetree.{h,cpp}` + `engine/typetree_shader_2021_3.inc` (generated by
    `tools/research/gen_typetree.py` from UnityPy's type database): generic type-tree reader/writer.
  - `engine/shader_gles3.{h,cpp}`: LZ4 block decoder, literal-only encoder, the transform of
    `add_gles3.py` with `ShaderXlat` in-process. A Shader is only rewritten if its bytes read and write
    back identically (guards against a different 2021.3 layout); files of other Unity versions are left.
- **Checks**: `ub-shader-convert --roundtrip` on both corpus files: 85 objects, 6 shaders, all
  byte-identical, and the rebuilt file is identical too. Converted with `--replace`: 51 programs in 6
  shaders, none failed, and UnityPy reads every object (Shader trees and decompressed blobs) of the
  result exactly as in the Python prototype's output.
- Next (M4d): HLSLcc and these sources into the iOS build, conversion of the game's files in
  `make_apk` (cached), test on the iPhone.

### M4d: conversion on the device (2026-10-29, built, waiting for the iPhone)

- `scripts/build-fexcore-ios.sh` fetches HLSLcc (`scripts/fetch-hlslcc.sh`, pinned) and builds it in
  FEX's project as `libhlslcc.a` (hook v2; a FEX checkout with the old hook must be deleted), the
  shader sources go into `libunitybridge_engine.a`; the app links every `.a` of the output.
- `engine/shader_import.{h,cpp}`: `ub_shader_import(src, dst, log)` maps the file, reads only the
  metadata first (no class 48 type: nothing to do), then `ConvertFile` without `--replace` (shaders
  with a GLES3 platform stay as they are) and writes the copy.
- `make_apk` (`app/Sources/unity_loader.c`, `convert_shaders`): every game file without extension or
  with `.assets` (not `Managed/`) goes through it; a converted copy lives in
  `Documents/Unity/shaders/<name>` and the APK entry points at it (the build-target rewrite then
  applies to the copy). A stamp `<name>.src` (converter version, source path, size, mtime, result)
  skips the work on later runs, also for files with nothing to convert. The log has a line per file
  converted: shaders, programs, failures (first reasons).
- Host check: `ub-shader-convert --import` on the Windows `unity default resources` with Unity's
  GLES3 removed: 6 shaders, 51 programs, none failed, same content as the reference; Unity's
  Android copy and the original Windows copy: nothing to convert; a non-Unity file: skipped.
- CI run 57 (`3a08365`) green: `libhlslcc.a` and the shader sources built for iOS and linked into the app.
- Next: on the iPhone: the first run converts (log lines `shaders:`), frames should
  show the game instead of pink.
- **iPhone, run 57** (2026-10-29): `globalgamemanagers` skipped (no shaders), then the app ended during
  `resources.assets` without a crash report. Most likely a stack overflow: the probe runs on a
  `Thread.detachNewThread` thread (512 KiB stack) and HLSLcc recurses over the control flow of
  programs much larger than Unity's built-in ones (the host checks ran on the 8 MiB main thread).
  Fix: `ub_shader_import` runs the conversion on a thread of its own with a 64 MiB stack, and logs a
  line per Shader object (`reading`) and per shader (`translating`) before the work, so a crash
  can be placed from the log.
- **iPhone, run 58**: the 64 MiB stack did not help: the log ends at `TextMeshPro/Sprite: translating`,
  a small shader (5,568 bytes), so not the stack. HLSLcc dereferences a binding it does not find in
  the RDEF (`GetShaderVarFromOffset` on a missing cbuffer, a missing sampler at `sample` with a
  separate sampler register, ...), and our tables only took the per-variant blob's cbuffers and
  textures plus the common textures. Host-thread faults left no report because only guest threads
  were registered with the Mach fault handler (StikDebug gets the rest).
- Fix (converter version 2):
  - `ReflectionOf` also takes separate samplers (binding kind 4, `m_CommonParameters.m_Samplers`) and
    the common constant buffers (`m_ConstantBuffers` with `m_ConstantBufferBindings`).
  - `ShaderXlat::CompleteReflection` reads the program's declarations (dcl_constantbuffer,
    dcl_sampler, dcl_resource) and adds stand-ins for what the tables lack: a cbuffer as one float4
    array `UnityBridge_cb<n>_data`, a sampler `UnityBridge_s<n>`. A texture the tables lack, UAVs,
    buffer resources and SM 5.1 fail that variant cleanly ("not translated: ...") instead of
    crashing. `ub-shader-xlat` runs it too.
  - The conversion thread registers with `MachFault`, so a fault there gets a report with symbols.
  - Host: the corpus result unchanged (0 stand-ins needed); a case with UnityPerDraw and the sampler
    removed from the tables translates with stand-ins; one with the texture removed fails cleanly.
- **iPhone, run 59**: now a fault report: `ToGLSL::TranslateVariableNameWithMask`, load from 0xc (a null
  `ShaderVarType`'s `Columns`): `GetShaderVarFromOffset` found no variable at an offset the program
  reads, in a cbuffer that is there. Reproduced on the host with the run-59 code and a case whose
  table lacks two variables the program reads (same function, same address 0xc).
- Fix (converter version 3): `CompleteReflection` also scans the instructions' operands (opcode and
  extended tokens, nested relative indices, immediates) for constant buffer reads `cb<n>[i].xyzw`
  and checks each component against HLSLcc's own lookup rule (`IsOffsetInType`, matrices as vec4
  arrays); a component without a variable gets a scalar `UnityBridge_cb<n>_<byte>`. The corpus
  needs none (output unchanged); the mutated case translates.
- **iPhone, run 60**: the conversion went through every file: `resources.assets` 4 programs,
  `sharedassets0.assets` 17, `unity_builtin_extra` 209, `globalgamemanagers.assets` 220 (2 of
  URP's UberPost not translated: "malformed code record"); then the engine started and swapped
  frames. Three findings:
  1. **Unity runs GLES2.** `Desired shader compiler platform 5 is not available in shader blob` (35
     times, in every run since the first frames): platform 5 is GLES20, although ANGLE gives an
     OpenGL ES 3.0 context. The Windows build's `BuildSettings.m_GraphicsAPIs` lists only desktop
     APIs; the Android player finds none of them, shows "Your device does not match the hardware
     requirements" (the dialog every run had) and falls back to GLES2, so our GLES3x programs are
     never asked for. Fix: the import also rewrites BuildSettings (class 141, type tree generated
     like class 48's: `engine/typetree_buildsettings_2021_3.inc`) to `m_GraphicsAPIs = [11]`
     (OpenGLES3) when it names no Android API. Checked by `tests/host/unity_assets_test.cpp`
     (synthetic object, round trip, layout identical to UnityPy's writer) since the game's file is
     not here.
  2. **Every program needed stand-ins**, also Unity's own cbuffers (UnityPerDraw, ...): the game's
     per-variant tables carry no cbuffer bindings, so every cbuffer got register 0. Fix: registers
     also from `m_CommonParameters.m_ConstantBufferBindings` (by name); a cbuffer bound nowhere is
     left to CompleteReflection's stand-in instead of register 0. Converter version 5.
  3. **Crash**: il2cpp asked GetProcAddress for `BCryptGenRandomW` first; our built-in bcrypt.dll
     handed out a stub for any name, and the unimplemented stub returned 0 (STATUS_SUCCESS) without
     random bytes: all-zero GUIDs, so .NET's case-sensitivity probe file
     `CASESENSITIVETEST00000000000040008000000000000000` from an earlier run already existed, the
     IOException went unhandled across the bridge and the process faulted. Fix: GetProcAddress /
     Baselib_DynamicLibrary_GetFunction on a built-in DLL return NULL for functions we do not
     implement, as Windows does, so il2cpp falls back to `BCryptGenRandom`.
- **iPhone, run 62**: `BuildSettings: graphics APIs [2] -> [OpenGLES3]`, and no more "platform 5"
  errors or the hardware-requirements dialog: the engine now takes the GLES3 path. BCrypt works
  (`no function BCryptGenRandomW`, then the real one). The game reached `mainMenu` again, then
  P/Invoked into ntdll and died on a managed NullReferenceException thrown across the bridge.
  - Likely cause: .NET's file enumeration calls `ntdll!RtlNtStatusToDosError` for any status other
    than success / no-more-files; we had none, and since run 62 GetFunction returns NULL for it (il2cpp
    turns the call through 0 into a NullReferenceException). Added `RtlNtStatusToDosError`, a trace of
    the first 24 `NtQueryDirectoryFile` calls (path, class, status, bytes) and a line per function
    Baselib resolves.
  - Stand-ins were still needed everywhere (`UnityPerDraw: 16 components without a variable` = one
    float4x4): the game lists each cbuffer in the variant's table by name only, the variables are in
    `m_CommonParameters.m_ConstantBuffers`, which were skipped when a same-named buffer existed. Now
    merged into it. Converter version 6. The corpus has no common cbuffers (checked), so this is
    for the device to show.
- **iPhone, run 63: first real frame [i].** `Frames/latest.bmp` shows Unity's splash screen ("Made with
  Unity", early in its fade-in, so dark): drawn with our translated GLES3 shaders, no longer pink.
  Every program now translates without stand-ins (shared cbuffer variables merged), 450 programs in
  5 files, 2 of URP's UberPost not translated ("malformed code record", to look at).
  `NtQueryDirectoryFile` works (the empty `Saves` folder: ".", "..", then no more files).
- Then the same managed NullReferenceException as in run 62 (same frames), unhandled across the
  bridge. On Windows il2cpp's catch blocks (`__CxxFrameHandler`, module+0x222ca0, on frames 5-15)
  would take it and Unity would log it and go on. Our dispatcher never found a handler in
  GameAssembly on the device: also `0x406d1388` (thread names, il2cpp's own `__try/__except`) was
  reported unhandled in every run. The host tests only exercise our own `__C_specific_handler`, not
  GameAssembly's statically linked MSVC handlers. Added a trace (`UB_SEH`) of the first 4 C++
  exceptions: every handler called with its disposition, and every RtlUnwindEx.
- **iPhone, run 64 (UB_SEH trace)**: GameAssembly's `__CxxFrameHandler` does find the catch: frame 5's
  handler calls `RtlUnwindEx(frame, +0xd59b9c, consolidate)` at once. But it then returns 1
  (ContinueSearch) with no resume pending, and the same repeats on frames 6..15: the resume was
  *applied* inside the language-handler callback instead of escaping to the RaiseException call
  (the guest then ran the managed code after the catch on the handler's callback and came back
  with a stray "1"). The resume root is `t_dispatchRootDepth` only while `t_dispatchActive`; the
  bridge's TakePendingResume/TakeResumeInto cleared that flag on every applied resume, so anything
  resumed while the catch funclet ran (RtlUnwindEx's consolidate callback) left the outer dispatch
  "inactive", and its resume rooted at the RtlUnwindEx call itself. Fix: the flag is only
  DispatchScope's (no more clearing on apply), and UnwindEx fixes its resume root on entry, before
  the consolidate callback runs guest code. The trace now also logs each resume request and the
  bridge's apply/escape decision.
- **iPhone, run 65**: no crash any more, but no catch either: the trace shows the resume requested
  with the right root (`root depth 1 (now 2)`), yet the handler came back with none pending and no
  apply/escape decision logged, and the engine stalled after 425 frames. Cause, also of run 64:
  `t_resumeByBridge` was `thread_local` and set only on the probe thread; the engine's threads
  (UnityMain calling il2cpp through the bridge) took the host-test path, where RtlUnwindEx moves
  the resume into CallContext fields the device bridge never reads, and clears it. Now a process
  flag. (Run 64's fix of the resume root stays: it is right, but it was not what lost the catch.)
- **iPhone, run 66: managed exceptions are caught now.** `UB_SEH` shows 24 resumes requested at
  root 1, escaped from depth 2 and applied at depth 1; Unity logs each exception with its managed
  stack trace and carries on (frames 421 at 5 s, 541 at 10 s). Two problems left:
  1. Every frame: `NullReferenceException at UnityEngine.Rendering.Universal.Internal.PostProcessPass.Render`
     (233 times). Likely the 2 UberPost variants that did not translate ("malformed code record"):
     a large shader's platform blob is split into several LZ4 segments (one offsets /
     compressedLengths / decompressedLengths entry each), and a record table entry
     `(offset, length, segment)` points into its segment; we decompressed segment 0 only (as
     UnityPy and USCSandbox do). Now all segments are decompressed and records read from theirs.
     Converter version 7.
  2. After ~20 s: a write fault in libunity at `0x323e04038`. Unity's allocator committed 2 MiB at
     `0x323e04000`, crossing the 64 MiB head of the window at `0x320000000`; the commit reached the
     kernel unextended and failed (ENOMEM), with no "grown" and no "no room" line, so `region_of`
     did not take it for the window's. Suspected: a stale foreign-mapping entry (one only partly
     unmapped keeps its entry) over address space a window took later. Now entries overlapping
     newly reserved space are dropped, each growth is logged, and a refused commit logs which
     region / foreign entry it was taken for.
- **iPhone, run 67: Fellworld's main menu [i] (2026-10-29).** The frame shows the game's title screen
  rendered correctly (logo, text, the Continue / New Game / Load Game / Settings / Quit buttons):
  the Windows game's IL2CPP code under FEXCore, Unity's Android engine, D3D11 shaders converted to
  GLES3 on the device, ANGLE over Metal. 60 s without a crash, 1941 frames (1939 presented), no
  window growth needed. All 222 programs of `globalgamemanagers.assets` translate now (blob
  segments), and the PostProcessPass null reference is gone with UberPost.
- Left in the log: `TextMeshPro/Distance Field` does not link ("Field numbers of uniform block
  'UnityPerCamera' differ between VERTEX and FRAGMENT shaders"): Unity's tables list each stage's
  used variables only, and HLSLcc declares a uniform block's members from the RDEF. A cbuffer both
  stages use now gets the union of both tables' variables, sorted by offset, in both stages (only
  when they differ; the corpus is unchanged). Converter version 8.
- **iPhone, run 68**: no link errors any more (TextMeshPro/Distance Field links), 60 s, 1866 frames.

### M5: the game on the screen, with touch (2026-10-29, built, waiting for the iPhone)

- The probe app only wrote frames to BMP files; the menu could be seen but not used. Now "Run Unity
  engine" opens a full-screen game view (`app/Sources/GameView.swift`): a UIView backed by a
  `CAMetalLayer`, turned to landscape (`requestGeometryUpdate`; Info.plist allows landscape), whose
  layer and size in pixels go to Husk (`tl_unity_config.metal_layer`, width, height) through
  `ub_unity_set_surface` before the engine starts; ANGLE presents into it (no BMP frames then).
- Touches go to the engine through `ub_unity_touch` → Husk's `tl_unity_touch` (pixels, y down, up to
  10 fingers, phase 0 down / 1 move / 2 up).
- One view (and layer) for the session, retained for good: a small × closes the game screen (the
  engine keeps running), "Show game" in the toolbar brings it back.
- **iPhone, run 69: the game is playable [i].** The game view shows the game in landscape, touches
  work: the user got from the main menu into character creation (the lab room, its text, the
  action buttons and the log panel). Some images are missing: the portrait panel, the bars under it
  and the map show as near-black shapes.
- Likely cause: textures in desktop formats. ANGLE over Metal on the iPhone offers ETC1/ETC2, ASTC and
  PVRTC, no S3TC/DXT nor BPTC/BC7 (the extension list in the log), and a Windows build compresses
  its textures as DXT1/DXT5/BC7 (Unity's own `unity default resources`: DXT1/DXT5 in the Windows
  copy where the Android copy has ETC_RGB4/ETC2_RGBA8). First step: a census of every file's
  Texture2D objects by format at import (`engine/unity_textures.{h,cpp}`, class 28's 2021.3 type tree
  generated like the others), logged as `shaders:   N textures: DXT5 .., BC7 ..`. Converter version 9
  (so every file is looked at again once). Next: decode the formats the census shows to RGBA32 (or
  re-encode) in the converted copies, including the data in `.resS` files.
- Also in the log: `MethodAccessException: ... ICollection<IEventSystemHandler>.Clear on
  List<TMPEffects.Modifiers.Rotation>` and "Could not load TMPEffectsSettings" (TMPEffects, a text
  effects package); to look at.
- **iPhone, run 70**: the user played: character creation, the lab, a fight ("Tragmaw has been
  incapacitated!"), 60 s+ at ~28 fps. A managed exception during the fight was caught (`UB_SEH`:
  resume at depth 2, Unity logged the MethodAccessException of TMPEffects and went on). The census
  did not show the game's files: they came from the import cache, and the census was only logged
  when a file was converted. Now each file's import summary (census included) is kept as the cache
  stamp's second line and logged on every start; converter version 10 rewrites the stamps once.
- The crash, ~2 min in, at "Loading area: Malith": three bridge calls into GameAssembly+0x16ade0
  failed in CallWin64 with stage Mapping / EFAULT, then libunity read through a null pointer
  (0x20). CallWin64 returns that for three different reasons (target not in guest code, guest stack
  too low, stack-argument slots not writable); each now logs which (`UB_FEX CallWin64(...)`), and
  the bridge's failure line names the GameAssembly offset, the thread, the HLE depth and whether the
  call nested on a running guest thread.
- **iPhone, run 71**: the census: Fellworld's textures are DXT5 (141 in sharedassets0, 6 in
  resources.assets), plus RGBA32/RGB24/Alpha8 (fine on GLES) and Unity's own DXT1/DXT5; no BC7.
  The crash on "Loading area: Malith" again: `stack arguments at 0x126c77fe0 not writable (nested,
  rsp 0x126c78078)` on UnityMain at HLE depth 1, with no exception dispatched before: most likely
  the 8 MiB guest stack of UnityMain's bridge thread ran out (a scene load nests engine -> managed
  -> engine calls on it). Bridge threads now get 64 MiB (pages are only used when touched); the
  failure line also names the bridge stack's range, to tell an overflow from a wrong rsp.
- **Textures decoded at import**: DXT1 (BC1) and DXT5 (BC3) Texture2D objects become RGBA32, every
  mip level (`UnityTextures::DecodeBlocks`, `ToRgba32`). Pixels kept in the object are replaced
  there; streamed ones (`m_StreamData` into a `.resS`) are read from the game's resource file and
  written, 16-byte aligned, to a new `<file>.ub.resS` next to the converted copy, which `make_apk`
  adds beside it; the objects point there (offset, size, path). The original files are not changed.
  Host checks: a DXT1 and a DXT5 block decode to the expected texels; Unity's Windows
  `unity default resources` converted: its two DXT textures (the splash cube, 2048x1024 with 12
  mips, and a 32x32 DXT1) match UnityPy's own decoding within 1 per channel. Converter version 11.
