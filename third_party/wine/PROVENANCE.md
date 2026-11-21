# Code adapted from Wine

UnityBridge's x86-64 structured-exception-handling code in `win32/ntdll_unwind.cpp` and
`win32/ntdll_exception.cpp` is adapted from [Wine](https://gitlab.winehq.org/wine/wine), tag
`wine-10.0` (commit `b073859675060c9211fcbccfd90e4e87520dc2c2`). No Wine source files are vendored
here; the logic was ported into UnityBridge's own files and structures (`win32/winnt_seh.h`).

| UnityBridge file | Adapted from (Wine) | What was taken |
|---|---|---|
| `win32/ntdll_unwind.cpp` | `dlls/ntdll/unwind.c` (x86-64 path) | `RtlVirtualUnwind2`'s UNWIND_INFO interpreter, `is_inside_epilog`/`interpret_epilog`, `find_function_info`, and `__C_specific_handler` |
| `win32/ntdll_exception.cpp` | `dlls/ntdll/signal_x86_64.c`, `dlls/ntdll/exception.c` | the exception dispatcher (`call_seh_handlers` → `DispatchException`), `RtlUnwindEx`, and `RtlRestoreContext`'s consolidate/longjmp handling |

## License

Wine is licensed under the GNU Lesser General Public License, version 2.1 or later
(`LICENSES/LGPL-2.1-or-later.txt`). LGPL-2.1-or-later is compatible with UnityBridge's
GPL-3.0-or-later: LGPL-2.1 §3 allows relicensing a covered work under the GPL (version 2 or later),
so the adapted code is redistributed here as GPL-3.0-or-later along with the rest of UnityBridge.

## Changes from Wine

The port keeps Wine's table-walking and handler logic but fits UnityBridge's model, where the
unwinder runs on the host over the guest DLL's `.pdata` while the guest's funclets run under FEXCore:

- Wine's `TRACE`/`FIXME`/`ERR` logging, the dynamic and growable function tables, the ARM64EC code
  paths, the Wine-internal `teb_frame` SEH list, and `KNONVOLATILE_CONTEXT_POINTERS` were removed.
- The structures come from `win32/winnt_seh.h` (laid out at the documented x64 offsets) instead of
  Wine's `winternl.h`/`winnt.h`.
- `__except` filters, `__finally` termination handlers and consolidate callbacks are guest code, so
  they are invoked through `Win32::CallGuest` rather than a direct call.
- The one non-local transfer (`NtContinue`) is not a syscall that resets the CPU; it is recorded as
  a pending resume that every SEH function returns through to the outermost HLE handler
  (`RaiseException`), which applies it to the guest thread. There is no `setjmp`/`longjmp` across a
  guest call, which the device's nested FEXCore callbacks could not survive.
