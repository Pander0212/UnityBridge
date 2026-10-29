// SPDX-License-Identifier: GPL-3.0-or-later
// ARM64 -> x64 bridge for the il2cpp C API (M3). Android's libunity.so calls the il2cpp_* functions
// of libil2cpp.so by name; here those functions live in the game's x86-64 GameAssembly.dll, running
// under FEXCore. Symbol(name) returns a native ARM64 function pointer -- what dlsym("libil2cpp.so",
// name) will hand libunity -- whose call runs the export on the calling host thread's guest thread.
//
// All il2cpp_* functions libunity uses take and return only integers and pointers (M1), so one
// generic thunk shape serves them: the eight AAPCS64 argument registers x0-x7 become the Microsoft
// x64 arguments rcx, rdx, r8, r9 and four stack slots; rax comes back in x0. Callers that pass fewer
// arguments leave garbage in the remaining slots, which the callee never reads. Functions that take
// a host callback cannot go through this shape (the guest would run ARM64 code as x86): Override
// installs a native replacement for such a name (engine/il2cpp_intercept wraps the callbacks in
// reverse thunks and calls the real export through Forwarder); without one they log and return 0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace UnityBridge::Il2cppBridge {

// Export lookup of the loaded GameAssembly.dll (guest address by name).
using ExportLookup = std::function<std::optional<uintptr_t>(std::string_view)>;

// Runs guest function `target` with the Microsoft x64 convention on the calling host thread's guest
// thread: args[0..3] in rcx, rdx, r8, r9, args[4..7] on the stack. Returns rax.
using GuestCall = uint64_t (*)(uintptr_t target, const uint64_t (&args)[8]);

// Binds the bridge to a module and a way to run guest code. Clears earlier bindings.
void Init(ExportLookup lookup, GuestCall caller);

// The ARM64 entry point for il2cpp export `name`, or nullptr if the module does not export it (or
// every thunk slot is taken). Stable: the same name always yields the same pointer. A name with an
// Override yields the override (if the module exports the name).
void* Symbol(std::string_view name);

// From now on Symbol(name) returns `native` instead of the forwarding thunk. Must be set before
// Symbol(name) is first handed out to take effect for every caller. Init clears all overrides.
void Override(std::string_view name, void* native);

// A thunk that always forwards to the guest export `name`, even for a host-callback API and past
// an Override: what a replacement uses to call the real function. nullptr if not exported.
void* Forwarder(std::string_view name);

// True for the il2cpp_* functions that take a host callback (refused unless overridden).
bool TakesHostCallback(std::string_view name);

constexpr size_t kMaxFunctions = 512;  // GameAssembly exports 235 il2cpp_* functions (M0)

} // namespace UnityBridge::Il2cppBridge
