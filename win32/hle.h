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
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace UnityBridge::Win32 {

// What a stub hands over: the argument registers and the stack pointer at the stub's entry.
struct CallContext {
  uint32_t id {};
  uint64_t gpr[4] {};       // rcx, rdx, r8, r9
  uint64_t xmm[4][2] {};    // xmm0-xmm3
  uintptr_t rsp {};         // points at the return address
  uint64_t ret {};          // rax on return
  uint64_t xmmRet[2] {};    // xmm0 on return (set together with returnsFloat)
  bool returnsFloat {};

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
  std::vector<std::string> UnimplementedCalls() const;  // "dll!name" seen at run time

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

// Last-error value of the calling guest thread (kernel32.cpp).
void SetLastErrorValue(uint32_t error);
uint32_t LastErrorValue();

std::string Lower(std::string_view text);

} // namespace UnityBridge::Win32
