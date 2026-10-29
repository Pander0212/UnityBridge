// SPDX-License-Identifier: GPL-3.0-or-later
// x86-64 structured-exception unwinding: the UNWIND_INFO interpreter (RtlVirtualUnwind),
// RtlLookupFunctionEntry over a loaded module's .pdata, and __C_specific_handler.
//
// The unwinder, the epilogue decoder and __C_specific_handler are adapted from Wine's
// dlls/ntdll/unwind.c (LGPL-2.1-or-later; see third_party/wine/PROVENANCE.md), with Wine's TRACE
// logging, dynamic/growable function tables, ARM64EC paths and KNONVOLATILE_CONTEXT_POINTERS
// removed, and the structures replaced by win32/winnt_seh.h. The register-number layout of CONTEXT
// (Rax..R15 contiguous) lets the opcodes index the registers exactly as Wine's code does.
#include "ntdll_internal.h"
#include "hle.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace UnityBridge::Win32 {
namespace {

// Documented x64 CONTEXT offsets, so a guest CONTEXT we fill or read matches MSVC's layout.
static_assert(offsetof(Context, ContextFlags) == 0x30);
static_assert(offsetof(Context, Rax) == 0x78);
static_assert(offsetof(Context, Rsp) == 0x98);
static_assert(offsetof(Context, Rip) == 0xf8);
static_assert(offsetof(Context, FltSave) == 0x100);
static_assert(offsetof(Context, FltSave) + offsetof(XsaveFormat, XmmRegisters) == 0x1a0);
static_assert(sizeof(Context) == 0x4d0);
static_assert(sizeof(RuntimeFunction) == 12);

struct UnwindModule {
  uint64_t imageBase {}, imageEnd {};
  RuntimeFunction* functions {};
  uint32_t count {};
};
std::mutex g_unwindMutex;
std::vector<UnwindModule> g_unwindModules;

union HandlerData {
  RuntimeFunction chain;
  uint32_t handler;
};

uint64_t GetIntReg(Context* context, int reg) { return (&context->Rax)[reg]; }
void SetIntReg(Context* context, int reg, uint64_t* val) { (&context->Rax)[reg] = *val; }
void SetFloatReg(Context* context, int reg, M128A* val) {
  std::memcpy(&context->FltSave.XmmRegisters[reg], val, sizeof(M128A));
}

int GetOpcodeSize(UnwindCode op) {
  switch (op.UnwindOp) {
  case UWOP_ALLOC_LARGE: return 2 + (op.OpInfo != 0);
  case UWOP_SAVE_NONVOL:
  case UWOP_SAVE_XMM128:
  case UWOP_EPILOG: return 2;
  case UWOP_SAVE_NONVOL_FAR:
  case UWOP_SAVE_XMM128_FAR: return 3;
  default: return 1;
  }
}

bool IsInsideEpilog(uint8_t* pc, uint64_t base, const RuntimeFunction* function) {
  // add or lea must be the first instruction, and it must have a rex.W prefix.
  if ((pc[0] & 0xf8) == 0x48) {
    switch (pc[1]) {
    case 0x81:  // add $nnnn,%rsp
      if (pc[0] == 0x48 && pc[2] == 0xc4) { pc += 7; break; }
      return false;
    case 0x83:  // add $n,%rsp
      if (pc[0] == 0x48 && pc[2] == 0xc4) { pc += 4; break; }
      return false;
    case 0x8d:  // lea n(reg),%rsp
      if (pc[0] & 0x06) return false;
      if (((pc[2] >> 3) & 7) != 4) return false;
      if ((pc[2] & 7) == 4) return false;
      if ((pc[2] >> 6) == 1) { pc += 4; break; }
      if ((pc[2] >> 6) == 2) { pc += 7; break; }
      return false;
    }
  }
  for (;;) {
    if ((*pc & 0xf0) == 0x40) pc++;  // rex prefix
    switch (*pc) {
    case 0x58: case 0x59: case 0x5a: case 0x5b:
    case 0x5c: case 0x5d: case 0x5e: case 0x5f:  // pop reg
      pc++;
      continue;
    case 0xc2:  // ret $nn
    case 0xc3:  // ret
      return true;
    case 0xe9:  // jmp nnnn
      pc += 5 + *(int32_t*)(pc + 1);
      if (pc - (uint8_t*)base >= function->BeginAddress && pc - (uint8_t*)base < function->EndAddress) continue;
      break;
    case 0xeb:  // jmp n
      pc += 2 + (signed char)pc[1];
      if (pc - (uint8_t*)base >= function->BeginAddress && pc - (uint8_t*)base < function->EndAddress) continue;
      break;
    case 0xf3:  // rep; ret
      return pc[1] == 0xc3;
    }
    return false;
  }
}

void InterpretEpilog(uint8_t* pc, Context* context) {
  for (;;) {
    uint8_t rex = 0;
    if ((*pc & 0xf0) == 0x40) rex = *pc++ & 0x0f;
    switch (*pc) {
    case 0x58: case 0x59: case 0x5a: case 0x5b:
    case 0x5c: case 0x5d: case 0x5e: case 0x5f:  // pop reg
      SetIntReg(context, *pc - 0x58 + (rex & 1) * 8, (uint64_t*)context->Rsp);
      context->Rsp += sizeof(uint64_t);
      pc++;
      continue;
    case 0x81:  // add $nnnn,%rsp
      context->Rsp += *(int32_t*)(pc + 2);
      pc += 2 + sizeof(int32_t);
      continue;
    case 0x83:  // add $n,%rsp
      context->Rsp += (signed char)pc[2];
      pc += 3;
      continue;
    case 0x8d:
      if ((pc[1] >> 6) == 1) {  // lea n(reg),%rsp
        context->Rsp = GetIntReg(context, (pc[1] & 7) + (rex & 1) * 8) + (signed char)pc[2];
        pc += 3;
      } else {  // lea nnnn(reg),%rsp
        context->Rsp = GetIntReg(context, (pc[1] & 7) + (rex & 1) * 8) + *(int32_t*)(pc + 2);
        pc += 2 + sizeof(int32_t);
      }
      continue;
    case 0xc2:  // ret $nn
      context->Rip = *(uint64_t*)context->Rsp;
      context->Rsp += sizeof(uint64_t) + *(uint16_t*)(pc + 1);
      return;
    case 0xc3:  // ret
    case 0xf3:  // rep; ret
      context->Rip = *(uint64_t*)context->Rsp;
      context->Rsp += sizeof(uint64_t);
      return;
    case 0xe9:  // jmp nnnn
      pc += 5 + *(int32_t*)(pc + 1);
      continue;
    case 0xeb:  // jmp n
      pc += 2 + (signed char)pc[1];
      continue;
    }
    return;
  }
}

RuntimeFunction* FindFunctionInfo(uint64_t pc, uint64_t base, RuntimeFunction* func, uint32_t count) {
  int min = 0, max = (int)count - 1;
  while (min <= max) {
    int pos = (min + max) / 2;
    if (pc < base + func[pos].BeginAddress) max = pos - 1;
    else if (pc >= base + func[pos].EndAddress) min = pos + 1;
    else {
      RuntimeFunction* f = func + pos;
      while (f->UnwindData & 1) f = (RuntimeFunction*)(base + (f->UnwindData & ~1u));
      return f;
    }
  }
  return nullptr;
}

} // namespace

