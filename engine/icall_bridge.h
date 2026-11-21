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
// or ArgDouble) and appends it to the next integer or floating register. Past eight of a kind the
// arguments go to the stack, as AAPCS64 lays them out (libunity is an Android library: the standard
// procedure call standard, not Apple's variant): in argument order, one 8-byte slot each, integer
// and floating arguments alike, below the callee's stack pointer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace UnityBridge::Win32 { class Runtime; }

namespace UnityBridge::IcallBridge {

// Argument and return kinds that differ between the conventions. Pointers and 32/64-bit integers
// are Int; float and double are Float (a 32-bit float still occupies a full register slot either
// way and its bits move unchanged, so the thunk treats r4 and r8 the same). Void returns use Int and
// the result is ignored. The 8- and 16-bit kinds (bool is UInt8, char is UInt16) are Int that the
// thunk sign- or zero-extends in both directions: a Microsoft x64 caller leaves the upper bits of a
// narrow argument undefined, while Apple's arm64 ABI lets the callee assume them extended to 32 bits.
enum class Kind : uint8_t { Int, Float, Int8, UInt8, Int16, UInt16 };

constexpr size_t kMaxArgs = 32;        // TextGenerator::Populate_Internal_Injected has 23 (M3f)
constexpr size_t kMaxStackSlots = 24;  // the arguments past 8 of a kind

struct Signature {
  uint8_t argCount = 0;     // including the instance pointer for an instance method
  Kind args[kMaxArgs] = {};  // args[i] is argument i
  Kind ret = Kind::Int;
};

// True if `signature` fits AAPCS64's argument registers: at most 8 Float and 8 non-Float arguments.
bool FitsRegisters(const Signature& signature);
// The AAPCS64 stack slots `signature` needs: the integer arguments past the eighth and the floating
// ones past the eighth.
unsigned StackSlots(const Signature& signature);
// True if the thunks can call a function with `signature`: at most kMaxArgs arguments, at most
// kMaxStackSlots of them on the stack.
bool Supported(const Signature& signature);

// Where each argument goes for AAPCS64. `raw[i]` is argument i as the Microsoft x64 caller passed it
// (its register or stack slot; a floating one's bits); narrow integers are extended here, in a
// register or a stack slot alike.
struct Layout {
  uint64_t gpr[8] = {};
  double fpr[8] = {};
  uint64_t stack[kMaxStackSlots] = {};
  unsigned stackSlots = 0;
};
Layout LayOut(const Signature& signature, const uint64_t* raw);

constexpr size_t kMaxThunks = 2048;  // Fellworld registers ~1,371 engine InternalCalls (M1)

// Binds the reverse thunks to the runtime that owns the guest stubs. Clears earlier thunks.
void Init(Win32::Runtime* runtime);

// A guest x86 stub for ARM64 function `fn` with `signature`, or nullopt if the signature is not
// Supported, the runtime is out of stub space or every thunk slot is taken. Stable: the same fn+signature need not be deduplicated,
// each call makes a new thunk.
std::optional<uintptr_t> Make(uintptr_t fn, const Signature& signature);

} // namespace UnityBridge::IcallBridge
