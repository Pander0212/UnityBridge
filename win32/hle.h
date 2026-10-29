// SPDX-License-Identifier: GPL-3.0-or-later
// The Windows functions a loaded DLL imports, implemented natively ("mini Win32").
//
// Every import is bound to a small x86-64 stub of its own. The stub carries the function's id to
// Dispatch(): on iPhone through `syscall` into FEXCore's HLE bridge (GuestEngine), in host tests
// (x86-64 Linux) through a jump to an assembly entry that calls Dispatch() directly. Imports
// without an implementation get a stub too; calling one logs "unimplemented DLL!Name".
//
// Calls follow the Microsoft x64 convention: arguments in rcx, rdx, r8, r9 (or xmm0-3 for
// floating point), the rest on the stack above 32 bytes of shadow space; result in rax or xmm0.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace UnityBridge::Pe { struct Tls; }

namespace UnityBridge::Win32 {

// What a stub hands over: the argument registers and the stack pointer at the stub's entry.
// The first fields keep fixed offsets shared with win32/host_entry_x64.S (checked in hle.cpp); new
// fields are appended so those offsets do not move.
struct CallContext {
  uint32_t id {};
  uint64_t gpr[4] {};       // rcx, rdx, r8, r9
  uint64_t xmm[4][2] {};    // xmm0-xmm3
  uintptr_t rsp {};         // points at the return address
  uint64_t ret {};          // rax on return
  uint64_t xmmRet[2] {};    // xmm0 on return (set together with returnsFloat)
  bool returnsFloat {};

  // Full guest register state at the call, for the SEH functions that snapshot or restore it
  // (RtlCaptureContext, NtContinue). Filled by the bridge / the host entry stub when `haveState` is
  // set. x86[i] is X86State register index i: RAX 0, RCX 1, RDX 2, RBX 3, RSP 4, RBP 5, RSI 6,
  // RDI 7, R8..R15 8..15. `rip` is the return address into the caller (what RtlCaptureContext
  // records as Rip). xmmFull holds XMM0..15; on the host only the callee-saved XMM6..15 are exact.
  uint64_t x86[16] {};
  uint64_t xmmFull[16][2] {};
  uint64_t resumeGpr[16] {};  // NtContinue target state, applied when `resume` is set
  uint64_t resumeXmm[16][2] {};
  uint64_t rip {};
  uint64_t resumeRip {};
  bool haveState {};
  bool resume {};

  uint64_t Arg(unsigned index) const;  // integer/pointer argument `index` (0-based)
  template <typename T> T* Ptr(unsigned index) const { return reinterpret_cast<T*>(Arg(index)); }
  double ArgDouble(unsigned index) const;
  void ReturnDouble(double value);
};

using Handler = uint64_t (*)(CallContext&);

struct Function {
  std::string dll;   // lower case, e.g. "kernel32.dll"
  std::string name;
  Handler handler;   // nullptr: not implemented
};

enum class StubKind {
  Bridge,      // mov r10, rcx; mov eax, op; syscall; ret   (FEXCore on iPhone)
  HostNative,  // mov r10, rcx; mov eax, op; movabs r11, entry; jmp r11   (x86-64 host tests)
};

// Operation number in rax for stub `id`; GuestBridge implementations recognise it by the tag.
constexpr uint32_t kOperationTag = 0x57490000u;  // "WI"
constexpr uint32_t Operation(uint32_t id) { return kOperationTag | id; }
constexpr bool IsWin32Operation(uint64_t op) { return (op & 0xffff0000u) == kOperationTag; }

class Runtime {
public:
  // `stubMemory` must stay valid and be readable by the guest (and executable for HostNative).
  Runtime(StubKind kind, std::span<uint8_t> stubMemory, uintptr_t hostEntry = 0);

  // Address for an import, creating its stub on first use. Never fails while stub space lasts.
  std::optional<uintptr_t> Resolve(std::string_view dll, std::string_view name);

  // Runs the handler for ctx.id; unknown or unimplemented ids log and return 0.
  void Dispatch(CallContext& ctx);

  const Function* Lookup(std::string_view dll, std::string_view name) const;
  // The implemented function named `name` in any built-in DLL, or nullptr.
  const Function* LookupAnyDll(std::string_view name) const;
  std::vector<std::string> UnimplementedCalls() const;  // "dll!name" seen at run time

  // The function a stub address belongs to, or nullptr if `address` is not a bound stub. Lets the
  // SEH dispatcher recognise a language-handler thunk that points at our own __C_specific_handler.
  const Function* FunctionForStub(uintptr_t address) const;

