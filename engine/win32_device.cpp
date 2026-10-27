// SPDX-License-Identifier: GPL-3.0-or-later
// M2d device run: loads a Windows x86-64 DLL with UnityBridge's PE loader, binds its imports to
// the mini Win32 through `syscall` stubs, and calls its exports under FEXCore (AetherPS4's
// GuestEngine) with the Microsoft x64 convention.
//
// How a call is set up:
//  - One 16 KiB trampoline page (host read-only) for all calls: call r11; hlt, with the target in
//    r11 (volatile, never an argument). The thread starts there, the export returns into the hlt,
//    and GuestEngine::Run comes back. The code never changes: FEXCore caches translated blocks by
//    address, so a fresh trampoline per call that lands where an earlier one was unmapped would run
//    the earlier call's target again (what the first iPhone run of the missing-import check hit).
//  - A guest stack (host read-write), 16-byte aligned at the call, with 32 bytes of shadow space.
//  - A minimal TEB as the gs: base (stack limits, self pointer, PEB), which MSVC code reads.
//  - Arguments in rcx, rdx, r8, r9 / xmm0-xmm3; the result from rax / xmm0.
// The DLL image and the stub page are reported executable through GuestBridge::QueryExecutableRange:
// PE sections are 4 KiB aligned, so code and data can share a 16 KiB iOS page, which the engine's
// static MappedRanges (16 KiB aligned, never writable and executable) cannot express.
#include "core/fex/fex_guest_engine.h"
#include "loader/pe_image.h"
#include "win32/hle.h"

#include <FEXCore/Core/X86Enums.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <variant>
#include <vector>

namespace {

using namespace UnityBridge;
namespace X86 = FEXCore::X86State;
constexpr size_t kPage = 16384;

struct Report {
  std::string text;
  void Line(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    text += buf;
    text += '\n';
    std::fprintf(stderr, "UB_WIN32_DEVICE %s\n", buf);
  }
};

class Win32Bridge final : public AetherPS4::Fex::GuestBridge {
public:
  Win32::Runtime* runtime = nullptr;
  std::vector<Core::GuestExecutionRange> executable;  // DLL images and the stub page

  AetherPS4::Fex::EngineResult<bool> Invoke(Core::GuestCpu::HleCallFrame& frame) override {
    if (!Win32::IsWin32Operation(frame.operation) || runtime == nullptr) {
      std::fprintf(stderr, "UB_WIN32_DEVICE unexpected syscall rax=%#llx\n", static_cast<unsigned long long>(frame.operation));
      return AetherPS4::Fex::EngineFailure {AetherPS4::Fex::EngineStage::Bridge, ENOSYS};
    }
    Win32::CallContext c;
    c.id = static_cast<uint32_t>(frame.operation & 0xffff);
    c.gpr[0] = frame.gpr[X86::REG_RCX];  // the engine already put the stub's r10 (the caller's rcx) here
    c.gpr[1] = frame.gpr[X86::REG_RDX];
    c.gpr[2] = frame.gpr[X86::REG_R8];
    c.gpr[3] = frame.gpr[X86::REG_R9];
    for (int i = 0; i < 4; ++i) {
      c.xmm[i][0] = frame.xmm[i][0];
      c.xmm[i][1] = frame.xmm[i][1];
    }
    c.rsp = frame.rsp;
    runtime->Dispatch(c);
    frame.gpr[X86::REG_RAX] = c.ret;
    if (c.returnsFloat) {
      frame.xmm[0][0] = c.xmmRet[0];
      frame.xmm[0][1] = c.xmmRet[1];
    }
    return true;
  }

  std::optional<Core::GuestExecutionRange> QueryExecutableRange(std::uintptr_t address) override {
    for (const auto& range : executable) {
      if (address >= range.Begin && address < range.Begin + range.Size) return range;
    }
    return std::nullopt;
  }
};

struct HostMemory {
  void* base = MAP_FAILED;
  size_t size = 0;
  explicit HostMemory(size_t bytes) : base {mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)}, size {bytes} {}
  ~HostMemory() { if (base != MAP_FAILED) munmap(base, size); }
  bool ok() const { return base != MAP_FAILED; }
  uintptr_t addr() const { return reinterpret_cast<uintptr_t>(base); }
};

struct CallResult {
  bool ok = false;
  uint64_t rax = 0;
  uint64_t xmm0 = 0;
};

