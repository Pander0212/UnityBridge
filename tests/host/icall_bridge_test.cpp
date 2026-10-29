// SPDX-License-Identifier: GPL-3.0-or-later
// Host test (x86-64 Linux only) for engine/icall_bridge: the x64 -> native reverse thunks. Binds
// native System V functions (standing in for libunity's ARM64 engine functions) through the bridge,
// then calls the resulting guest stubs the way GameAssembly would -- as Microsoft x64 functions, so
// arguments really arrive in rcx/rdx/r8/r9 + xmm0-3 + stack and the thunk must re-lay them out.
// Checks integer, pointer, float and double arguments, their interleaving (independent register
// counters on the native side), integer vs floating returns, narrow integers (extended in both
// directions), and the refusal of signatures that would need AAPCS64 stack arguments.
#include "engine/icall_bridge.h"
#include "win32/hle.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

// Narrow arguments: the native side may assume them extended (Apple arm64 does, to 32 bits).
int64_t Narrow(uint8_t a, int8_t b, uint16_t c, int16_t d) { return a * 1000000 + b * 10000 + c + d; }
int8_t NegByte(int64_t x) { return static_cast<int8_t>(x); }  // leaves junk above al in eax

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

  using N4 = int64_t(__attribute__((ms_abi))*)(uint64_t, uint64_t, uint64_t, uint64_t);
  auto narrow = reinterpret_cast<N4>(*Bridge::Make(reinterpret_cast<uintptr_t>(&Narrow),
                                                   Sig({Kind::UInt8, Kind::Int8, Kind::UInt16, Kind::Int16}, Kind::Int)));
  Check(narrow && narrow(0xffffff07, 0xffff00fe, 0xdead0010, 0x1234fffd) == 7 * 1000000 - 2 * 10000 + 16 - 3,
        "narrow arguments extended (junk above them dropped)");
  using R1 = uint64_t(__attribute__((ms_abi))*)(int64_t);
  auto neg = reinterpret_cast<R1>(*Bridge::Make(reinterpret_cast<uintptr_t>(&NegByte), Sig({Kind::Int}, Kind::Int8)));
  Check(neg && neg(0x12345680) == 0xffffffffffffff80ull, "narrow result sign-extended to rax");

  Signature nineInts;
  nineInts.argCount = 9;
  Signature eightAndEight;
  eightAndEight.argCount = 16;
  for (unsigned i = 8; i < 16; ++i) eightAndEight.args[i] = Kind::Float;
  Check(!Bridge::FitsRegisters(nineInts) && Bridge::StackSlots(nineInts) == 1 && Bridge::Supported(nineInts) &&
            Bridge::Make(reinterpret_cast<uintptr_t>(&ManyInts), nineInts),
        "nine integer arguments: one AAPCS64 stack slot, a thunk is made");
  Check(Bridge::FitsRegisters(eightAndEight) && Bridge::StackSlots(eightAndEight) == 0,
        "eight integer plus eight floating arguments fit");

  // AAPCS64 layout past the registers. Arguments alternate Int, Float, ... (12 each): integers 0-7
  // and floats 0-7 take x0-x7 / d0-d7, the rest go to the stack in argument order, mixed.
  {
    Signature mixed;
    mixed.argCount = 24;
    uint64_t raw[24];
    for (unsigned i = 0; i < 24; ++i) {
      mixed.args[i] = (i % 2) ? Kind::Float : Kind::Int;
      const double d = 0.5 + i;
      if (i % 2) std::memcpy(&raw[i], &d, sizeof(d));
      else raw[i] = 1000 + i;
    }
    mixed.args[18] = Kind::Int8;  // a narrow integer on the stack is extended too
    raw[18] = 0xabcdef80;
    const auto l = Bridge::LayOut(mixed, raw);
    bool ok = l.stackSlots == 8 && Bridge::StackSlots(mixed) == 8;
    for (unsigned k = 0; k < 8; ++k) ok &= l.gpr[k] == 1000 + 2 * k && l.fpr[k] == 0.5 + (2 * k + 1);
    // Past the registers: arguments 16..23, in order: Int 16, Float 17, Int8 18, Float 19, ...
    uint64_t f17, f19;
    const double d17 = 17.5, d19 = 19.5;
    std::memcpy(&f17, &d17, 8);
    std::memcpy(&f19, &d19, 8);
    ok &= l.stack[0] == 1016 && l.stack[1] == f17 && l.stack[2] == 0xffffffffffffff80ull && l.stack[3] == f19;
    ok &= l.stack[4] == 1020 && l.stack[6] == 1022;
    Check(ok, "AAPCS64: 8+8 in registers, the rest on the stack in argument order (narrow ones extended)");

    Signature tooMany;
    tooMany.argCount = 33;
    Check(!Bridge::Supported(tooMany) && !Bridge::Make(reinterpret_cast<uintptr_t>(&ManyInts), tooMany),
          "more than kMaxArgs arguments refused");
  }

  if (g_failures) std::printf("RESULT: %d checks failed\n", g_failures);
  else std::printf("RESULT: all checks passed\n");
  return g_failures ? 1 : 0;
}
