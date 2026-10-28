// SPDX-License-Identifier: GPL-3.0-or-later
// First real IL2CPP probe: loads Fellworld's own GameAssembly.dll with the PE loader, binds its
// imports to the mini Win32, and calls il2cpp_set_data_dir + il2cpp_init under FEXCore to see how
// far the runtime gets. Logging only, no graphics. The game's files never touch the build machine
// or CI: the user places GameAssembly.dll and the <Game>_Data folder on the iPhone, and this runs
// against those paths.
//
// The FEXCore + mini-Win32 glue mirrors engine/win32_device.cpp (the device-confirmed ubtest
// runner): one guest thread per call through GuestEngine::CallWin64, TLS and SEH wired up, the
// bridge realizing non-local resumes. It is duplicated here deliberately rather than shared, to
// keep the proven ubtest runner untouched.
#include "core/fex/fex_guest_engine.h"
#include "loader/pe_image.h"
#include "win32/hle.h"

#include <FEXCore/Core/X86Enums.h>

#include <cerrno>
#include <csignal>
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
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    text += buf;
    text += '\n';
    std::fprintf(stderr, "UB_IL2CPP %s\n", buf);
  }
};

class Win32Bridge final : public AetherPS4::Fex::GuestBridge {
public:
  Win32::Runtime* runtime = nullptr;
  std::vector<Core::GuestExecutionRange> executable;
  uintptr_t callbackReturnVeneer = 0;

  AetherPS4::Fex::EngineResult<bool> Invoke(Core::GuestCpu::HleCallFrame& frame) override {
    if (!Win32::IsWin32Operation(frame.operation) || runtime == nullptr) {
      std::fprintf(stderr, "UB_IL2CPP unexpected syscall rax=%#llx\n", static_cast<unsigned long long>(frame.operation));
      return AetherPS4::Fex::EngineFailure {AetherPS4::Fex::EngineStage::Bridge, ENOSYS};
    }
    static thread_local int t_depth = 0;
    struct DepthGuard {
      DepthGuard() { Win32::SetInvokeDepth(++t_depth); }
      ~DepthGuard() { Win32::SetInvokeDepth(--t_depth); }
    } depthGuard;

    Win32::CallContext c;
    c.id = static_cast<uint32_t>(frame.operation & 0xffff);
    c.gpr[0] = frame.gpr[X86::REG_RCX];
    c.gpr[1] = frame.gpr[X86::REG_RDX];
    c.gpr[2] = frame.gpr[X86::REG_R8];
    c.gpr[3] = frame.gpr[X86::REG_R9];
    for (int i = 0; i < 4; ++i) {
      c.xmm[i][0] = frame.xmm[i][0];
      c.xmm[i][1] = frame.xmm[i][1];
    }
    c.rsp = frame.rsp;
    for (int i = 0; i < 16; ++i) c.x86[i] = frame.gpr[i];
    for (int i = 0; i < 16; ++i) { c.xmmFull[i][0] = frame.xmm[i][0]; c.xmmFull[i][1] = frame.xmm[i][1]; }
    c.rip = *reinterpret_cast<const uint64_t*>(frame.rsp);
    c.haveState = true;

    runtime->Dispatch(c);

    uint64_t gpr[16], xmm[16][2], rip;
    switch (Win32::TakePendingResume(gpr, xmm, rip)) {
    case Win32::ResumeAction::Apply:
      for (int i = 0; i < 16; ++i) frame.gpr[i] = gpr[i];
      for (int i = 0; i < 16; ++i) { frame.xmm[i][0] = xmm[i][0]; frame.xmm[i][1] = xmm[i][1]; }
      {
        const uint64_t targetRsp = gpr[X86::REG_RSP];
        *reinterpret_cast<uint64_t*>(targetRsp - 8) = rip;
        frame.gpr[X86::REG_RSP] = targetRsp - 8;
        frame.rsp = targetRsp - 8;
      }
      return true;
    case Win32::ResumeAction::Escape:
      *reinterpret_cast<uint64_t*>(frame.rsp) = callbackReturnVeneer;
      return true;
    case Win32::ResumeAction::None:
      break;
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
};

struct GuestThread {
  HostMemory entry {kPage}, stack {8u << 20}, teb {2 * kPage};  // 8 MiB stack: il2cpp_init recurses deep
  AetherPS4::Fex::GuestEngine::Thread* thread = nullptr;
  uintptr_t tls = 0;

  bool Create(AetherPS4::Fex::GuestEngine& engine, Report* report) {
    auto fail = [&](const char* what, int stage, int error) {
      if (report) report->Line("FAIL %s: stage=%d error=%d", what, stage, error);
      else std::fprintf(stderr, "UB_IL2CPP FAIL %s: stage=%d error=%d\n", what, stage, error);
      return false;
    };
    if (!entry.ok() || !stack.ok() || !teb.ok()) return fail("host memory for the guest thread", 0, ENOMEM);
    *static_cast<uint8_t*>(entry.base) = 0xf4;  // hlt
    mprotect(entry.base, kPage, PROT_READ);
    auto* t = static_cast<uint8_t*>(teb.base);
    const uint64_t stackBase = stack.addr() + stack.size, stackLimit = stack.addr(), self = teb.addr();
    const uint64_t peb = teb.addr() + kPage, pid = static_cast<uint64_t>(getpid());
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    tid = static_cast<uint32_t>(tid);
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

CallResult CallWindows(GuestThread& guest, Report& report, uintptr_t target, std::initializer_list<uint64_t> args) {
  CallResult out;
  AetherPS4::Fex::Win64Call call;
  call.Rip = target;
  size_t i = 0;
  for (const auto a : args) if (i < 4) call.Gpr[i++] = a;
  auto run = g_engine->CallWin64(guest.thread, call);
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&run)) {
    report.Line("FAIL call %#lx: stage=%d error=%d", static_cast<unsigned long>(target),
                static_cast<int>(failure->Stage), failure->Error);
    return out;
  }
  out.ok = true;
  out.rax = std::get<Core::GuestExecutionState>(run).Gpr[X86::REG_RAX];
  return out;
}

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
  RunTlsCallbacksOn(guest, Win32::DLL_THREAD_ATTACH);
  AetherPS4::Fex::Win64Call call;
  call.Rip = start;
  call.Gpr[0] = parameter;
  auto run = g_engine->CallWin64(guest.thread, call);
  uint64_t code = 0xffffffffu;
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&run)) {
    std::fprintf(stderr, "UB_IL2CPP thread %#lx failed: stage=%d error=%d\n", static_cast<unsigned long>(start),
                 static_cast<int>(failure->Stage), failure->Error);
  } else {
    code = std::get<Core::GuestExecutionState>(run).Gpr[X86::REG_RAX];
  }
  RunTlsCallbacksOn(guest, Win32::DLL_THREAD_DETACH);
  Win32::FreeThreadTls(guest.tls);
  g_engine->DestroyThread(guest.thread);
  return code;
}

