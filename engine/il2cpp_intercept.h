// SPDX-License-Identifier: GPL-3.0-or-later
// The il2cpp_* functions that take host function pointers (M3c). libunity hands these ARM64
// pointers to the runtime -- InternalCalls through il2cpp_add_internal_call, plus memory, log,
// plugin and GC callbacks -- and the x86-64 GameAssembly.dll later calls them. Each such API gets a
// native replacement here (installed with Il2cppBridge::Override) that wraps every pointer in a
// reverse thunk (IcallBridge::Make) and passes the guest stub on to the real export.
//
// InternalCalls carry no signature of their own, so it comes from the il2cpp metadata: the
// registered name ("Namespace.Class::Method", "Ns.Outer/Inner::Method" or with a "(Type,Type)"
// parameter list) is looked up through the metadata API, and the method's parameter and return
// types are classified for the thunk (DeriveSignature). Registrations made before il2cpp_init has
// run (when the metadata is not loaded yet) are kept and made once the intercepted il2cpp_init
// returns successfully.
//
// Not intercepted: il2cpp_unity_install_unitytls_interface (a version-specific table of ~60 TLS
// functions, some with structs by value) and il2cpp_register_debugger_agent_transport (M1: left out).
// They stay refused by the bridge.
#pragma once

#include "engine/icall_bridge.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace UnityBridge::Il2cppIntercept {

// Native (ARM64) entry point of il2cpp API function `name`, or nullptr.
using Resolve = std::function<void*(std::string_view name)>;

// The il2cpp_* functions this module replaces.
std::span<const std::string_view> Intercepted();

// `api` gives the metadata API used for the lookups (Il2cppBridge::Symbol on the device); `original`
// gives the real, unintercepted exports (Il2cppBridge::Forwarder). `runtimeUp`: il2cpp_init has
// already succeeded, so InternalCalls can be resolved at once. IcallBridge must be initialised.
// Clears earlier state (pending registrations, wrapped callbacks, counters).
void Init(Resolve api, Resolve original, bool runtimeUp);

// The native replacement for intercepted API `name`, or nullptr if `name` is not intercepted.
void* Replacement(std::string_view name);

// The thunk signature of InternalCall `name` from the metadata, or nullopt with the reason in
// `error` (not in the metadata: stripped or never declared; or a type the thunk cannot carry).
std::optional<IcallBridge::Signature> DeriveSignature(std::string_view name, std::string* error);

struct Stats {
  size_t registered = 0;  // passed on to il2cpp with a reverse thunk
  size_t absent = 0;      // no such InternalCall in the metadata (stripped): not registered
  size_t failed = 0;      // in the metadata but not bridgeable (struct by value, stack arguments, ...)
  size_t pending = 0;     // waiting for il2cpp_init
  size_t callbacks = 0;   // other host callbacks wrapped
};
Stats GetStats();

} // namespace UnityBridge::Il2cppIntercept
