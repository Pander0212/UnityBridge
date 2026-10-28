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
// keep the proven ubtest runner untouched. Hardware faults (FEX's unaligned-atomic fixups) go through
// the shared engine/mach_fault.
//
// After il2cpp_init the runtime lives for the rest of the process: il2cpp has started threads of its
// own (the GC finalizer, ...) that keep running guest code, and the GC scans the main thread's
// stack. So the engine, the main guest thread (stack + TEB), the loaded image and the thread runner
// are never torn down after that point, and the probe runs once per app launch.
#include "core/fex/fex_guest_engine.h"
#include "engine/il2cpp_bridge.h"
#include "engine/mach_fault.h"
#include "loader/pe_image.h"
#include "win32/hle.h"

#include <FEXCore/Core/X86Enums.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <pthread.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>

namespace {

using namespace UnityBridge;
namespace X86 = FEXCore::X86State;
constexpr size_t kPage = 16384;

uintptr_t g_imageBase = 0, g_imageEnd = 0;  // GameAssembly.dll's mapped range, for logging offsets
std::atomic<int> g_guestThreads {0};         // threads il2cpp created that are running guest code
thread_local int t_hleDepth = 0;             // >0 while this host thread runs an HLE call for the guest
std::atomic<bool> g_il2cppStarted {false};   // set once per process: the runtime then stays up

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
    struct DepthGuard {
      DepthGuard() { Win32::SetInvokeDepth(++t_hleDepth); }
      ~DepthGuard() { Win32::SetInvokeDepth(--t_hleDepth); }
    } depthGuard;

    Win32::CallContext c;
    c.id = static_cast<uint32_t>(frame.operation & 0xffff);
    {
      const auto nm = runtime->StubName(c.id);  // record the API before it runs, for a fault report
      MachFault::NoteImport(nm.first, nm.second);
    }
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
  MachFault::RegisterThisThread();  // a guest thread il2cpp created: its hardware faults too
  const unsigned long long offset = start >= g_imageBase && start < g_imageEnd ? start - g_imageBase : start;
  std::fprintf(stderr, "UB_IL2CPP guest thread started: start=GameAssembly+%#llx (%d running)\n", offset,
               ++g_guestThreads);
  GuestThread guest;
  if (!guest.Create(*g_engine, nullptr)) {
    --g_guestThreads;
    MachFault::UnregisterThisThread();
    return 0xffffffffu;
  }
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
  std::fprintf(stderr, "UB_IL2CPP guest thread ended: start=GameAssembly+%#llx code=%#llx (%d running)\n", offset,
               static_cast<unsigned long long>(code), --g_guestThreads);
  MachFault::UnregisterThisThread();
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

// Il2cppBridge::GuestCall: runs an il2cpp export for native (ARM64) code on this host thread. From
// inside an HLE call (guest -> host -> bridge) it nests on the guest thread already running here;
// otherwise it uses this host thread's own guest thread, created on its first call and kept for as
// long as the host thread may call in (the main thread's is the one il2cpp_init ran on).
thread_local GuestThread* t_bridgeGuest = nullptr;

uint64_t BridgeCall(uintptr_t target, const uint64_t (&args)[8]) {
  AetherPS4::Fex::Win64Call call;
  call.Rip = target;
  for (int i = 0; i < 4; ++i) call.Gpr[i] = args[i];
  call.Stack = std::span<const uint64_t>(args + 4, 4);
  AetherPS4::Fex::GuestEngine::Thread* thread = nullptr;  // nested
  if (t_hleDepth == 0) {
    if (t_bridgeGuest == nullptr) {
      auto* guest = new GuestThread;
      MachFault::RegisterThisThread();
      if (!guest->Create(*g_engine, nullptr)) {
        delete guest;
        return 0;
      }
      RunTlsCallbacksOn(*guest, Win32::DLL_THREAD_ATTACH);
      t_bridgeGuest = guest;
    }
    thread = t_bridgeGuest->thread;
  }
  auto run = g_engine->CallWin64(thread, call);
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&run)) {
    std::fprintf(stderr, "UB_IL2CPP bridge call %#lx failed: stage=%d error=%d\n", static_cast<unsigned long>(target),
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
uint8_t g_altStack[128 * 1024];
const int kCaughtSignals[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};  // not SIGTRAP: the JIT uses it
struct sigaction g_oldActions[std::size(kCaughtSignals)];

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
  std::snprintf(buf, sizeof(buf), "\nUB_IL2CPP ==== FATAL SIGNAL %d (addr=%p) ====\n", sig,
                info ? info->si_addr : nullptr);
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
  for (size_t i = 0; i < std::size(kCaughtSignals); ++i) {
    if (kCaughtSignals[i] != sig) continue;
    struct sigaction* old = &g_oldActions[i];
    if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction) { old->sa_sigaction(sig, info, ctx); return; }
    break;
  }
  signal(sig, SIG_DFL);
  raise(sig);
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
  for (size_t i = 0; i < std::size(kCaughtSignals); ++i) sigaction(kCaughtSignals[i], &a, &g_oldActions[i]);
}

