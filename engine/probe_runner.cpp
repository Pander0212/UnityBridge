// SPDX-License-Identifier: GPL-3.0-or-later
// M2b device probe: starts FEXCore through AetherPS4's GuestEngine and runs its built-in
// controlled harness (x86-64 integer/flags/SSE, an HLE bridge call through `syscall`, guest
// threads with TLS, unaligned access and code invalidation). Based on AetherPS4's
// runtime/probes/fexcore-guest-harness.cpp (MIT).

#include "core/fex/fex_guest_engine.h"

#include <FEXCore/Core/X86Enums.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/sysctl.h>
#include <unistd.h>
#include <variant>

extern "C" int csops(pid_t pid, unsigned int ops, void* useraddr, size_t usersize);

namespace {

constexpr uint64_t kBridgeOperation = 0xB4C4'F001ULL;
constexpr uint64_t kBridgeArgument = 0x1020'3040'5060'7080ULL;
constexpr uint64_t kBridgeResult = 0x8877'6655'4433'2211ULL;

class ProbeBridge final : public AetherPS4::Fex::GuestBridge {
public:
  AetherPS4::Fex::EngineResult<bool> Invoke(Core::GuestCpu::HleCallFrame& frame) override {
    if (frame.operation != kBridgeOperation || frame.gpr[FEXCore::X86State::REG_RDI] != kBridgeArgument) {
      return AetherPS4::Fex::EngineFailure {AetherPS4::Fex::EngineStage::Bridge, ENOSYS};
    }
    frame.gpr[FEXCore::X86State::REG_RAX] = kBridgeResult;
    return true;
  }
};

const char* StageName(AetherPS4::Fex::EngineStage stage) {
  switch (stage) {
  case AetherPS4::Fex::EngineStage::Request: return "request";
  case AetherPS4::Fex::EngineStage::Config: return "config";
  case AetherPS4::Fex::EngineStage::Context: return "context";
  case AetherPS4::Fex::EngineStage::Mapping: return "mapping";
  case AetherPS4::Fex::EngineStage::Thread: return "thread";
  case AetherPS4::Fex::EngineStage::Execute: return "execute";
  case AetherPS4::Fex::EngineStage::Bridge: return "bridge";
  case AetherPS4::Fex::EngineStage::Invalidate: return "invalidate";
  case AetherPS4::Fex::EngineStage::Teardown: return "teardown";
  }
  return "?";
}

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
    std::fprintf(stderr, "UB_PROBE %s\n", buf);
  }
};

int Fail(Report& report, const char* what, const AetherPS4::Fex::EngineFailure& failure) {
  report.Line("FAIL %s: stage=%s error=%d", what, StageName(failure.Stage), failure.Error);
  return 1;
}

} // namespace

extern "C" bool ub_probe_jit_attached(void) {
  uint32_t flags = 0;
  // CS_OPS_STATUS = 0; CS_DEBUGGED = 0x10000000: set while (or after) a debugger is attached.
  return csops(getpid(), 0, &flags, sizeof(flags)) == 0 && (flags & 0x10000000u) != 0;
}

// Whether a debugger is attached right now (P_TRACED). CS_DEBUGGED stays set after a debugger
// detaches, e.g. when JIT was enabled from StikDebug's own app list: then nobody answers the
// JIT request (brk #0xf00d) and the app dies with SIGTRAP (seen on iPhone 15, iOS 26.6.2).
extern "C" bool ub_probe_debugger_attached(void) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  struct kinfo_proc info {};
  size_t size = sizeof(info);
  if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0) return false;
  return (info.kp_proc.p_flag & P_TRACED) != 0;
}

// Runs the probe and writes a human-readable report into out. Returns 0 when every check passed.
extern "C" int ub_probe_run(char* out, size_t capacity) {
  Report report;
  auto finish = [&](int status) {
    std::snprintf(out, capacity, "%s", report.text.c_str());
    return status;
  };

  report.Line("UnityBridge probe M2b, page size %ld", sysconf(_SC_PAGESIZE));
  if (!ub_probe_jit_attached()) {
    report.Line("FAIL no JIT: this app is not marked as debugged. Enable JIT for it in StikDebug first.");
    return finish(2);
  }
  report.Line("JIT: debugger flag set");
  if (!ub_probe_debugger_attached()) {
    report.Line("FAIL no debugger attached: JIT was enabled, but StikDebug is not attached any more, so nobody "
                "would answer the JIT request. Tap \"Enable JIT\" in this app (not in StikDebug's list).");
    return finish(3);
  }
  report.Line("JIT: debugger attached");

  static ProbeBridge bridge;
  auto created = AetherPS4::Fex::GuestEngine::Create(bridge);
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&created)) {
    return finish(Fail(report, "create engine", *failure));
  }
  auto engine = std::move(std::get<std::unique_ptr<AetherPS4::Fex::GuestEngine>>(created));
  report.Line("FEXCore context created");

  auto run = engine->RunControlledHarness();
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&run)) {
    return finish(Fail(report, "controlled harness", *failure));
  }
  const auto result = std::get<AetherPS4::Fex::GuestRunResult>(run);
  report.Line("x86-64 integer      %s", result.Gpr ? "ok" : "FAIL");
  report.Line("x86-64 flags        %s", result.Rflags ? "ok" : "FAIL");
  report.Line("SSE (addsd)         %s", result.Xmm ? "ok" : "FAIL");
  report.Line("bridge (syscall)    %s", result.Bridge ? "ok" : "FAIL");
  report.Line("guest threads       %s", result.Threads ? "ok" : "FAIL");
  report.Line("guest TLS (fs:)     %s", result.Tls ? "ok" : "FAIL");
  report.Line("unaligned access    %s", result.Unaligned ? "ok" : "FAIL");
  report.Line("code invalidation   %s", result.Invalidation ? "ok" : "FAIL");
  const bool all = result.Gpr && result.Rflags && result.Xmm && result.Bridge && result.Threads &&
                   result.Tls && result.Unaligned && result.Invalidation;

  auto shutdown = engine->Shutdown();
  if (const auto* failure = std::get_if<AetherPS4::Fex::EngineFailure>(&shutdown)) {
    return finish(Fail(report, "shutdown", *failure));
  }
  report.Line(all ? "RESULT: all checks passed" : "RESULT: some checks failed");
  return finish(all ? 0 : 1);
}
