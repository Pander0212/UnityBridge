// SPDX-License-Identifier: GPL-3.0-or-later
// M2d device run: loads a Windows x86-64 DLL with UnityBridge's PE loader, binds its imports to
// the mini Win32 through `syscall` stubs, and calls its exports under FEXCore (AetherPS4's
// GuestEngine) with the Microsoft x64 convention.
//
// How a call is set up: one guest thread for the run (a 1 MiB stack, a minimal TEB as the gs:
// base with stack limits, self pointer and PEB), reused for every call through
// GuestEngine::CallWin64, which also runs the callbacks the mini Win32 makes (EnumSystemLocalesW,
// FLS) nested on the thread that is in the syscall. Arguments in rcx, rdx, r8, r9 / xmm0-xmm3;
// the result from rax / xmm0.
// The DLL image, the stub page and the engine's callback-return page are reported executable
// through GuestBridge::QueryExecutableRange: PE sections are 4 KiB aligned, so code and data can
// share a 16 KiB iOS page, which the engine's static MappedRanges (16 KiB aligned, never writable
// and executable) cannot express.
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
#include <pthread.h>
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
    // Full register state for the SEH functions (RtlCaptureContext snapshots it; NtContinue restores
    // it). frame.gpr is in X86State order with RCX already holding the caller's rcx; rip is the
    // return address the stub would ret to.
    for (int i = 0; i < 16; ++i) c.x86[i] = frame.gpr[i];
    for (int i = 0; i < 16; ++i) { c.xmmFull[i][0] = frame.xmm[i][0]; c.xmmFull[i][1] = frame.xmm[i][1]; }
    c.rip = *reinterpret_cast<const uint64_t*>(frame.rsp);
    c.haveState = true;

    runtime->Dispatch(c);

    if (c.resume) {
      // A non-local resume: load the target register file and make the stub's `ret` land at
      // resumeRip, so FEXCore continues the guest at the handler target with the unwound state.
      for (int i = 0; i < 16; ++i) frame.gpr[i] = c.resumeGpr[i];
      for (int i = 0; i < 16; ++i) { frame.xmm[i][0] = c.resumeXmm[i][0]; frame.xmm[i][1] = c.resumeXmm[i][1]; }
      const uint64_t targetRsp = c.resumeGpr[X86::REG_RSP];
      *reinterpret_cast<uint64_t*>(targetRsp - 8) = c.resumeRip;
      frame.gpr[X86::REG_RSP] = targetRsp - 8;
      frame.rsp = targetRsp - 8;
      return true;
    }
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

// The guest thread all calls run on, with its stack and TEB. CreateThread wants an entry point in
// a read-only code range; the thread never starts there (a lone hlt), CallWin64 sets rip itself.
struct GuestThread {
  HostMemory entry {kPage}, stack {1 << 20}, teb {2 * kPage};
  AetherPS4::Fex::GuestEngine::Thread* thread = nullptr;
  uintptr_t tls = 0;  // ThreadLocalStoragePointer (TEB+0x58), freed in DestroyGuestThread

  // `report` may be null (threads the guest creates log to stderr).
  bool Create(AetherPS4::Fex::GuestEngine& engine, Report* report) {
    auto fail = [&](const char* what, int stage, int error) {
      if (report) report->Line("FAIL %s: stage=%d error=%d", what, stage, error);
      else std::fprintf(stderr, "UB_WIN32_DEVICE FAIL %s: stage=%d error=%d\n", what, stage, error);
      return false;
    };
    if (!entry.ok() || !stack.ok() || !teb.ok()) return fail("host memory for the guest thread", 0, ENOMEM);
    *static_cast<uint8_t*>(entry.base) = 0xf4;  // hlt
    mprotect(entry.base, kPage, PROT_READ);     // the engine requires guest code host read-only
    // TEB (x64): NT_TIB.StackBase @0x8, StackLimit @0x10, Self @0x30, ClientId @0x40/0x48,
    // ProcessEnvironmentBlock @0x60 (a zeroed PEB in the second page).
    auto* t = static_cast<uint8_t*>(teb.base);
    const uint64_t stackBase = stack.addr() + stack.size, stackLimit = stack.addr(), self = teb.addr();
    const uint64_t peb = teb.addr() + kPage, pid = static_cast<uint64_t>(getpid());
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    tid = static_cast<uint32_t>(tid);  // GetCurrentThreadId's value
    // ThreadLocalStoragePointer @0x58: this thread's copy of each module's __declspec(thread) data,
    // so gs:[0x58] reaches implicit TLS the way it does on Windows.
    tls = Win32::AllocThreadTls();
    std::memcpy(t + 0x08, &stackBase, 8);
    std::memcpy(t + 0x10, &stackLimit, 8);
    std::memcpy(t + 0x30, &self, 8);
    std::memcpy(t + 0x40, &pid, 8);
    std::memcpy(t + 0x48, &tid, 8);
    std::memcpy(t + 0x58, &tls, 8);
    std::memcpy(t + 0x60, &peb, 8);

    Core::GuestExecutionRequest request;
    request.Rip = entry.addr();
    request.Rsp = stackBase - 0x20;
    request.Rflags = 1u << 1;
    request.GsBase = teb.addr();
    request.MappedRanges = {{entry.addr(), kPage, true, false}, {stack.addr(), stack.size, false, true}};
    auto created = engine.CreateThread(request);
    if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&created)) {
      return fail("create guest thread", static_cast<int>(failure->Stage), failure->Error);
    }
    thread = std::get<AetherPS4::Fex::GuestEngine::Thread*>(created);
    return true;
  }
};