// M3 groundwork: the il2cpp C API through the ARM64 bridge, called exactly the way libunity will --
// plain native function pointers from Il2cppBridge::Symbol, nothing at the call sites knows about
// FEXCore or the Microsoft x64 convention. Lists the assemblies, then runs real managed code: a
// static call into an il2cpp icall (which lands in our GetSystemInfo), an argument round trip, an
// instance method on a string we create, and a managed exception (il2cpp throws it as a C++
// exception, so this also exercises our SEH with GameAssembly's own __CxxFrameHandler).
// Returns the number of failed checks.
int RunBridgeTests(Report& report) {
  report.Line("--- il2cpp API through the ARM64 bridge (as libunity will call it) ---");
  bool missing = false;
  auto api = [&](const char* name) {
    void* fn = Il2cppBridge::Symbol(name);
    if (fn == nullptr) {
      report.Line("  %s: not exported", name);
      missing = true;
    }
    return fn;
  };
  using P = void*;
  const auto domain_get = reinterpret_cast<P (*)()>(api("il2cpp_domain_get"));
  const auto domain_get_assemblies = reinterpret_cast<P* (*)(P, size_t*)>(api("il2cpp_domain_get_assemblies"));
  const auto assembly_get_image = reinterpret_cast<P (*)(P)>(api("il2cpp_assembly_get_image"));
  const auto image_get_name = reinterpret_cast<const char* (*)(P)>(api("il2cpp_image_get_name"));
  const auto image_get_class_count = reinterpret_cast<size_t (*)(P)>(api("il2cpp_image_get_class_count"));
  const auto image_get_class = reinterpret_cast<P (*)(P, size_t)>(api("il2cpp_image_get_class"));
  const auto class_get_name = reinterpret_cast<const char* (*)(P)>(api("il2cpp_class_get_name"));
  const auto class_get_namespace = reinterpret_cast<const char* (*)(P)>(api("il2cpp_class_get_namespace"));
  const auto get_corlib = reinterpret_cast<P (*)()>(api("il2cpp_get_corlib"));
  const auto class_from_name = reinterpret_cast<P (*)(P, const char*, const char*)>(api("il2cpp_class_from_name"));
  const auto class_get_method_from_name = reinterpret_cast<P (*)(P, const char*, int)>(api("il2cpp_class_get_method_from_name"));
  const auto runtime_invoke = reinterpret_cast<P (*)(P, P, P*, P*)>(api("il2cpp_runtime_invoke"));
  const auto object_unbox = reinterpret_cast<P (*)(P)>(api("il2cpp_object_unbox"));
  const auto object_get_class = reinterpret_cast<P (*)(P)>(api("il2cpp_object_get_class"));
  const auto string_new = reinterpret_cast<P (*)(const char*)>(api("il2cpp_string_new"));
  const auto string_chars = reinterpret_cast<const char16_t* (*)(P)>(api("il2cpp_string_chars"));
  const auto string_length = reinterpret_cast<int32_t (*)(P)>(api("il2cpp_string_length"));
  const auto format_exception = reinterpret_cast<void (*)(P, char*, int)>(api("il2cpp_format_exception"));
  if (missing) {
    report.Line("BRIDGE: exports missing, tests skipped");
    return 1;
  }

  int failures = 0;
  auto check = [&](bool ok, const char* what) {
    report.Line("%-52s %s", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };
  auto text = [&](P str) -> std::string {
    if (str == nullptr) return "(null)";
    return Win32::ToUtf8(std::u16string_view(string_chars(str), static_cast<size_t>(string_length(str))));
  };
  auto className = [&](P klass) {
    return std::string(class_get_namespace(klass)) + "." + class_get_name(klass);
  };
  auto describe = [&](P exc) {
    char message[512] {};
    format_exception(exc, message, sizeof(message));
    return className(object_get_class(exc)) + " (" + message + ")";
  };

  // 1. What the game is made of.
  P domain = domain_get();
  size_t count = 0;
  P* assemblies = domain_get_assemblies(domain, &count);
  size_t totalClasses = 0;
  P gameImage = nullptr;
  report.Line("%zu assemblies:", count);
  for (size_t i = 0; i < count; ++i) {
    P image = assembly_get_image(assemblies[i]);
    const char* name = image_get_name(image);
    const size_t classes = image_get_class_count(image);
    totalClasses += classes;
    report.Line("  %-44s %6zu classes", name, classes);
    if (std::string_view(name) == "Assembly-CSharp.dll") gameImage = image;
  }
  report.Line("%zu classes in all", totalClasses);
  check(count > 0 && totalClasses > 0, "assemblies and classes listed through the bridge");
  if (gameImage != nullptr) {
    const size_t classes = image_get_class_count(gameImage);
    report.Line("the game's own code (Assembly-CSharp), first classes:");
    for (size_t i = 0; i < classes && i < 12; ++i) report.Line("  %s", className(image_get_class(gameImage, i)).c_str());
  }

  // Finds a corlib method by class and name (first of the given names that exists: stripping may
  // have removed some).
  P corlib = get_corlib();
  auto method = [&](const char* ns, const char* cls, std::initializer_list<const char*> names, int argc) -> P {
    P klass = class_from_name(corlib, ns, cls);
    for (const char* name : names) {
      if (P m = klass ? class_get_method_from_name(klass, name, argc) : nullptr) return m;
    }
    report.Line("  %s.%s::%s/%d not found (stripped?)", ns, cls, *names.begin(), argc);
    return nullptr;
  };

  // 2. Static call into an il2cpp icall: Environment.ProcessorCount ends in our GetSystemInfo.
  if (P m = method("System", "Environment", {"get_ProcessorCount"}, 0)) {
    P exc = nullptr;
    P boxed = runtime_invoke(m, nullptr, nullptr, &exc);
    const int32_t cpus = (exc == nullptr && boxed) ? *static_cast<int32_t*>(object_unbox(boxed)) : -1;
    if (exc) report.Line("  threw %s", describe(exc).c_str());
    report.Line("  Environment.ProcessorCount = %d (this iPhone: %u)", cpus, std::thread::hardware_concurrency());
    check(cpus == static_cast<int32_t>(std::max(1u, std::thread::hardware_concurrency())),
          "managed static call -> icall -> mini Win32");
  } else {
    ++failures;
  }

  // 3. Argument round trip: Int32.Parse("1234").
  P parse = method("System", "Int32", {"Parse"}, 1);
  if (parse) {
    P args[1] = {string_new("1234")};
    P exc = nullptr;
    P boxed = runtime_invoke(parse, nullptr, args, &exc);
    const int32_t value = (exc == nullptr && boxed) ? *static_cast<int32_t*>(object_unbox(boxed)) : -1;
    if (exc) report.Line("  threw %s", describe(exc).c_str());
    report.Line("  Int32.Parse(\"1234\") = %d", value);
    check(value == 1234, "managed string argument, boxed int result");
  } else {
    ++failures;
  }

  // 4. Instance method on a managed string we created.
  if (P m = method("System", "String", {"ToUpperInvariant", "ToUpper"}, 0)) {
    P str = string_new("Fellworld on iPhone");
    P exc = nullptr;
    P result = runtime_invoke(m, str, nullptr, &exc);
    if (exc) report.Line("  threw %s", describe(exc).c_str());
    const std::string upper = exc ? "" : text(result);
    report.Line("  \"%s\".ToUpper = \"%s\"", text(str).c_str(), upper.c_str());
    check(upper == "FELLWORLD ON IPHONE", "managed instance call, string result");
  } else {
    ++failures;
  }

  // 5. A managed exception: il2cpp raises it as a C++ exception (_CxxThrowException -> our
  // RaiseException) and runtime_invoke catches it again (GameAssembly's __CxxFrameHandler + our
  // unwinder), handing it back in `exc`.
  if (parse) {
    P args[1] = {string_new("not a number")};
    P exc = nullptr;
    runtime_invoke(parse, nullptr, args, &exc);
    const std::string what = exc ? describe(exc) : "(no exception)";
    report.Line("  Int32.Parse(\"not a number\") threw %s", what.c_str());
    check(exc != nullptr && className(object_get_class(exc)) == "System.FormatException",
          "managed exception caught and returned (C++ EH + SEH)");
  } else {
    ++failures;
  }

  report.Line("BRIDGE: %s", failures ? "some checks failed (see above)" : "all checks passed");
  return failures;
}

} // namespace

