// SPDX-License-Identifier: GPL-3.0-or-later
// x64 -> ARM64 reverse thunks for il2cpp InternalCalls (M3b). The game's x86-64 GameAssembly.dll
// calls engine functions that Android's libunity.so provides as native ARM64 (AAPCS64) functions,
// registered through il2cpp_add_internal_call. Make(fn, signature) returns a guest x86 stub that,
// called with the Microsoft x64 convention, moves the arguments into AAPCS64 registers by the
// signature and calls `fn`, then returns its result to the x86 caller.
//
// Microsoft x64 gives each argument a positional slot (ints in rcx/rdx/r8/r9, floats in xmm0-3,
// the rest on the stack); AAPCS64 fills x0-x7 and d0-d7 by two independent counters. So the thunk
// walks the arguments and, per the signature, reads each from its Microsoft slot (CallContext::Arg
// or ArgDouble) and appends it to the next integer or floating register. Up to eight of each; the
// 16 engine functions with more arguments than that (M1) are not handled yet.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace UnityBridge::Win32 { class Runtime; }

namespace UnityBridge::IcallBridge {

// Argument and return kinds that differ between the conventions. Pointers and every integer width
// are Int; float and double are Float (a 32-bit float still occupies a full register slot either
// way, so the thunk treats r4 and r8 the same). Void returns use Int and the result is ignored.
enum class Kind : uint8_t { Int, Float };

struct Signature {
  uint8_t argCount = 0;  // including the instance pointer for an instance method
  Kind args[8] = {};     // args[i] is argument i; arguments past 8 are not supported yet
  Kind ret = Kind::Int;
};

constexpr size_t kMaxThunks = 2048;  // Fellworld registers ~1,371 engine InternalCalls (M1)

// Binds the reverse thunks to the runtime that owns the guest stubs. Clears earlier thunks.
void Init(Win32::Runtime* runtime);

// A guest x86 stub for ARM64 function `fn` with `signature`, or nullopt if the runtime is out of
// stub space or every thunk slot is taken. Stable: the same fn+signature need not be deduplicated,
// each call makes a new thunk.
std::optional<uintptr_t> Make(uintptr_t fn, const Signature& signature);

} // namespace UnityBridge::IcallBridge