  // The (dll, name) bound to stub `id`, or empty views if `id` is unbound. Read-only; for a
  // diagnostic breadcrumb that records the last import a crashing thread was dispatching.
  std::pair<std::string_view, std::string_view> StubName(uint32_t id) const;

  // A fresh guest stub whose call runs `handler`, for a callback the guest calls (an il2cpp
  // InternalCall reverse thunk). Returns the stub address, or nullopt if stub space is exhausted.
  // The handler must outlive the Runtime; `name` is only for diagnostics.
  std::optional<uintptr_t> BindCallback(Handler handler, std::string name = "callback");

  static Runtime* Current();  // the instance Dispatch() for host stubs goes to
  static void SetCurrent(Runtime* runtime);

private:
  struct Stub {
    uint32_t id;
    const Function* function;  // nullptr: name not in the table
    std::string dll, name;
  };
  uintptr_t EmitStub(uint32_t id);

  StubKind kind_;
  std::span<uint8_t> stubs_;
  uintptr_t hostEntry_;
  std::vector<Stub> bound_;
  std::vector<std::string> unimplemented_;
  std::deque<Function> callbacks_;  // synthetic Functions for BindCallback; pointers must stay stable
};

// Modules the guest can see through GetModuleHandle/GetProcAddress/LoadLibrary: loaded PE images
// and the built-in DLLs (whose "exports" are stubs from the Runtime).
struct Module {
  std::string name;              // lower case file name, e.g. "gameassembly.dll"
  std::u16string path;           // what GetModuleFileNameW reports
  uintptr_t base {};             // HMODULE; for built-in DLLs a pseudo handle
  std::function<std::optional<uintptr_t>(std::string_view)> lookup;  // export by name
};
void RegisterModule(Module module, bool isMainModule = false);
const Module* FindModuleByName(std::string_view name);   // "kernel32", "KERNEL32.dll", ...
const Module* FindModuleByHandle(uintptr_t handle);
const Module* MainModule();
bool IsBuiltinDll(std::string_view name);
// HMODULE of a loaded module or a built-in DLL (registered on first use), as LoadLibrary sees it; 0 if
// neither.
uint64_t ModuleHandle(const std::string& name);

// Windows paths <-> host paths (kernel32_files.cpp). "C:\" is the host root, so the Unix paths
// the Android engine passes (e.g. /var/mobile/...) reach the guest as C:\var\mobile\... and map
// back unchanged. C:\Users\Player (AppData, saves) is redirected to `profileRoot`.
void SetProfileRoot(std::string hostDirectory);
std::string HostPath(std::u16string_view windowsPath);
std::u16string WindowsPath(std::string_view hostPath);

// UTF-16 (Windows wchar_t) helpers for guest strings.
std::u16string ReadWide(const char16_t* text);
std::string ToUtf8(std::u16string_view text);
std::u16string ToUtf16(std::string_view text);

// The table of implemented functions, from all win32/*.cpp files.
std::span<const Function> Functions();

// Per-module registration, called by Functions() once.
void RegisterKernel32(std::vector<Function>& out);
void RegisterOtherDlls(std::vector<Function>& out);
void RegisterBaselib(std::vector<Function>& out);
void RegisterKernel32Sync(std::vector<Function>& out);
void RegisterKernel32Files(std::vector<Function>& out);
void RegisterKernel32Misc(std::vector<Function>& out);
void RegisterNtdllSeh(std::vector<Function>& out);

// Calls guest code (a callback the guest passed in) with the Microsoft x64 convention and integer
// or pointer arguments; returns rax. Set by the host: FEXCore's GuestEngine::CallWin64 on iPhone,
// a direct ms_abi call in host tests. Without a caller the call is logged and 0 returned.
using GuestCaller = uint64_t (*)(uintptr_t function, std::span<const uint64_t> arguments);
void SetGuestCaller(GuestCaller caller);
uint64_t CallGuest(uintptr_t function, std::initializer_list<uint64_t> arguments);

// Runs a thread the guest creates (CreateThread): called on a new host thread, it must run
// `start(parameter)` as guest code there and return its exit code. On iPhone it gives the host
// thread a guest thread of its own (stack, TEB); in host tests it calls `start` directly.
using ThreadRunner = uint64_t (*)(uintptr_t start, uint64_t parameter);
void SetThreadRunner(ThreadRunner runner);

// ---- implicit TLS and the thread environment block (win32/tls.cpp) -----------------------
// A loaded PE module can carry a TLS directory (__declspec(thread) data and TLS callbacks). Each
// guest thread needs its own copy of that data, reached through the TEB: x64 Windows code loads
// gs:[0x58] (ThreadLocalStoragePointer), an array indexed by the module's TLS index, each entry a
// pointer to that thread's copy of the template. The host and the device both build a TEB with
// this layout so the same guest code runs against either.

enum DllReason : uint32_t {  // DllMain / TLS-callback reason
  DLL_PROCESS_DETACH = 0, DLL_PROCESS_ATTACH = 1, DLL_THREAD_ATTACH = 2, DLL_THREAD_DETACH = 3,
};

// Registers the loaded module's TLS directory, assigns it a TLS index and writes that index to the
// directory's AddressOfIndex. Called once after the image is loaded; `imageBase` is its HINSTANCE.
void RegisterTlsImage(uintptr_t imageBase, const Pe::Tls& tls);

// Allocates this guest thread's ThreadLocalStoragePointer array and a fresh copy of each registered
// module's TLS template, and returns the array address to store at TEB+0x58. Without a registered
// TLS image it returns 0 (the TEB slot is then null, which guest code without TLS never reads).
uintptr_t AllocThreadTls();
void FreeThreadTls(uintptr_t tlsPointer);

// Runs every registered module's TLS callbacks with (HINSTANCE, reason, nullptr) through CallGuest.
// Call it on the thread whose TEB/TLS is already live (host tests, where CallGuest runs natively).
void RunTlsCallbacks(DllReason reason);

// The registered TLS callbacks as (callback, HINSTANCE) pairs in call order, for a caller that
// drives guest execution itself: on the device CallGuest cannot reach a specific idle thread, so
// the runner invokes each one through GuestEngine::CallWin64 on the thread being started.
std::vector<std::pair<uintptr_t, uintptr_t>> TlsCallbacks();

// Fills the standard x64 TEB fields plus ThreadLocalStoragePointer into a caller-provided buffer of
// at least kTebSize zeroed bytes. `peb` points at a zeroed PEB, `tlsPointer` at the thread's TLS
// array (from AllocThreadTls, may be 0). gs must be based at `self` for the guest to find it.
constexpr size_t kTebSize = 0x1000;
constexpr size_t kTebTlsPointerOffset = 0x58;
void InitTeb(void* teb, uintptr_t stackBase, uintptr_t stackLimit, uintptr_t self, uintptr_t peb,
             uint32_t pid, uint32_t tid, uintptr_t tlsPointer);

// ---- x64 structured exception handling (win32/ntdll_unwind.cpp, win32/ntdll_exception.cpp) -----
// Registers a loaded module's exception-unwinding table: `pdata` is its .pdata RUNTIME_FUNCTION
// array (byte length `pdataSize`) and [imageBase, imageEnd) its mapped range. RtlLookupFunctionEntry
// resolves a guest PC to its RUNTIME_FUNCTION through this, and RtlPcToFileHeader to the module.
void RegisterUnwindImage(uintptr_t imageBase, uintptr_t imageEnd, uintptr_t pdata, uint32_t pdataSize);

// Non-local resume (NtContinue) realization, for the device bridge. A guest language handler runs
// inside a FEXCore callback and may itself call RtlUnwindEx; the resume must be applied at the HLE
// call the faulting code made, not at that nested one. The bridge reports the current HLE-call depth
// (1 outermost, +1 per nested callback syscall) with SetInvokeDepth before dispatching, and after
// dispatching asks TakePendingResume what to do:
//   None   - nothing pending; return normally.
//   Apply  - transfer the guest to `gpr`/`xmm`/`rip` (consumed).
//   Escape - a nested language-handler sub-call: let this callback return (so the resume is realized
//            at its outer level); the resume stays pending.
// SetResumeRealizedByBridge(true) tells the SEH wrappers to leave the resume for the bridge; host
// tests leave it false so the wrappers fill CallContext::resume and the entry stub applies it.
enum class ResumeAction { None, Apply, Escape };
void SetInvokeDepth(int depth);
void SetResumeRealizedByBridge(bool bridge);
ResumeAction TakePendingResume(uint64_t gpr[16], uint64_t xmm[16][2], uint64_t& rip);

// Last-error value of the calling guest thread (kernel32.cpp).
void SetLastErrorValue(uint32_t error);
uint32_t LastErrorValue();

std::string Lower(std::string_view text);

} // namespace UnityBridge::Win32