extern "C" int ub_win32_device_has_run();

// Nonzero once the il2cpp probe has started in this process (its runtime then stays up until exit).
extern "C" int ub_il2cpp_probe_started() { return g_il2cppStarted.load() ? 1 : 0; }

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
  // il2cpp_init can run once per process, and the runtime it starts lives until the app exits. The
  // ubtest runner shares the mini Win32 (modules, TLS and unwind registries, thread runner) and starts
  // an engine of its own, so the two never run in the same process.
  if (g_il2cppStarted.exchange(true)) {
    report.Line("Already ran in this app session: il2cpp is up and stays up. Close and reopen the app to run again.");
    return finish(3);
  }
  if (ub_win32_device_has_run()) {
    g_il2cppStarted = false;
    report.Line("The Windows DLL test ran in this app session. Close and reopen the app, then run il2cpp_init first.");
    return finish(3);
  }

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

  // Kept for the process: the registered module's export lookup refers to this image.
  auto& loaded = *new Pe::LoadResult(
      Pe::Load(file, [](const Pe::Import& imp) { return Win32::Runtime::Current()->Resolve(imp.dll, imp.Symbol()); }));
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

  // The main guest thread outlives this call: il2cpp registers its stack with the GC.
  GuestThread& guest = *new GuestThread;
  t_bridgeGuest = &guest;  // native il2cpp calls from this thread run where il2cpp_init ran
  if (!guest.Create(*engine, &report)) {
    Win32::SetGuestCaller(nullptr);
    Win32::SetThreadRunner(nullptr);
    engine->Shutdown();
    g_engine = nullptr;
    return finish(1);
  }

  // Catch a fault inside il2cpp (or our handlers) and log the guest RIP before the process dies.
  InstallCrashHandler();
  report.Line("crash catcher installed (SIGSEGV/BUS/ABRT/ILL/FPE)");
  g_imageBase = reinterpret_cast<uintptr_t>(image.base);
  g_imageEnd = g_imageBase + image.size;
  MachFault::Install(g_imageBase, g_imageEnd);
  MachFault::RegisterThisThread();
  report.Line("mach exception handler installed (BAD_ACCESS/BAD_INSTRUCTION/ARITHMETIC, per thread)");

  // The loader runs the module's TLS callbacks and then DllMain with DLL_PROCESS_ATTACH.
  RunTlsCallbacksOn(guest, Win32::DLL_PROCESS_ATTACH);
  if (image.entryPoint) {
    const auto dll = CallWindows(guest, report, image.entryPoint,
                                 {reinterpret_cast<uintptr_t>(image.base), Win32::DLL_PROCESS_ATTACH, 0});
    report.Line("DllMain(DLL_PROCESS_ATTACH) returned %#llx", static_cast<unsigned long long>(dll.rax));
  }

  // Point il2cpp at the game's data folder (where it finds il2cpp_data/Metadata/global-metadata.dat).
  // The path is a Windows path mapping back to the device folder through the mini Win32.
  static const std::string dataDirWin = Win32::ToUtf8(Win32::WindowsPath(dataDir ? dataDir : ""));
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
      Il2cppBridge::Init([&image](std::string_view name) { return image.Export(name); }, &BridgeCall);
      RunBridgeTests(report);
    }
  } else {
    report.Line("il2cpp_init did not return (a fault, or the export is missing)");
  }

  const auto unimplemented = runtime.UnimplementedCalls();
  report.Line("unimplemented imports called during init: %zu", unimplemented.size());
  for (const auto& u : unimplemented) report.Line("  %s", u.c_str());

  const bool ok = initResult && *initResult != 0;
  report.Line("RESULT: %s", ok ? "il2cpp_init succeeded" : "il2cpp_init did not succeed (see above)");

  // No teardown from here on (see the top of the file): il2cpp's threads keep running guest code.
  // Only this host thread goes away, so it leaves fault handling; its guest stack and TEB stay.
  (void)engine.release();
  report.Line("guest threads still running: %d (engine kept alive for them)", g_guestThreads.load());
  MachFault::UnregisterThisThread();

  // Proof of life for the runtime: if the app survives, these show up in probe.log.
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::fprintf(stderr, "UB_IL2CPP still alive 5 s after il2cpp_init (%d guest threads running)\n", g_guestThreads.load());
    std::this_thread::sleep_for(std::chrono::seconds(25));
    std::fprintf(stderr, "UB_IL2CPP still alive 30 s after il2cpp_init (%d guest threads running)\n", g_guestThreads.load());
  }).detach();
  return finish(ok ? 0 : 1);
}