AetherPS4::Fex::GuestEngine* g_engine = nullptr;

// Calls `target(args...)` with the Microsoft x64 convention on `guest`.
CallResult CallWindows(GuestThread& guest, Report& report, uintptr_t target, std::initializer_list<uint64_t> args,
                       std::initializer_list<double> doubles = {}) {
  CallResult out;
  AetherPS4::Fex::Win64Call call;
  call.Rip = target;
  size_t i = 0;
  for (const auto a : args) if (i < 4) call.Gpr[i++] = a;
  i = 0;
  for (const auto d : doubles) if (i < 4) std::memcpy(call.Xmm[i++].data(), &d, 8);
  auto run = g_engine->CallWin64(guest.thread, call);
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&run)) {
    report.Line("FAIL call %#lx: stage=%d error=%d", static_cast<unsigned long>(target), static_cast<int>(failure->Stage),
                failure->Error);
    return out;
  }
  const auto& state = std::get<Core::GuestExecutionState>(run);
  out.ok = true;
  out.rax = state.Gpr[X86::REG_RAX];
  out.xmm0 = state.Xmm[0][0];
  return out;
}

// Win32::ThreadRunner on iPhone: the new host thread gets a guest thread of its own for the start
// routine and gives it back before the thread counts as finished (Shutdown needs none left).
// Runs the registered TLS callbacks on `guest` with the given reason (DLL_THREAD_ATTACH etc.).
void RunTlsCallbacksOn(GuestThread& guest, uint32_t reason) {
  for (const auto& [callback, imageBase] : Win32::TlsCallbacks()) {
    AetherPS4::Fex::Win64Call call;
    call.Rip = callback;
    call.Gpr[0] = imageBase;
    call.Gpr[1] = reason;
    call.Gpr[2] = 0;
    g_engine->CallWin64(guest.thread, call);
  }
}

uint64_t RunGuestThread(uintptr_t start, uint64_t parameter) {
  GuestThread guest;
  if (!guest.Create(*g_engine, nullptr)) return 0xffffffffu;
  // The Windows loader runs the TLS callbacks with DLL_THREAD_ATTACH before the start routine.
  RunTlsCallbacksOn(guest, Win32::DLL_THREAD_ATTACH);
  AetherPS4::Fex::Win64Call call;
  call.Rip = start;
  call.Gpr[0] = parameter;
  auto run = g_engine->CallWin64(guest.thread, call);
  uint64_t code = 0xffffffffu;
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&run)) {
    std::fprintf(stderr, "UB_WIN32_DEVICE thread %#lx failed: stage=%d error=%d\n", static_cast<unsigned long>(start),
                 static_cast<int>(failure->Stage), failure->Error);
  } else {
    code = std::get<Core::GuestExecutionState>(run).Gpr[X86::REG_RAX];
  }
  RunTlsCallbacksOn(guest, Win32::DLL_THREAD_DETACH);
  Win32::FreeThreadTls(guest.tls);
  g_engine->DestroyThread(guest.thread);
  return code;
}

// Win32::CallGuest on iPhone: a callback from inside an import, nested on the calling guest thread.
uint64_t CallGuestNested(uintptr_t function, std::span<const uint64_t> arguments) {
  AetherPS4::Fex::Win64Call call;
  call.Rip = function;
  for (size_t i = 0; i < arguments.size() && i < 4; ++i) call.Gpr[i] = arguments[i];
  if (arguments.size() > 4) call.Stack = arguments.subspan(4);
  auto run = g_engine->CallWin64(nullptr, call);
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&run)) {
    std::fprintf(stderr, "UB_WIN32_DEVICE callback %#lx failed: stage=%d error=%d\n", static_cast<unsigned long>(function),
                 static_cast<int>(failure->Stage), failure->Error);
    return 0;
  }
  return std::get<Core::GuestExecutionState>(run).Gpr[X86::REG_RAX];
}

