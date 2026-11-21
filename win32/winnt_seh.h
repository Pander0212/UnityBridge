// SPDX-License-Identifier: GPL-3.0-or-later
// The x86-64 Windows structures the structured-exception-handling code works with, laid out exactly
// as the guest's MSVC-compiled code and its .pdata/UNWIND_INFO expect them. These are ABI facts
// (offsets fixed by Windows on x64), checked against their documented offsets in ntdll_unwind.cpp.
#pragma once

#include <cstdint>

namespace UnityBridge::Win32 {

using DWORD64 = uint64_t;
using ULONG = uint32_t;
using ULONG64 = uint64_t;
using ULONG_PTR = uint64_t;

struct M128A {
  uint64_t Low;
  int64_t High;
};

// XSAVE_FORMAT (FXSAVE area), 512 bytes; XmmRegisters sit at offset 0xa0 within it.
struct XsaveFormat {
  uint16_t ControlWord, StatusWord;
  uint8_t TagWord, Reserved1;
  uint16_t ErrorOpcode;
  uint32_t ErrorOffset;
  uint16_t ErrorSelector, Reserved2;
  uint32_t DataOffset;
  uint16_t DataSelector, Reserved3;
  uint32_t MxCsr, MxCsr_Mask;
  M128A FloatRegisters[8];
  M128A XmmRegisters[16];
  uint8_t Reserved4[96];
};

// CONTEXT (x64), 1232 (0x4d0) bytes. Rax..R15 are contiguous in x64 register-number order (RAX 0,
// RCX 1, RDX 2, RBX 3, RSP 4, RBP 5, RSI 6, RDI 7, R8..R15 8..15), so `(&ctx.Rax)[n]` is register n,
// and the UNWIND_INFO opcode register numbers index them directly.
struct alignas(16) Context {
  uint64_t P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
  uint32_t ContextFlags;
  uint32_t MxCsr;
  uint16_t SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
  uint32_t EFlags;
  uint64_t Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
  uint64_t Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
  uint64_t R8, R9, R10, R11, R12, R13, R14, R15;
  uint64_t Rip;
  XsaveFormat FltSave;
  M128A VectorRegister[26];
  uint64_t VectorControl;
  uint64_t DebugControl, LastBranchToRip, LastBranchFromRip, LastExceptionToRip, LastExceptionFromRip;
};

// CONTEXT.ContextFlags
inline constexpr uint32_t kContextAmd64 = 0x00100000u;
inline constexpr uint32_t kContextControl = kContextAmd64 | 0x1;
inline constexpr uint32_t kContextInteger = kContextAmd64 | 0x2;
inline constexpr uint32_t kContextFull = kContextControl | kContextInteger | (kContextAmd64 | 0x4);
inline constexpr uint32_t kContextAll = kContextFull | (kContextAmd64 | 0x8) | (kContextAmd64 | 0x10) | (kContextAmd64 | 0x20);

inline constexpr unsigned kExceptionMaximumParameters = 15;

struct ExceptionRecord {
  uint32_t ExceptionCode;
  uint32_t ExceptionFlags;
  ExceptionRecord* ExceptionRecord_;  // nested record
  void* ExceptionAddress;
  uint32_t NumberParameters;
  uint32_t Alignment;
  uint64_t ExceptionInformation[kExceptionMaximumParameters];
};

struct ExceptionPointers {
  ExceptionRecord* ExceptionRecord_;
  Context* ContextRecord;
};

// .pdata entry.
struct RuntimeFunction {
  uint32_t BeginAddress;
  uint32_t EndAddress;
  uint32_t UnwindData;  // RVA of UNWIND_INFO, or (low bit set) a chained RUNTIME_FUNCTION
};

struct UnwindCode {
  uint8_t CodeOffset;
  uint8_t UnwindOp : 4;
  uint8_t OpInfo : 4;
};

struct UnwindInfo {
  uint8_t Version : 3;
  uint8_t Flags : 5;
  uint8_t SizeOfProlog;
  uint8_t CountOfCodes;
  uint8_t FrameRegister : 4;
  uint8_t FrameOffset : 4;
  UnwindCode UnwindCode_[1];  // CountOfCodes entries, then handler / chained function
};

// UNWIND_INFO.Flags
inline constexpr uint8_t UNW_FLAG_EHANDLER = 0x1;
inline constexpr uint8_t UNW_FLAG_UHANDLER = 0x2;
inline constexpr uint8_t UNW_FLAG_CHAININFO = 0x4;

// Unwind opcodes
enum : uint8_t {
  UWOP_PUSH_NONVOL = 0, UWOP_ALLOC_LARGE = 1, UWOP_ALLOC_SMALL = 2, UWOP_SET_FPREG = 3,
  UWOP_SAVE_NONVOL = 4, UWOP_SAVE_NONVOL_FAR = 5, UWOP_EPILOG = 6, UWOP_SAVE_XMM128 = 8,
  UWOP_SAVE_XMM128_FAR = 9, UWOP_PUSH_MACHFRAME = 10,
};

struct ScopeTable {
  uint32_t Count;
  struct {
    uint32_t BeginAddress;
    uint32_t EndAddress;
    uint32_t HandlerAddress;
    uint32_t JumpTarget;
  } ScopeRecord[1];  // Count entries
};

struct UnwindHistoryTable {
  uint32_t Count;
  uint8_t LocalHint;
  uint8_t GlobalHint;
  uint8_t Search;
  uint8_t Once;
  uint64_t LowAddress;
  uint64_t HighAddress;
  struct { uint64_t ImageBase; uint64_t Gp; RuntimeFunction* FunctionEntry; } Entry[12];
};

struct DispatcherContext {
  uint64_t ControlPc;
  uint64_t ImageBase;
  RuntimeFunction* FunctionEntry;
  uint64_t EstablisherFrame;
  uint64_t TargetIp;
  Context* ContextRecord;
  void* LanguageHandler;  // PEXCEPTION_ROUTINE (guest or our __C_specific_handler)
  void* HandlerData;
  UnwindHistoryTable* HistoryTable;
  uint32_t ScopeIndex;
  uint32_t Fill0;
};

// EXCEPTION_DISPOSITION
enum ExceptionDisposition : int {
  ExceptionContinueExecution = 0,
  ExceptionContinueSearch = 1,
  ExceptionNestedException = 2,
  ExceptionCollidedUnwind = 3,
};

// EXCEPTION_RECORD.ExceptionFlags
inline constexpr uint32_t EXCEPTION_NONCONTINUABLE = 0x01;
inline constexpr uint32_t EXCEPTION_UNWINDING = 0x02;
inline constexpr uint32_t EXCEPTION_EXIT_UNWIND = 0x04;
inline constexpr uint32_t EXCEPTION_STACK_INVALID = 0x08;
inline constexpr uint32_t EXCEPTION_NESTED_CALL = 0x10;
inline constexpr uint32_t EXCEPTION_TARGET_UNWIND = 0x20;
inline constexpr uint32_t EXCEPTION_COLLIDED_UNWIND = 0x40;
inline constexpr uint32_t EXCEPTION_UNWIND =
    EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND | EXCEPTION_TARGET_UNWIND | EXCEPTION_COLLIDED_UNWIND;

// UNWIND type (RtlVirtualUnwind)
inline constexpr uint32_t UNW_FLAG_NHANDLER = 0x0;

// Status / exception codes
inline constexpr uint32_t STATUS_SUCCESS = 0x00000000u;
inline constexpr uint32_t STATUS_UNWIND = 0xc0000027u;
inline constexpr uint32_t STATUS_INVALID_DISPOSITION = 0xc0000026u;
inline constexpr uint32_t STATUS_NONCONTINUABLE_EXCEPTION = 0xc0000025u;
inline constexpr uint32_t STATUS_INVALID_UNWIND_TARGET = 0xc0000029u;
inline constexpr uint32_t STATUS_BAD_FUNCTION_TABLE = 0xc00000ffu;
inline constexpr uint32_t STATUS_UNHANDLED_EXCEPTION = 0xc0000144u;
inline constexpr uint32_t STATUS_LONGJUMP = 0x80000026u;
inline constexpr uint32_t STATUS_UNWIND_CONSOLIDATE = 0x80000029u;

// __except filter return values
inline constexpr int32_t EXCEPTION_EXECUTE_HANDLER = 1;
inline constexpr int32_t EXCEPTION_CONTINUE_SEARCH = 0;
inline constexpr int32_t EXCEPTION_CONTINUE_EXECUTION = -1;

} // namespace UnityBridge::Win32
