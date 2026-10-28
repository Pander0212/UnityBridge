// SPDX-License-Identifier: GPL-3.0-or-later
// ARM64 -> x64 bridge for the il2cpp C API: see il2cpp_bridge.h.
//
// Each bound function gets a slot and one of kMaxFunctions template-instantiated thunks; thunk I
// forwards its eight register arguments to the guest export bound to slot I. No code is generated at
// run time, so the thunks are ordinary signed ARM64 functions, safe to hand to libunity.
#include "engine/il2cpp_bridge.h"

#include <array>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace UnityBridge::Il2cppBridge {
namespace {

using Word = uint64_t;
using Thunk = Word (*)(Word, Word, Word, Word, Word, Word, Word, Word);

struct Slot {
  std::string name;
  uintptr_t target = 0;  // guest address of the export
  bool blocked = false;  // takes a host callback: refused (a Forwarder slot never is)
  bool forwarder = false;
};

struct OverrideEntry {
  std::string name;
  void* native = nullptr;
};

// The il2cpp_* functions that hand the runtime a host function pointer (M1, plus the InternalCall
// registration): the guest would execute ARM64 code as x86, so they need an Override.
constexpr std::string_view kHostCallbackApis[] = {
    "il2cpp_add_internal_call",
    "il2cpp_set_memory_callbacks",
    "il2cpp_register_log_callback",
    "il2cpp_set_find_plugin_callback",
    "il2cpp_unity_install_unitytls_interface",
    "il2cpp_gc_foreach_heap",
    "il2cpp_gchandle_foreach_get_target",
    "il2cpp_gc_set_external_allocation_tracker",
    "il2cpp_gc_set_external_wbarrier_tracker",
    "il2cpp_register_debugger_agent_transport",
};

std::mutex g_mutex;
ExportLookup g_lookup;
GuestCall g_caller = nullptr;
Slot g_slots[kMaxFunctions];
size_t g_used = 0;
std::vector<OverrideEntry> g_overrides;

// A slot is complete before its thunk is handed out (under g_mutex), and never changes afterwards.
Word Forward(size_t index, const Word (&args)[8]) {
  const Slot& slot = g_slots[index];
  if (slot.blocked || g_caller == nullptr) {
    std::fprintf(stderr, "UB_IL2CPP_BRIDGE %s: %s, returning 0\n", slot.name.c_str(),
                 slot.blocked ? "takes a host callback and has no override" : "no guest caller");
    return 0;
  }
  return g_caller(slot.target, args);
}

template <size_t I>
Word ThunkAt(Word a0, Word a1, Word a2, Word a3, Word a4, Word a5, Word a6, Word a7) {
  const Word args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
  return Forward(I, args);
}

template <size_t... I>
constexpr std::array<Thunk, sizeof...(I)> MakeThunks(std::index_sequence<I...>) {
  return {&ThunkAt<I>...};
}

constexpr auto kThunks = MakeThunks(std::make_index_sequence<kMaxFunctions>());

} // namespace

bool TakesHostCallback(std::string_view name) {
  for (const auto api : kHostCallbackApis) {
    if (api == name) return true;
  }
  return false;
}

void Init(ExportLookup lookup, GuestCall caller) {
  std::scoped_lock lock {g_mutex};
  g_lookup = std::move(lookup);
  g_caller = caller;
  for (size_t i = 0; i < g_used; ++i) g_slots[i] = {};
  g_used = 0;
  g_overrides.clear();
}

namespace {

// The thunk for `name` of the given kind, binding a slot on first use. Caller holds g_mutex.
void* ThunkFor(std::string_view name, bool forwarder) {
  for (size_t i = 0; i < g_used; ++i) {
    if (g_slots[i].name == name && g_slots[i].forwarder == forwarder) return reinterpret_cast<void*>(kThunks[i]);
  }
  const auto target = g_lookup ? g_lookup(name) : std::nullopt;
  if (!target) return nullptr;
  if (g_used == kMaxFunctions) {
    std::fprintf(stderr, "UB_IL2CPP_BRIDGE no thunk left for %.*s\n", static_cast<int>(name.size()), name.data());
    return nullptr;
  }
  g_slots[g_used] = {std::string(name), *target, !forwarder && TakesHostCallback(name), forwarder};
  return reinterpret_cast<void*>(kThunks[g_used++]);
}

} // namespace

void* Symbol(std::string_view name) {
  std::scoped_lock lock {g_mutex};
  for (const auto& o : g_overrides) {
    if (o.name == name) return g_lookup && g_lookup(name) ? o.native : nullptr;
  }
  return ThunkFor(name, false);
}

void Override(std::string_view name, void* native) {
  std::scoped_lock lock {g_mutex};
  for (auto& o : g_overrides) {
    if (o.name == name) {
      o.native = native;
      return;
    }
  }
  g_overrides.push_back({std::string(name), native});
}

void* Forwarder(std::string_view name) {
  std::scoped_lock lock {g_mutex};
  return ThunkFor(name, true);
}

} // namespace UnityBridge::Il2cppBridge
