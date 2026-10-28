// SPDX-License-Identifier: GPL-3.0-or-later
// Host test (x86-64 Linux only) for engine/il2cpp_bridge: binds fake il2cpp_* "exports" -- native
// functions with the Microsoft x64 convention, so arguments 5-8 really travel on the stack as they
// do under FEXCore's CallWin64 -- and calls them through the bridge's function pointers the way
// libunity will. Checks argument order for all eight slots, short signatures, stable pointers,
// missing exports, the blocked host-callback APIs, and slot exhaustion.
#include "engine/il2cpp_bridge.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

namespace {

using UnityBridge::Il2cppBridge::GuestCall;
namespace Bridge = UnityBridge::Il2cppBridge;

int g_failures = 0;
void Check(bool ok, const char* what) {
  std::printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++g_failures;
}

#define MS __attribute__((ms_abi))
using Win64Fn = uint64_t(MS*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

MS uint64_t Weighted8(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;  // order-sensitive
}
MS uint64_t MulAdd3(uint64_t a, uint64_t b, uint64_t c) { return a * b + c; }
MS uint64_t HighGarbage() { return 0xdeadbeef00000005ull; }  // an int32 result with junk above it
MS uint64_t Pointer() { return 0x0000123456789abcull; }

int g_guestCalls = 0;
uint64_t NativeCall(uintptr_t target, const uint64_t (&a)[8]) {
  ++g_guestCalls;
  return reinterpret_cast<Win64Fn>(target)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}

std::map<std::string, uintptr_t> g_exports = {
    {"il2cpp_weighted8", reinterpret_cast<uintptr_t>(&Weighted8)},
    {"il2cpp_muladd3", reinterpret_cast<uintptr_t>(&MulAdd3)},
    {"il2cpp_high_garbage", reinterpret_cast<uintptr_t>(&HighGarbage)},
    {"il2cpp_pointer", reinterpret_cast<uintptr_t>(&Pointer)},
    {"il2cpp_register_log_callback", reinterpret_cast<uintptr_t>(&Pointer)},  // exported, but blocked
};

std::optional<uintptr_t> Lookup(std::string_view name) {
  if (const auto it = g_exports.find(std::string(name)); it != g_exports.end()) return it->second;
  if (name.starts_with("il2cpp_filler_")) return reinterpret_cast<uintptr_t>(&Pointer);
  return std::nullopt;
}

} // namespace

int main() {
  Bridge::Init(&Lookup, &NativeCall);

  using Fn8 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
  auto* weighted = reinterpret_cast<Fn8>(Bridge::Symbol("il2cpp_weighted8"));
  Check(weighted != nullptr, "bound an exported function");
  Check(weighted && weighted(1, 10, 100, 1000, 10000, 100000, 1000000, 10000000) == 87654321,
        "eight arguments arrive in order (4 on stack)");
  Check(Bridge::Symbol("il2cpp_weighted8") == reinterpret_cast<void*>(weighted), "same name, same pointer");

  auto* mulAdd = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t)>(Bridge::Symbol("il2cpp_muladd3"));
  Check(mulAdd && reinterpret_cast<void*>(mulAdd) != reinterpret_cast<void*>(weighted), "second function, own thunk");
  Check(mulAdd && mulAdd(6, 7, 8) == 50, "short signature (3 args) through the thunk");

  auto* narrow = reinterpret_cast<int32_t (*)()>(Bridge::Symbol("il2cpp_high_garbage"));
  Check(narrow && narrow() == 5, "int32 result: junk above it is ignored");
  auto* pointer = reinterpret_cast<void* (*)()>(Bridge::Symbol("il2cpp_pointer"));
  Check(pointer && pointer() == reinterpret_cast<void*>(0x0000123456789abcull), "pointer result");

  Check(Bridge::Symbol("il2cpp_not_exported") == nullptr, "missing export gives nullptr");

  const int before = g_guestCalls;
  auto* logCallback = reinterpret_cast<uint64_t (*)(void*)>(Bridge::Symbol("il2cpp_register_log_callback"));
  Check(Bridge::TakesHostCallback("il2cpp_register_log_callback") && logCallback != nullptr,
        "host-callback API is bound but marked");
  Check(logCallback && logCallback(nullptr) == 0 && g_guestCalls == before,
        "host-callback API returns 0, guest not called");

  // Fill every remaining slot; the next distinct name finds none.
  bool allBound = true;
  for (size_t i = 0;; ++i) {
    const std::string name = "il2cpp_filler_" + std::to_string(i);
    if (Bridge::Symbol(name) == nullptr) {
      allBound = i + 5 == Bridge::kMaxFunctions;  // the five bound above
      break;
    }
  }
  Check(allBound, "slots run out at kMaxFunctions");
  Check(Bridge::Symbol("il2cpp_weighted8") == reinterpret_cast<void*>(weighted), "bound names still resolve when full");

  Bridge::Init(&Lookup, &NativeCall);
  auto* again = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t)>(Bridge::Symbol("il2cpp_muladd3"));
  Check(again != nullptr && again(2, 3, 4) == 10, "Init starts over");

  if (g_failures) std::printf("RESULT: %d checks failed\n", g_failures);
  else std::printf("RESULT: all checks passed\n");
  return g_failures ? 1 : 0;
}