uint64_t CallGuestNested(uintptr_t function, std::span<const uint64_t> arguments) {
  AetherPS4::Fex::Win64Call call;
  call.Rip = function;
  for (size_t i = 0; i < arguments.size() && i < 4; ++i) call.Gpr[i] = arguments[i];
  if (arguments.size() > 4) call.Stack = arguments.subspan(4);
  auto run = g_engine->CallWin64(nullptr, call);
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&run)) {
    std::fprintf(stderr, "UB_IL2CPP callback %#lx failed: stage=%d error=%d\n", static_cast<unsigned long>(function),
                 static_cast<int>(failure->Stage), failure->Error);
    return 0;
  }
  return std::get<Core::GuestExecutionState>(run).Gpr[X86::REG_RAX];
}

// Calls export `name` with integer/pointer args; logs and returns rax. Missing export logs a note.
std::optional<uint64_t> CallExport(GuestThread& guest, Report& report, const Pe::Image& image,
                                   const char* name, std::initializer_list<uint64_t> args) {
  const auto fn = image.Export(name);
  if (!fn) {
    report.Line("  %s: not exported", name);
    return std::nullopt;
  }
  const auto r = CallWindows(guest, report, *fn, args);
  if (!r.ok) return std::nullopt;
  return r.rax;
}

// ---- crash catcher -----------------------------------------------------------------------------
// The harness installs no SIGSEGV/SIGBUS handler of its own, so a fault in il2cpp's code (or ours)
// is a silent default crash. This handler first lets FEXCore recover genuine JIT faults (which also
// wires up FEX's dual-map alias-fault recovery that nothing called before), and on a real fault
// writes the guest RIP, registers and fault address through the engine's crash-path diagnostics,
// then re-raises so the process still dies honestly. Output goes to stderr (probe.log), unbuffered.
volatile sig_atomic_t g_inCrash = 0;
struct sigaction g_oldSegv, g_oldBus;
uint8_t g_altStack[64 * 1024];