const char* kChecks[] = {
    "SetLastError/GetLastError", "HeapAlloc zeroed", "HeapFree", "TlsAlloc/Set/Get", "critical section (recursive)",
    "MultiByteToWideChar (6 args)", "WideCharToMultiByte (8 args)", "QueryPerformanceCounter/Frequency",
    "GetModuleHandleW + GetProcAddress", "VirtualAlloc/VirtualFree", "callee-saved registers across calls",
    "DIR64 relocation applied", "BSTR (OLEAUT32 by ordinal)", "SAFEARRAY 2-D bounds and data", "CryptGenRandom",
    "SHGetFolderPathW", "baselib semaphore (C++ names)", "events, mutex, semaphore, waits",
    "files, find, mapping (profile folder)", "locale: case mapping, compare, en-US", "InterlockedFlushSList",
    "callback: EnumSystemLocalesW", "callback: FlsFree (nested import)",
    "CreateThread: own TLS and last error", "CreateThread suspended + ResumeThread",
    "implicit TLS: __declspec(thread) + callback",
    "SEH: __try/__except catches RaiseException",
    "SEH: __finally runs while unwinding to __except",
    "SEH: RtlCaptureContext/LookupFunctionEntry/VirtualUnwind/PcToFileHeader",
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
  if (image.tls) Win32::RegisterTlsImage(reinterpret_cast<uintptr_t>(image.base), *image.tls);
  Win32::RegisterUnwindImage(reinterpret_cast<uintptr_t>(image.base),
                             reinterpret_cast<uintptr_t>(image.base) + image.size,
                             reinterpret_cast<uintptr_t>(image.base) + image.pdataRva, image.pdataSize);

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
  g_engine = engine.get();
  bridge.executable.push_back(engine->CallbackReturnRange());
  Win32::SetGuestCaller(&CallGuestNested);
  Win32::SetThreadRunner(&RunGuestThread);
  const auto all = image.Export("ub_test_all");
  const auto fl = image.Export("ub_test_float");
  const auto missing = image.Export("ub_test_missing");
  GuestThread guest;
  if (!all || !fl || !missing || !guest.Create(*engine, &report)) {
    if (!all || !fl || !missing) report.Line("FAIL exports missing");
    Win32::SetGuestCaller(nullptr);
    Win32::SetThreadRunner(nullptr);
    engine->Shutdown();
    g_engine = nullptr;
    return finish(1);
  }

  int failures = 0;

  // The initial thread runs the TLS callbacks with DLL_PROCESS_ATTACH before any export, as the
  // Windows loader does; this installs each thread variable's template and lets the callback run.
  RunTlsCallbacksOn(guest, Win32::DLL_PROCESS_ATTACH);

  const auto r = CallWindows(guest, report, *all, {});
  if (!r.ok) ++failures;
  const auto bits = static_cast<uint32_t>(r.rax);
  for (size_t i = 0; i < std::size(kChecks); ++i) {
    const bool ok = r.ok && (bits & (1u << i));
    if (!ok) ++failures;
    report.Line("%-40s %s", kChecks[i], ok ? "ok" : "FAIL");
  }

  const auto f = CallWindows(guest, report, *fl, {}, {3.0, 4.0});
  double product = 0;
  std::memcpy(&product, &f.xmm0, 8);
  const bool floatOk = f.ok && product == 12.5;
  if (!floatOk) ++failures;
  report.Line("%-40s %s", "double arguments and xmm0 return", floatOk ? "ok" : "FAIL");

  const auto m = CallWindows(guest, report, *missing, {});
  const auto seen = runtime.UnimplementedCalls();
  const bool logged = !seen.empty() && seen.back() == "kernel32.dll!UnityBridgeTestMissingFunction";
  const bool missingOk = m.ok && m.rax == 0 && logged;
  if (!missingOk) ++failures;
  if (!missingOk) report.Line("  rax=%#llx, recorded as unimplemented: %s", static_cast<unsigned long long>(m.rax), logged ? "yes" : "no");
  report.Line("%-40s %s", "unimplemented import returns 0", missingOk ? "ok" : "FAIL");

  RunTlsCallbacksOn(guest, Win32::DLL_PROCESS_DETACH);
  Win32::FreeThreadTls(guest.tls);
  engine->DestroyThread(guest.thread);
  Win32::SetGuestCaller(nullptr);
  Win32::SetThreadRunner(nullptr);
  const auto shutdown = engine->Shutdown();
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&shutdown)) {
    report.Line("engine shutdown: stage=%d error=%d", static_cast<int>(failure->Stage), failure->Error);
  }
  g_engine = nullptr;
  if (failures) report.Line("RESULT: %d checks failed", failures);
  else report.Line("RESULT: all checks passed");
  return finish(failures ? 1 : 0);
}