// The shared trampoline page: call r11; hlt (`length` bytes).
struct Trampoline {
  HostMemory page {kPage};
  size_t length = 0;
  Trampoline() {
    if (!page.ok()) return;
    auto* code = static_cast<uint8_t*>(page.base);
    code[length++] = 0x41; code[length++] = 0xff; code[length++] = 0xd3;  // call r11
    code[length++] = 0xf4;                                                 // hlt
    mprotect(page.base, kPage, PROT_READ);  // the engine requires guest code host read-only
  }
};

// Calls `target(args...)` with the Microsoft x64 convention on a fresh guest thread.
CallResult CallWindows(AetherPS4::Fex::GuestEngine& engine, Report& report, uintptr_t target,
                       std::initializer_list<uint64_t> args, std::initializer_list<double> doubles = {}) {
  CallResult out;
  static Trampoline trampoline;
  HostMemory stack(1 << 20), teb(2 * kPage);
  if (!trampoline.page.ok() || !stack.ok() || !teb.ok()) {
    report.Line("FAIL host memory for the call");
    return out;
  }

  const uintptr_t stackTop = stack.addr() + stack.size;
  // TEB (x64): NT_TIB.StackBase @0x8, StackLimit @0x10, Self @0x30, ClientId @0x40/0x48,
  // ProcessEnvironmentBlock @0x60 (a zeroed PEB in the second page).
  auto* t = static_cast<uint8_t*>(teb.base);
  const uint64_t stackBase = stackTop, stackLimit = stack.addr(), self = teb.addr(), peb = teb.addr() + kPage;
  const uint64_t pid = static_cast<uint64_t>(getpid());
  std::memcpy(t + 0x08, &stackBase, 8);
  std::memcpy(t + 0x10, &stackLimit, 8);
  std::memcpy(t + 0x30, &self, 8);
  std::memcpy(t + 0x40, &pid, 8);
  std::memcpy(t + 0x60, &peb, 8);

  Core::GuestExecutionRequest request;
  request.Rip = trampoline.page.addr();
  request.Gpr[X86::REG_R11] = target;
  request.Rsp = stackTop - 0x20;  // 16-byte aligned at `call`; the callee's shadow space is above
  request.Rflags = 1u << 1;
  request.GsBase = teb.addr();
  const uint64_t registers[4] = {X86::REG_RCX, X86::REG_RDX, X86::REG_R8, X86::REG_R9};
  size_t i = 0;
  for (const auto a : args) if (i < 4) request.Gpr[registers[i++]] = a;
  i = 0;
  for (const auto d : doubles) if (i < 4) std::memcpy(request.Xmm[i++].data(), &d, 8);
  request.MappedRanges = {
      {trampoline.page.addr(), kPage, true, false},
      {stack.addr(), stack.size, false, true},
  };

  auto created = engine.CreateThread(request);
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&created)) {
    report.Line("FAIL create guest thread: stage=%d error=%d", static_cast<int>(failure->Stage), failure->Error);
    return out;
  }
  auto* thread = std::get<AetherPS4::Fex::GuestEngine::Thread*>(created);
  auto run = engine.Run(*thread);
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&run)) {
    report.Line("FAIL run: stage=%d error=%d", static_cast<int>(failure->Stage), failure->Error);
  } else {
    const auto& state = std::get<Core::GuestExecutionState>(run);
    const uintptr_t expectedStop = trampoline.page.addr() + trampoline.length;  // just past the hlt
    out.ok = state.Rip == expectedStop || state.Rip == expectedStop - 1;
    if (!out.ok) report.Line("FAIL guest stopped at rip=%#lx, expected the trampoline's hlt", static_cast<unsigned long>(state.Rip));
    out.rax = state.Gpr[X86::REG_RAX];
    out.xmm0 = state.Xmm[0][0];
  }
  engine.DestroyThread(thread);
  return out;
}

const char* kChecks[] = {
    "SetLastError/GetLastError", "HeapAlloc zeroed", "HeapFree", "TlsAlloc/Set/Get", "critical section (recursive)",
    "MultiByteToWideChar (6 args)", "WideCharToMultiByte (8 args)", "QueryPerformanceCounter/Frequency",
    "GetModuleHandleW + GetProcAddress", "VirtualAlloc/VirtualFree", "callee-saved registers across calls",
    "DIR64 relocation applied", "BSTR (OLEAUT32 by ordinal)", "SAFEARRAY 2-D bounds and data", "CryptGenRandom",
    "SHGetFolderPathW", "baselib semaphore (C++ names)", "events, mutex, semaphore, waits",
    "files, find, mapping (profile folder)", "locale: case mapping, compare, en-US", "InterlockedFlushSList",
};

} // namespace