void WriteStr(const char* s) { if (s) { ssize_t n = write(2, s, std::strlen(s)); (void)n; } }

void CrashHandler(int sig, siginfo_t* info, void* ctx) {
#ifndef _WIN32
  if (sig == SIGBUS || sig == SIGSEGV) {
    if (AetherPS4::Fex::HandleGuestSignal(sig, info, ctx)) return;      // recoverable JIT fault
    if (AetherPS4::Fex::TryRecoverJitAliasFault(sig, info, ctx)) return;
  }
#endif
  if (g_inCrash) { signal(sig, SIG_DFL); raise(sig); return; }
  g_inCrash = 1;
  char buf[4096];
  WriteStr("\nUB_IL2CPP ==== FATAL SIGNAL ");
  std::snprintf(buf, sizeof(buf), "%d (addr=%p) ====\n", sig, info ? info->si_addr : nullptr);
  WriteStr(buf);
#ifndef _WIN32
  uint64_t rip = 0, syscallNo = 0;
  if (AetherPS4::Fex::BachataQueryGuestRipSyscall(&rip, &syscallNo)) {
    std::snprintf(buf, sizeof(buf), "UB_IL2CPP guest rip=%#llx rax(syscall)=%#llx\n",
                  static_cast<unsigned long long>(rip), static_cast<unsigned long long>(syscallNo));
    WriteStr(buf);
  } else {
    WriteStr("UB_IL2CPP no active FEX guest thread at the fault\n");
  }
  uint64_t rsp = 0;
  if (AetherPS4::Fex::BachataDumpGuestRegisters(buf, sizeof(buf), &rsp)) { WriteStr(buf); WriteStr("\n"); }
  if (info && AetherPS4::Fex::BachataDescribeHostFaultAddress(info->si_addr, buf, sizeof(buf))) {
    WriteStr(buf);
    WriteStr("\n");
  }
#endif
  struct sigaction* old = (sig == SIGSEGV) ? &g_oldSegv : &g_oldBus;
  if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction) old->sa_sigaction(sig, info, ctx);
  else { signal(sig, SIG_DFL); raise(sig); }
}

void InstallCrashHandler() {
  stack_t ss {};
  ss.ss_sp = g_altStack;
  ss.ss_size = sizeof(g_altStack);
  sigaltstack(&ss, nullptr);
  struct sigaction a {};
  a.sa_sigaction = &CrashHandler;
  sigemptyset(&a.sa_mask);
  a.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &a, &g_oldSegv);
  sigaction(SIGBUS, &a, &g_oldBus);
}

void RestoreCrashHandler() {
  sigaction(SIGSEGV, &g_oldSegv, nullptr);
  sigaction(SIGBUS, &g_oldBus, nullptr);
}

} // namespace

