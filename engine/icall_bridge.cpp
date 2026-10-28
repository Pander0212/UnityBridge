// SPDX-License-Identifier: GPL-3.0-or-later
// x64 -> ARM64 reverse thunks for il2cpp InternalCalls: see icall_bridge.h.
//
// Each registered function gets a slot and one template-instantiated handler; handler I marshals
// for the thunk in slot I. The handler reads the Microsoft x64 arguments from the CallContext the
// guest stub captured, lays them out for AAPCS64 by the signature, and calls the native function
// through ub_call_native (engine/call_native_*.S), which loads the argument registers and returns
// the integer and floating results together so the handler can pick the one the signature names.
#include "engine/icall_bridge.h"
#include "win32/hle.h"

#include <array>
#include <cstdio>
#include <mutex>
#include <utility>

// Loads gpr[0..7] into x0-x7 (rdi,rsi,rdx,rcx,r8,r9 on the host) and fpr[0..7] into d0-d7 (xmm0-7),
// calls `fn`, and writes its integer result (x0/rax) and floating result (d0/xmm0) to `out` as
// {uint64_t, double}. In engine/call_native_arm64.S (device) and call_native_sysv.S (host test).
extern "C" void ub_call_native(void* fn, const uint64_t gpr[8], const double fpr[8], void* out);

namespace UnityBridge::IcallBridge {
namespace {

using Win32::CallContext;

struct Thunk {
  uintptr_t fn = 0;
  Signature sig;
};

// Integer result in x0/rax, floating result in d0/xmm0: the trampoline fills both and the handler
// keeps the one the signature asks for, since one native call cannot be read two ways afterwards.
struct NativeResult {
  uint64_t integer;
  double floating;
};

std::mutex g_mutex;
Win32::Runtime* g_runtime = nullptr;
Thunk g_thunks[kMaxThunks];
size_t g_used = 0;

uint64_t Marshal(size_t index, CallContext& c) {
  const Thunk& t = g_thunks[index];
  uint64_t gpr[8] = {};
  double fpr[8] = {};
  unsigned ngrn = 0, nsrn = 0;  // next integer / floating register, AAPCS64's two counters
  for (unsigned i = 0; i < t.sig.argCount; ++i) {
    if (t.sig.args[i] == Kind::Float) {
      if (nsrn < 8) fpr[nsrn++] = c.ArgDouble(i);
    } else {
      if (ngrn < 8) gpr[ngrn++] = c.Arg(i);
    }
  }
  NativeResult result {};
  ub_call_native(reinterpret_cast<void*>(t.fn), gpr, fpr, &result);
  if (t.sig.ret == Kind::Float) {
    c.ReturnDouble(result.floating);
    return 0;  // rax is unused for a floating return
  }
  return result.integer;
}

template <size_t I>
uint64_t HandlerAt(CallContext& c) {
  return Marshal(I, c);
}

template <size_t... I>
constexpr std::array<Win32::Handler, sizeof...(I)> MakeHandlers(std::index_sequence<I...>) {
  return {&HandlerAt<I>...};
}

constexpr auto kHandlers = MakeHandlers(std::make_index_sequence<kMaxThunks>());

} // namespace

void Init(Win32::Runtime* runtime) {
  std::scoped_lock lock {g_mutex};
  g_runtime = runtime;
  g_used = 0;
}

std::optional<uintptr_t> Make(uintptr_t fn, const Signature& signature) {
  std::scoped_lock lock {g_mutex};
  if (g_runtime == nullptr || g_used == kMaxThunks) {
    std::fprintf(stderr, "UB_ICALL_BRIDGE no thunk for %#lx (%s)\n", static_cast<unsigned long>(fn),
                 g_runtime == nullptr ? "not bound" : "slots full");
    return std::nullopt;
  }
  const size_t index = g_used;
  g_thunks[index] = {fn, signature};
  const auto stub = g_runtime->BindCallback(kHandlers[index], "il2cpp_icall");
  if (!stub) {
    std::fprintf(stderr, "UB_ICALL_BRIDGE runtime out of stub space at thunk %zu\n", index);
    return std::nullopt;
  }
  ++g_used;
  return stub;
}

} // namespace UnityBridge::IcallBridge
