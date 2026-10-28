// SPDX-License-Identifier: GPL-3.0-or-later
// Host test (x86-64 Linux only) for engine/icall_bridge: the x64 -> native reverse thunks. Binds
// native System V functions (standing in for libunity's ARM64 engine functions) through the bridge,
// then calls the resulting guest stubs the way GameAssembly would -- as Microsoft x64 functions, so
// arguments really arrive in rcx/rdx/r8/r9 + xmm0-3 + stack and the thunk must re-lay them out.
// Checks integer, pointer, float and double arguments, their interleaving (independent register
// counters on the native side), and integer vs floating returns.
#include "engine/icall_bridge.h"
#include "win32/hle.h"

#include <cstdint>
#include <cstdio>
#include <sys/mman.h>
#include <vector>

extern "C" void ub_win32_host_entry();

namespace {

using UnityBridge::IcallBridge::Kind;
using UnityBridge::IcallBridge::Signature;
namespace Bridge = UnityBridge::IcallBridge;

int g_failures = 0;
void Check(bool ok, const char* what) {
  std::printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
  std::fflush(stdout);
  if (!ok) ++g_failures;
}

// Stand-ins for libunity's native engine functions. The System V convention here plays the part of
// AAPCS64: integer and floating arguments are counted in separate registers, exactly what the thunk
// must reproduce from Microsoft x64's positional slots.
int64_t AddInts(int64_t a, int64_t b, int64_t c) { return a + 100 * b + 10000 * c; }
double MixSum(int64_t n, double x, int64_t m, double y) { return n + 10 * x + 100 * m + 1000 * y; }
void* PickPointer(void* a, int64_t which, void* b) { return which ? b : a; }
double ScaleFloat(float x) { return static_cast<double>(x) * 2.0; }  // a 32-bit float argument
int64_t ManyInts(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f) {
  return a - b + c - d + e - f;  // six integer args: the System V limit the host trampoline supports
}

Signature Sig(std::initializer_list<Kind> args, Kind ret) {
  Signature s;
  s.ret = ret;
  for (Kind k : args) s.args[s.argCount++] = k;
  return s;
}

} // namespace

int main() {
  // A runtime whose stubs jump into the host entry, so a stub is callable as a native ms_abi
  // function -- the same path the Windows DLL host test uses.
  void* stubs = mmap(nullptr, 64 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
  Check(stubs != MAP_FAILED, "stub memory");
  UnityBridge::Win32::Runtime runtime(UnityBridge::Win32::StubKind::HostNative,
                                      {static_cast<uint8_t*>(stubs), 64 * 1024},
                                      reinterpret_cast<uintptr_t>(&ub_win32_host_entry));
  UnityBridge::Win32::Runtime::SetCurrent(&runtime);
  Bridge::Init(&runtime);

  const int I = static_cast<int>(Kind::Int), F = static_cast<int>(Kind::Float);
  (void)I;
  (void)F;

  using I3 = int64_t(__attribute__((ms_abi))*)(int64_t, int64_t, int64_t);
  auto addInts = reinterpret_cast<I3>(*Bridge::Make(reinterpret_cast<uintptr_t>(&AddInts),
                                                    Sig({Kind::Int, Kind::Int, Kind::Int}, Kind::Int)));
  Check(addInts != nullptr && addInts(7, 3, 5) == 50307, "three integer arguments, integer return");

  using Mix = double(__attribute__((ms_abi))*)(int64_t, double, int64_t, double);
  auto mix = reinterpret_cast<Mix>(*Bridge::Make(reinterpret_cast<uintptr_t>(&MixSum),
                                                 Sig({Kind::Int, Kind::Float, Kind::Int, Kind::Float}, Kind::Float)));
  Check(mix != nullptr && mix(1, 2.0, 3, 4.0) == 1 + 20 + 300 + 4000,
        "interleaved int/double arguments, double return");

  using Pick = void*(__attribute__((ms_abi))*)(void*, int64_t, void*);
  auto pick = reinterpret_cast<Pick>(*Bridge::Make(reinterpret_cast<uintptr_t>(&PickPointer),
                                                   Sig({Kind::Int, Kind::Int, Kind::Int}, Kind::Int)));
  int a = 0, b = 0;
  Check(pick && pick(&a, 0, &b) == &a && pick(&a, 1, &b) == &b, "pointer arguments and pointer return");

  using Scale = double(__attribute__((ms_abi))*)(float);
  auto scale = reinterpret_cast<Scale>(*Bridge::Make(reinterpret_cast<uintptr_t>(&ScaleFloat),
                                                     Sig({Kind::Float}, Kind::Float)));
  Check(scale && scale(2.5f) == 5.0, "32-bit float argument, double return");

  using I6 = int64_t(__attribute__((ms_abi))*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
  auto many = reinterpret_cast<I6>(*Bridge::Make(reinterpret_cast<uintptr_t>(&ManyInts),
                                                 Sig({Kind::Int, Kind::Int, Kind::Int, Kind::Int, Kind::Int, Kind::Int},
                                                     Kind::Int)));
  Check(many && many(10, 1, 20, 2, 30, 3) == 10 - 1 + 20 - 2 + 30 - 3,
        "six integer arguments (two from the Microsoft stack)");

  if (g_failures) std::printf("RESULT: %d checks failed\n", g_failures);
  else std::printf("RESULT: all checks passed\n");
  return g_failures ? 1 : 0;
}