// Runs tests/testdll/ubtest.dll under FEXCore. `profileDir` receives the guest's C:\Users\Player.
extern "C" int ub_win32_device_run(const char* dllPath, const char* profileDir, char* out, size_t capacity) {
  Report report;
  auto finish = [&](int status) {
    std::snprintf(out, capacity, "%s", report.text.c_str());
    return status;
  };
  report.Line("UnityBridge M2d: Windows DLL under FEXCore");

  std::ifstream in(dllPath, std::ios::binary);
  const std::vector<uint8_t> file {std::istreambuf_iterator<char>(in), {}};
  if (file.empty()) {
    report.Line("FAIL cannot read %s", dllPath);
    return finish(2);
  }
  Win32::SetProfileRoot(profileDir);

  static HostMemory stubs(4 * kPage);
  static Win32::Runtime runtime(Win32::StubKind::Bridge, {static_cast<uint8_t*>(stubs.base), stubs.size});
  Win32::Runtime::SetCurrent(&runtime);

  auto loaded = Pe::Load(file, [](const Pe::Import& imp) { return Win32::Runtime::Current()->Resolve(imp.dll, imp.Symbol()); });
  if (!loaded.image) {
    report.Line("FAIL load: %s", loaded.error.c_str());
    return finish(1);
  }
  auto& image = *loaded.image;
  report.Line("loaded at %p (%zu bytes), %zu imports, %zu unresolved", static_cast<void*>(image.base), image.size,
              image.imports.size(), loaded.unresolved.size());

  Win32::Module module;
  module.name = "ubtest.dll";
  module.path = u"C:\\UnityBridge\\ubtest.dll";
  module.base = reinterpret_cast<uintptr_t>(image.base);
  module.lookup = [&image](std::string_view name) { return image.Export(name); };
  Win32::RegisterModule(std::move(module), true);

  static Win32Bridge bridge;
  bridge.runtime = &runtime;
  bridge.executable = {
      {reinterpret_cast<uintptr_t>(image.base), (image.size + kPage - 1) & ~(kPage - 1), true, true},
      {stubs.addr(), stubs.size, true, true},
  };

  auto created = AetherPS4::Fex::GuestEngine::Create(bridge);
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&created)) {
    report.Line("FAIL create engine: stage=%d error=%d", static_cast<int>(failure->Stage), failure->Error);
    return finish(1);
  }
  auto engine = std::move(std::get<std::unique_ptr<AetherPS4::Fex::GuestEngine>>(created));

  int failures = 0;
  const auto all = image.Export("ub_test_all");
  const auto fl = image.Export("ub_test_float");
  const auto missing = image.Export("ub_test_missing");
  if (!all || !fl || !missing) {
    report.Line("FAIL exports missing");
    return finish(1);
  }

  const auto r = CallWindows(*engine, report, *all, {});
  if (!r.ok) ++failures;
  const auto bits = static_cast<uint32_t>(r.rax);
  for (size_t i = 0; i < std::size(kChecks); ++i) {
    const bool ok = r.ok && (bits & (1u << i));
    if (!ok) ++failures;
    report.Line("%-40s %s", kChecks[i], ok ? "ok" : "FAIL");
  }

  const auto f = CallWindows(*engine, report, *fl, {}, {3.0, 4.0});
  double product = 0;
  std::memcpy(&product, &f.xmm0, 8);
  const bool floatOk = f.ok && product == 12.5;
  if (!floatOk) ++failures;
  report.Line("%-40s %s", "double arguments and xmm0 return", floatOk ? "ok" : "FAIL");

  const auto m = CallWindows(*engine, report, *missing, {});
  const auto seen = runtime.UnimplementedCalls();
  const bool logged = !seen.empty() && seen.back() == "kernel32.dll!UnityBridgeTestMissingFunction";
  const bool missingOk = m.ok && m.rax == 0 && logged;
  if (!missingOk) ++failures;
  if (!missingOk) report.Line("  rax=%#llx, recorded as unimplemented: %s", static_cast<unsigned long long>(m.rax), logged ? "yes" : "no");
  report.Line("%-40s %s", "unimplemented import returns 0", missingOk ? "ok" : "FAIL");

  engine->Shutdown();
  if (failures) report.Line("RESULT: %d checks failed", failures);
  else report.Line("RESULT: all checks passed");
  return finish(failures ? 1 : 0);
}
