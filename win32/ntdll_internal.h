// SPDX-License-Identifier: GPL-3.0-or-later
// Internal interface shared by win32/ntdll_unwind.cpp (the UNWIND_INFO interpreter and
// __C_specific_handler, adapted from Wine) and win32/ntdll_exception.cpp (the dispatcher, raise and
// resume glue). Not part of the public mini-Win32 surface.
#pragma once

#include "winnt_seh.h"

#include <cstdint>

namespace UnityBridge::Win32 {

struct CallContext;

namespace Ntdll {

// The language handler the unwinder hands back (DISPATCHER_CONTEXT.LanguageHandler). It may be our
// own __C_specific_handler or a guest routine; call_language_handler routes it either way.
using ExceptionRoutine = ExceptionDisposition (*)(ExceptionRecord*, void* frame, Context*, DispatcherContext*);

// RtlLookupFunctionEntry: the RUNTIME_FUNCTION covering `pc`, with its module image base.
RuntimeFunction* LookupFunctionEntry(uint64_t pc, uint64_t* imageBase);
// RtlPcToFileHeader: the image base of the module that contains `pc`, or 0.
uint64_t PcToFileHeader(uint64_t pc);

// RtlVirtualUnwind over x64 UNWIND_INFO: steps `ctx` out of the frame at `pc`, returns the frame's
// language handler (or null) and sets *handlerData and *frame. Adapted from Wine's ntdll.
ExceptionRoutine* VirtualUnwind(uint32_t type, uint64_t base, uint64_t pc, RuntimeFunction* function,
                                Context* ctx, void** handlerData, uint64_t* frame);

// __C_specific_handler: runs __except filters / __finally termination handlers for a frame. Adapted
// from Wine's ntdll. Host-side so the dispatcher can call it directly without a guest round trip.
ExceptionDisposition CSpecificHandler(ExceptionRecord* rec, void* frame, Context* ctx, DispatcherContext* dispatch);

// RtlUnwindEx: unwinds from the exception point (the context last given to SetExceptionContext, or
// the one DispatchException is handling) to `endFrame`, running termination handlers, then requests
// a resume to `targetIp` with `retval` in Rax. Records the pending resume and returns.
void UnwindEx(void* endFrame, void* targetIp, ExceptionRecord* rec, void* retval, UnwindHistoryTable* table);

// Sets the context the next UnwindEx starts from (a direct RtlUnwindEx call captures its own).
void SetExceptionContext(const Context& ctx);

// Dispatches an exception through the SEH handler chain (RtlDispatchException). Returns true when a
// handler resolved it (a resume is pending), false when unhandled.
bool DispatchException(ExceptionRecord* rec, Context* ctx);

// Calls a (possibly guest) language handler, routing our own __C_specific_handler to the host
// implementation and anything else through CallGuest.
ExceptionDisposition CallLanguageHandler(void* handler, ExceptionRecord* rec, uint64_t frame,
                                         Context* ctx, DispatcherContext* dispatch);

// Fills a CONTEXT from the registers captured at an HLE call (RtlCaptureContext).
void CaptureContextFromCall(Context* out, const CallContext& call);

// Requests a non-local resume of the current guest thread with `ctx` (NtContinue / RtlRestoreContext
// minus the consolidate/longjmp fix-ups). Every SEH function returns early once this is set; the
// RaiseException HLE handler applies it to the guest when it unwinds back out.
void RequestResume(const Context* ctx);
bool ResumePending();
void ClearResume();
// Copies the pending resume into the CallContext resume fields the bridge applies. Clears pending.
void TakeResumeInto(CallContext& call);

// Is `guestAddr` a thunk (or direct pointer) to our own __C_specific_handler stub?
bool IsOurCSpecificHandler(uint64_t guestAddr);

// Validity check against the current thread's TEB stack bounds (is_valid_frame).
bool IsValidFrame(uint64_t frame);

} // namespace Ntdll
} // namespace UnityBridge::Win32