// Loads GameAssembly.dll from `dllPath`, points il2cpp at `dataDir` (the host path of the game's
// <Game>_Data folder), and calls il2cpp_init. `profileDir` maps C:\Users\Player.
extern "C" int ub_il2cpp_probe_run(const char* dllPath, const char* dataDir, const char* profileDir,
                                   char* out, size_t capacity) {
  Report report;
  auto finish = [&](int status) {
    std::snprintf(out, capacity, "%s", report.text.c_str());
    return status;
  };
  report.Line("UnityBridge IL2CPP probe: GameAssembly.dll under FEXCore");

  std::ifstream in(dllPath, std::ios::binary);
  const std::vector<uint8_t> file {std::istreambuf_iterator<char>(in), {}};
  if (file.empty()) {
    report.Line("FAIL cannot read %s", dllPath);
    report.Line("Put GameAssembly.dll in the folder shown in the app and try again.");
    return finish(2);
  }
  report.Line("GameAssembly.dll: %zu bytes", file.size());
  Win32::SetProfileRoot(profileDir);

  static HostMemory stubs(64 * kPage);  // GameAssembly imports ~206 functions, each one small stub
  static Win32::Runtime runtime(Win32::StubKind::Bridge, {static_cast<uint8_t*>(stubs.base), stubs.size});
  Win32::Runtime::SetCurrent(&runtime);

  auto loaded = Pe::Load(file, [](const Pe::Import& imp) { return Win32::Runtime::Current()->Resolve(imp.dll, imp.Symbol()); });
  if (!loaded.image) {
    report.Line("FAIL load: %s", loaded.error.c_str());
    return finish(1);
  }
  auto& image = *loaded.image;
  report.Line("loaded at %p (%zu bytes), %zu exports, %zu imports, %zu unresolved",
              static_cast<void*>(image.base), image.size, image.exports.size(), image.imports.size(),
              loaded.unresolved.size());
  for (const auto& imp : loaded.unresolved) report.Line("  unresolved import: %s!%s", imp.dll.c_str(), imp.Symbol().c_str());

  Win32::Module module;
  module.name = "gameassembly.dll";
  module.path = u"C:\\Game\\GameAssembly.dll";
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
  bridge.callbackReturnVeneer = engine->CallbackReturnRange().Begin;
  Win32::SetResumeRealizedByBridge(true);
  Win32::SetGuestCaller(&CallGuestNested);
  Win32::SetThreadRunner(&RunGuestThread);

  GuestThread guest;
  if (!guest.Create(*engine, &report)) {
    Win32::SetGuestCaller(nullptr);
    Win32::SetThreadRunner(nullptr);
    engine->Shutdown();
    g_engine = nullptr;
    return finish(1);
  }

  // Catch a fault inside il2cpp (or our handlers) and log the guest RIP before the process dies.
  InstallCrashHandler();

  // The loader runs the module's TLS callbacks and then DllMain with DLL_PROCESS_ATTACH.
  RunTlsCallbacksOn(guest, Win32::DLL_PROCESS_ATTACH);
  if (image.entryPoint) {
    const auto dll = CallWindows(guest, report, image.entryPoint,
                                 {reinterpret_cast<uintptr_t>(image.base), Win32::DLL_PROCESS_ATTACH, 0});
    report.Line("DllMain(DLL_PROCESS_ATTACH) returned %#llx", static_cast<unsigned long long>(dll.rax));
  }

  // Point il2cpp at the game's data folder (where it finds il2cpp_data/Metadata/global-metadata.dat).
  // The path is a Windows path mapping back to the device folder through the mini Win32.
  const std::string dataDirWin = Win32::ToUtf8(Win32::WindowsPath(dataDir ? dataDir : ""));
  report.Line("data dir: %s  ->  %s", dataDir ? dataDir : "(none)", dataDirWin.c_str());
  CallExport(guest, report, image, "il2cpp_set_data_dir", {reinterpret_cast<uintptr_t>(dataDirWin.c_str())});
  CallExport(guest, report, image, "il2cpp_set_config_dir", {reinterpret_cast<uintptr_t>(dataDirWin.c_str())});

  static const char kDomainName[] = "IL2CPP Root Domain";
  report.Line("calling il2cpp_init ...");
  const auto initResult = CallExport(guest, report, image, "il2cpp_init", {reinterpret_cast<uintptr_t>(kDomainName)});
  if (initResult) {
    report.Line("il2cpp_init returned %llu", static_cast<unsigned long long>(*initResult));
    if (*initResult) {
      if (const auto corlib = CallExport(guest, report, image, "il2cpp_get_corlib", {}))
        report.Line("il2cpp_get_corlib = %#llx (mscorlib image; non-zero means metadata loaded)",
                    static_cast<unsigned long long>(*corlib));
      if (const auto domain = CallExport(guest, report, image, "il2cpp_domain_get", {}))
        report.Line("il2cpp_domain_get = %#llx", static_cast<unsigned long long>(*domain));
    }
  } else {
    report.Line("il2cpp_init did not return (a fault, or the export is missing)");
  }

  const auto unimplemented = runtime.UnimplementedCalls();
  report.Line("unimplemented imports called during init: %zu", unimplemented.size());
  for (const auto& u : unimplemented) report.Line("  %s", u.c_str());

  const bool ok = initResult && *initResult != 0;
  report.Line("RESULT: %s", ok ? "il2cpp_init succeeded" : "il2cpp_init did not succeed (see above)");

  RestoreCrashHandler();
  Win32::FreeThreadTls(guest.tls);
  engine->DestroyThread(guest.thread);
  Win32::SetGuestCaller(nullptr);
  Win32::SetThreadRunner(nullptr);
  engine->Shutdown();
  g_engine = nullptr;
  return finish(ok ? 0 : 1);
}