void RegisterUnwindImage(uintptr_t imageBase, uintptr_t imageEnd, uintptr_t pdata, uint32_t pdataSize) {
  std::scoped_lock lock {g_unwindMutex};
  g_unwindModules.push_back(
      {imageBase, imageEnd, reinterpret_cast<RuntimeFunction*>(pdata), pdataSize / (uint32_t)sizeof(RuntimeFunction)});
}

namespace Ntdll {

RuntimeFunction* LookupFunctionEntry(uint64_t pc, uint64_t* imageBase) {
  std::scoped_lock lock {g_unwindMutex};
  for (const auto& m : g_unwindModules) {
    if (pc >= m.imageBase && pc < m.imageEnd) {
      *imageBase = m.imageBase;
      return FindFunctionInfo(pc, m.imageBase, m.functions, m.count);
    }
  }
  *imageBase = 0;
  return nullptr;
}

uint64_t PcToFileHeader(uint64_t pc) {
  std::scoped_lock lock {g_unwindMutex};
  for (const auto& m : g_unwindModules)
    if (pc >= m.imageBase && pc < m.imageEnd) return m.imageBase;
  return 0;
}

// RtlVirtualUnwind: steps `context` out of the frame whose code is at `pc`. Returns the frame's
// language handler (base + handler RVA) when UNWIND_INFO carries one and `type` matches, else null.
ExceptionRoutine* VirtualUnwind(uint32_t type, uint64_t base, uint64_t pc, RuntimeFunction* function,
                                Context* context, void** handlerData, uint64_t* frameRet) {
  HandlerData* handler_data;
  uint64_t frame, off;
  UnwindInfo* info;
  unsigned i, prolog_offset;
  bool mach_frame = false;

  *handlerData = nullptr;
  frame = *frameRet = context->Rsp;

  if (!function) {  // leaf function
    context->Rip = *(uint64_t*)context->Rsp;
    context->Rsp += sizeof(uint64_t);
    return nullptr;
  }

  for (;;) {
    info = (UnwindInfo*)(base + function->UnwindData);
    handler_data = (HandlerData*)&info->UnwindCode_[(info->CountOfCodes + 1) & ~1];

    if (info->Version != 1 && info->Version != 2) return nullptr;  // bad unwind info

    if (info->FrameRegister) frame = GetIntReg(context, info->FrameRegister) - info->FrameOffset * 16;

    if (pc >= base + function->BeginAddress && pc < base + function->BeginAddress + info->SizeOfProlog) {
      prolog_offset = (unsigned)(pc - base - function->BeginAddress);
    } else {
      prolog_offset = ~0u;
      if (info->CountOfCodes && IsInsideEpilog((uint8_t*)pc, base, function)) {
        InterpretEpilog((uint8_t*)pc, context);
        *frameRet = frame;
        return nullptr;
      }
    }

    for (i = 0; i < info->CountOfCodes; i += GetOpcodeSize(info->UnwindCode_[i])) {
      if (prolog_offset < info->UnwindCode_[i].CodeOffset) continue;  // skip it
      switch (info->UnwindCode_[i].UnwindOp) {
      case UWOP_PUSH_NONVOL:
        SetIntReg(context, info->UnwindCode_[i].OpInfo, (uint64_t*)context->Rsp);
        context->Rsp += sizeof(uint64_t);
        break;
      case UWOP_ALLOC_LARGE:
        if (info->UnwindCode_[i].OpInfo) context->Rsp += *(uint32_t*)&info->UnwindCode_[i + 1];
        else context->Rsp += *(uint16_t*)&info->UnwindCode_[i + 1] * 8;
        break;
      case UWOP_ALLOC_SMALL:
        context->Rsp += (info->UnwindCode_[i].OpInfo + 1) * 8;
        break;
      case UWOP_SET_FPREG:
        context->Rsp = *frameRet = frame;
        break;
      case UWOP_SAVE_NONVOL:
        off = frame + *(uint16_t*)&info->UnwindCode_[i + 1] * 8;
        SetIntReg(context, info->UnwindCode_[i].OpInfo, (uint64_t*)off);
        break;
      case UWOP_SAVE_NONVOL_FAR:
        off = frame + *(uint32_t*)&info->UnwindCode_[i + 1];
        SetIntReg(context, info->UnwindCode_[i].OpInfo, (uint64_t*)off);
        break;
      case UWOP_SAVE_XMM128:
        off = frame + *(uint16_t*)&info->UnwindCode_[i + 1] * 16;
        SetFloatReg(context, info->UnwindCode_[i].OpInfo, (M128A*)off);
        break;
      case UWOP_SAVE_XMM128_FAR:
        off = frame + *(uint32_t*)&info->UnwindCode_[i + 1];
        SetFloatReg(context, info->UnwindCode_[i].OpInfo, (M128A*)off);
        break;
      case UWOP_PUSH_MACHFRAME:
        if (info->Flags & UNW_FLAG_CHAININFO) break;
        if (i + GetOpcodeSize(info->UnwindCode_[i]) < info->CountOfCodes) break;
        if (info->UnwindCode_[i].OpInfo) context->Rsp += 0x8;
        context->Rip = *(uint64_t*)context->Rsp;
        context->Rsp = *(uint64_t*)(context->Rsp + 24);
        mach_frame = true;
        break;
      case UWOP_EPILOG:
        break;  // version 2: nothing to do
      default:
        break;
      }
    }

    if (!(info->Flags & UNW_FLAG_CHAININFO)) break;
    function = &handler_data->chain;
  }

  if (!mach_frame) {
    context->Rip = *(uint64_t*)context->Rsp;
    context->Rsp += sizeof(uint64_t);
  }

  if (!(info->Flags & type)) return nullptr;         // no matching handler
  if (prolog_offset != ~0u) return nullptr;          // inside prolog
  *handlerData = &handler_data->handler + 1;
  return (ExceptionRoutine*)(base + handler_data->handler);
}

// __C_specific_handler: the language handler MSVC references for __try/__except/__finally. Adapted
// from Wine's ntdll (x86-64 path).
ExceptionDisposition CSpecificHandler(ExceptionRecord* rec, void* frame, Context* context,
                                      DispatcherContext* dispatch) {
  const ScopeTable* table = (const ScopeTable*)dispatch->HandlerData;
  uint64_t base = dispatch->ImageBase;
  uint64_t pc = dispatch->ControlPc;

  if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
    for (uint32_t i = dispatch->ScopeIndex; i < table->Count; i++) {
      if (pc < base + table->ScopeRecord[i].BeginAddress) continue;
      if (pc >= base + table->ScopeRecord[i].EndAddress) continue;
      if (table->ScopeRecord[i].JumpTarget) continue;
      if (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND &&
          dispatch->TargetIp >= base + table->ScopeRecord[i].BeginAddress &&
          dispatch->TargetIp < base + table->ScopeRecord[i].EndAddress) {
        break;
      }
      // A __finally termination handler: handler(TRUE, frame), run as guest code.
      uint64_t handler = base + table->ScopeRecord[i].HandlerAddress;
      dispatch->ScopeIndex = i + 1;
      CallGuest(handler, {1 /* abnormal termination */, (uint64_t)frame});
    }
  } else {
    for (uint32_t i = dispatch->ScopeIndex; i < table->Count; i++) {
      if (pc < base + table->ScopeRecord[i].BeginAddress) continue;
      if (pc >= base + table->ScopeRecord[i].EndAddress) continue;
      if (!table->ScopeRecord[i].JumpTarget) continue;
      if (table->ScopeRecord[i].HandlerAddress != (uint32_t)EXCEPTION_EXECUTE_HANDLER) {
        ExceptionPointers ptrs = {rec, context};
        uint64_t filter = base + table->ScopeRecord[i].HandlerAddress;
        // __except filter: int filter(EXCEPTION_POINTERS*, frame), run as guest code.
        const int32_t result = (int32_t)(uint32_t)CallGuest(filter, {(uint64_t)&ptrs, (uint64_t)frame});
        if (result == EXCEPTION_CONTINUE_SEARCH) continue;
        if (result == EXCEPTION_CONTINUE_EXECUTION) return ExceptionContinueExecution;
      }
      UnwindEx(frame, (char*)base + table->ScopeRecord[i].JumpTarget, rec, nullptr,
               dispatch->HistoryTable);
      // UnwindEx requested the resume; nothing after it runs.
      return ExceptionContinueSearch;
    }
  }
  return ExceptionContinueSearch;
}

} // namespace Ntdll
} // namespace UnityBridge::Win32
