// SPDX-License-Identifier: GPL-3.0-or-later
// The engine side of x64 structured exception handling: capturing a guest CONTEXT at an HLE call,
// raising and dispatching exceptions through the frame handlers, unwinding to a target, and the
// non-local resume of the guest thread.
//
// The dispatcher (DispatchException), RtlUnwindEx and RtlRestoreContext are adapted from Wine's
// dlls/ntdll/signal_x86_64.c and exception.c (LGPL-2.1-or-later; see third_party/wine/PROVENANCE.md),
// with the host-vs-guest split that UnityBridge needs: the unwinder runs on the host over the
// guest's .pdata, guest funclets (__except filters, __finally handlers, consolidate callbacks) run
// through CallGuest and return normally, and the one non-local transfer (NtContinue) is recorded as
// a pending resume that the outermost HLE handler applies to the guest when it returns. There is no
// setjmp/longjmp across a guest call, which the device's nested FEXCore callbacks could not survive.
#include "ntdll_internal.h"
#include "hle.h"
#include "common.h"

#include <cstdio>
#include <cstring>

namespace UnityBridge::Win32 {
namespace Ntdll {
namespace {

// The context at the point an exception was raised (or an explicit RtlUnwindEx call site); the
// unwind starts from here so it visits every frame between the raise and the target.
thread_local Context t_exceptionContext;
// Traces the dispatch of the first few C++ exceptions (frames, handlers, dispositions, unwinds) to
// stderr: GameAssembly's own handlers (statically linked __CxxFrameHandler / __C_specific_handler)
// have never caught one on the device, and only the device runs them.
thread_local bool t_trace = false;
int g_traced = 0;
// The pending non-local resume (NtContinue). Every SEH function returns early once it is set.
thread_local Context t_resumeContext;
thread_local bool t_resumePending = false;

// Nesting bookkeeping for the device. A guest language handler (GameAssembly's statically linked
// __C_specific_handler or __CxxFrameHandler*) runs inside a FEXCore callback and itself calls our
// RtlUnwindEx; the resume must then NOT be applied at that nested HLE call (which would leave the
// callback's host frames stranded) but at the HLE call that the faulting code made directly. The
// device bridge reports the current HLE-call depth through SetInvokeDepth; a resume carries the
// depth of the dispatch that started it, and the bridge escapes (lets the callback return) at any
// deeper level and applies at that root. Host tests do not nest HLE calls (guest handlers run as
// native calls on the one stack), so the resume is applied at the innermost call and this is unused.
thread_local int t_invokeDepth = 0;
thread_local int t_dispatchRootDepth = 0;
thread_local bool t_dispatchActive = false;
thread_local int t_resumeRootDepth = 0;
thread_local bool t_resumeByBridge = false;

std::optional<uint64_t> DecodeJumpTarget(uint64_t addr) {
  const auto* p = reinterpret_cast<const uint8_t*>(addr);
  int32_t disp;
  if (p[0] == 0xff && p[1] == 0x25) {  // jmp qword ptr [rip+disp32]
    std::memcpy(&disp, p + 2, 4);
    return *reinterpret_cast<const uint64_t*>(addr + 6 + disp);
  }
  if (p[0] == 0x48 && p[1] == 0xff && p[2] == 0x25) {
    std::memcpy(&disp, p + 3, 4);
    return *reinterpret_cast<const uint64_t*>(addr + 7 + disp);
  }
  return std::nullopt;
}

} // namespace

void SetExceptionContext(const Context& ctx) { t_exceptionContext = ctx; }

void RequestResume(const Context* ctx) {
  t_resumeContext = *ctx;
  t_resumeRootDepth = t_dispatchActive ? t_dispatchRootDepth : t_invokeDepth;
  t_resumePending = true;
}
bool ResumePending() { return t_resumePending; }
void ClearResume() { t_resumePending = false; }

void TakeResumeInto(CallContext& call) {
  if (!t_resumePending) return;
  for (int i = 0; i < 16; ++i) call.resumeGpr[i] = (&t_resumeContext.Rax)[i];
  for (int i = 0; i < 16; ++i) std::memcpy(call.resumeXmm[i], &t_resumeContext.FltSave.XmmRegisters[i], 16);
  call.resumeRip = t_resumeContext.Rip;
  call.resume = true;
  t_resumePending = false;
  t_dispatchActive = false;
}

// Marks the extent of a dispatch, so a resume triggered from a nested RtlUnwindEx knows which HLE
// call to resume at. Restores the previous root on exit (nested dispatches, e.g. from a __finally).
struct DispatchScope {
  int savedRoot;
  bool savedActive;
  DispatchScope() : savedRoot {t_dispatchRootDepth}, savedActive {t_dispatchActive} {
    t_dispatchRootDepth = t_invokeDepth;
    t_dispatchActive = true;
  }
  ~DispatchScope() {
    t_dispatchRootDepth = savedRoot;
    t_dispatchActive = savedActive;
  }
};

void CaptureContextFromCall(Context* out, const CallContext& call) {
  std::memset(out, 0, sizeof(*out));
  out->ContextFlags = kContextFull;
  for (int i = 0; i < 16; ++i) (&out->Rax)[i] = call.x86[i];
  out->Rip = call.rip;
  out->Rsp = call.rsp + 8;  // the caller's rsp after this call returns
  for (int i = 0; i < 16; ++i) std::memcpy(&out->FltSave.XmmRegisters[i], call.xmmFull[i], 16);
}

uint64_t ModuleOffset(uint64_t pc) {
  const uint64_t base = PcToFileHeader(pc);
  return base ? pc - base : pc;
}

bool IsValidFrame(uint64_t frame) { return frame != 0 && (frame & (sizeof(void*) - 1)) == 0; }

bool IsOurCSpecificHandler(uint64_t guestAddr) {
  auto* runtime = Runtime::Current();
  if (runtime == nullptr) return false;
  const uint64_t target = DecodeJumpTarget(guestAddr).value_or(guestAddr);
  const Function* f = runtime->FunctionForStub(target);
  return f != nullptr && f->name == "__C_specific_handler";
}

ExceptionDisposition CallLanguageHandler(void* handler, ExceptionRecord* rec, uint64_t frame,
                                         Context* ctx, DispatcherContext* dispatch) {
  if (IsOurCSpecificHandler(reinterpret_cast<uint64_t>(handler)))
    return CSpecificHandler(rec, reinterpret_cast<void*>(frame), ctx, dispatch);
  // A guest language handler (e.g. a statically linked __C_specific_handler or __CxxFrameHandler):
  //   EXCEPTION_DISPOSITION handler(rec, EstablisherFrame, context, dispatch)
  return static_cast<ExceptionDisposition>(static_cast<uint32_t>(
      CallGuest(reinterpret_cast<uintptr_t>(handler),
                {reinterpret_cast<uint64_t>(rec), frame, reinterpret_cast<uint64_t>(ctx),
                 reinterpret_cast<uint64_t>(dispatch)})));
}

// RtlRestoreContext (minus the TEB-frame hack): handles the consolidate and longjmp pseudo-records,
// then records the resume. Adapted from Wine's signal_x86_64.c.
void RestoreContext(Context* context, ExceptionRecord* rec) {
  if (rec && rec->ExceptionCode == STATUS_UNWIND_CONSOLIDATE && rec->NumberParameters >= 1) {
    // The consolidate callback (a guest function) returns the address to resume at. MSVC's C++
    // exception handling uses this to run the catch funclet on a clean frame.
    const uint64_t newRip = CallGuest(static_cast<uintptr_t>(rec->ExceptionInformation[0]),
                                      {reinterpret_cast<uint64_t>(rec)});
    context->Rip = newRip;
  }
  RequestResume(context);
}

// RtlUnwindEx: unwinds from the exception point to `endFrame`, running termination handlers, then
// records a resume to `targetIp` with `retval` in Rax. Adapted from Wine's RtlUnwindEx.
void UnwindEx(void* endFrame, void* targetIp, ExceptionRecord* rec, void* retval,
              UnwindHistoryTable* /*table*/) {  // starts from t_exceptionContext
  Context context = t_exceptionContext;
  Context new_context = context;
  if (t_trace)
    std::fprintf(stderr, "UB_SEH  unwind to frame %#llx, target +%#llx%s\n",
                 static_cast<unsigned long long>(reinterpret_cast<uint64_t>(endFrame)),
                 static_cast<unsigned long long>(ModuleOffset(reinterpret_cast<uint64_t>(targetIp))),
                 rec && rec->ExceptionCode == STATUS_UNWIND_CONSOLIDATE ? " (consolidate)" : "");
  ExceptionRecord record;
  if (!rec) {
    std::memset(&record, 0, sizeof(record));
    record.ExceptionCode = STATUS_UNWIND;
    record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
    rec = &record;
  }
  rec->ExceptionFlags |= EXCEPTION_UNWINDING | (endFrame ? 0 : EXCEPTION_EXIT_UNWIND);

  DispatcherContext dispatch;
  std::memset(&dispatch, 0, sizeof(dispatch));
  dispatch.TargetIp = reinterpret_cast<uint64_t>(targetIp);
  dispatch.ContextRecord = &context;

  for (int guard = 0; guard < 65536; ++guard) {
    dispatch.ControlPc = new_context.Rip;
    uint64_t base = 0;
    dispatch.FunctionEntry = LookupFunctionEntry(new_context.Rip, &base);
    dispatch.ImageBase = base;
    dispatch.ScopeIndex = 0;
    uint64_t frame = 0;
    void* handlerData = nullptr;
    ExceptionRoutine* handler =
        VirtualUnwind(UNW_FLAG_UHANDLER, base, new_context.Rip, dispatch.FunctionEntry, &new_context,
                      &handlerData, &frame);
    dispatch.EstablisherFrame = frame;
    dispatch.HandlerData = handlerData;
    dispatch.LanguageHandler = reinterpret_cast<void*>(handler);

    if (handler) {
      if (!IsValidFrame(frame)) { rec->ExceptionFlags |= EXCEPTION_STACK_INVALID; break; }
      if (endFrame && frame > reinterpret_cast<uint64_t>(endFrame)) break;  // invalid end frame
      if (frame == reinterpret_cast<uint64_t>(endFrame)) rec->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;
      const ExceptionDisposition res = CallLanguageHandler(reinterpret_cast<void*>(handler), rec, frame,
                                                           &context, &dispatch);
      if (res == ExceptionCollidedUnwind) {
        uint64_t f2;
        void* hd2;
        new_context = context;
        VirtualUnwind(UNW_FLAG_NHANDLER, dispatch.ImageBase, dispatch.ControlPc, dispatch.FunctionEntry,
                      &new_context, &hd2, &f2);
        rec->ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
        continue;
      }
      rec->ExceptionFlags &= ~EXCEPTION_COLLIDED_UNWIND;
    } else if (!dispatch.FunctionEntry && PcToFileHeader(new_context.Rip) == 0) {
      break;  // left all known guest code
    }

    if (frame == reinterpret_cast<uint64_t>(endFrame)) break;
    if (!frame) break;
    context = new_context;
  }

  context.Rax = reinterpret_cast<uint64_t>(retval);
  if (rec->ExceptionCode != STATUS_UNWIND_CONSOLIDATE) context.Rip = reinterpret_cast<uint64_t>(targetIp);
  RestoreContext(&context, rec);
}

// RtlDispatchException: the EHANDLER search over the frame chain. Adapted from Wine's
// call_seh_handlers. Returns true when a handler resolved the exception (a resume is pending).
bool DispatchException(ExceptionRecord* rec, Context* orig_context) {
  DispatchScope dispatchScope;  // marks the HLE-call depth to resume at (see t_resumeRootDepth)
  const bool outerTrace = t_trace;
  t_trace = rec->ExceptionCode == 0xe06d7363 && g_traced < 4;
  if (t_trace) {
    ++g_traced;
    std::fprintf(stderr, "UB_SEH dispatch %#x at +%#llx, %u parameters, magic %#llx\n", rec->ExceptionCode,
                 static_cast<unsigned long long>(ModuleOffset(orig_context->Rip)), rec->NumberParameters,
                 static_cast<unsigned long long>(rec->NumberParameters ? rec->ExceptionInformation[0] : 0));
  }
  struct Restore {
    bool v;
    ~Restore() { t_trace = v; }
  } restoreTrace {outerTrace};
  t_exceptionContext = *orig_context;
  Context context = *orig_context;
  DispatcherContext dispatch;
  std::memset(&dispatch, 0, sizeof(dispatch));
  dispatch.TargetIp = 0;
  dispatch.ContextRecord = &context;

  for (int guard = 0; guard < 65536; ++guard) {
    dispatch.ControlPc = context.Rip;
    uint64_t base = 0;
    dispatch.FunctionEntry = LookupFunctionEntry(context.Rip, &base);
    dispatch.ImageBase = base;
    dispatch.ScopeIndex = 0;
    uint64_t frame = 0;
    void* handlerData = nullptr;
    ExceptionRoutine* handler = VirtualUnwind(UNW_FLAG_EHANDLER, base, context.Rip,
                                              dispatch.FunctionEntry, &context, &handlerData, &frame);
    dispatch.EstablisherFrame = frame;
    dispatch.HandlerData = handlerData;
    dispatch.LanguageHandler = reinterpret_cast<void*>(handler);

    if (handler) {
      if (!IsValidFrame(frame)) { rec->ExceptionFlags |= EXCEPTION_STACK_INVALID; break; }
      const ExceptionDisposition res =
          CallLanguageHandler(reinterpret_cast<void*>(handler), rec, frame, orig_context, &dispatch);
      if (t_trace)
        std::fprintf(stderr, "UB_SEH  pc +%#llx frame %#llx handler +%#llx -> %d%s\n",
                     static_cast<unsigned long long>(dispatch.ControlPc - dispatch.ImageBase),
                     static_cast<unsigned long long>(frame),
                     static_cast<unsigned long long>(ModuleOffset(reinterpret_cast<uint64_t>(handler))),
                     static_cast<int>(res), t_resumePending ? " (resume pending)" : "");
      if (t_resumePending) return true;  // a handler executed (RtlUnwindEx ran)
      rec->ExceptionFlags &= EXCEPTION_NONCONTINUABLE;
      switch (res) {
      case ExceptionContinueExecution:
        if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) return false;
        RequestResume(orig_context);
        return true;
      case ExceptionContinueSearch:
        break;
      case ExceptionNestedException:
        rec->ExceptionFlags |= EXCEPTION_NESTED_CALL;
        break;
      case ExceptionCollidedUnwind: {
        uint64_t f2;
        void* hd2;
        VirtualUnwind(UNW_FLAG_NHANDLER, dispatch.ImageBase, dispatch.ControlPc, dispatch.FunctionEntry,
                      &context, &hd2, &f2);
        continue;
      }
      default:
        return false;
      }
    } else if (!dispatch.FunctionEntry && PcToFileHeader(context.Rip) == 0) {
      break;  // unwound out of all known guest code: unhandled
    }
    if (!frame) break;
  }
  return false;
}

// An exception nobody handled: the frames the search walked (from the raise point, as module offsets,
// with or without unwind info and handler) and where it stopped; for an MSVC C++ exception thrown by
// il2cpp (an Il2CppExceptionWrapper), the managed exception's class and message. For the log only.
void DescribeUnhandled(const ExceptionRecord* rec, const Context* raised) {
  Context context = *raised;
  for (int depth = 0; depth < 48; ++depth) {
    uint64_t base = 0;
    RuntimeFunction* fn = LookupFunctionEntry(context.Rip, &base);
    const uint64_t module = PcToFileHeader(context.Rip);
    uint64_t frame = 0;
    void* handlerData = nullptr;
    const uint64_t rip = context.Rip;
    ExceptionRoutine* handler = VirtualUnwind(UNW_FLAG_EHANDLER, base, rip, fn, &context, &handlerData, &frame);
    std::fprintf(stderr, "UB_WIN32   #%d rip=%#llx %s+%#llx%s handler=%#llx frame=%#llx\n", depth,
                 static_cast<unsigned long long>(rip), module ? "module" : "(no module)",
                 static_cast<unsigned long long>(module ? rip - module : 0), fn ? "" : " (no unwind info)",
                 static_cast<unsigned long long>(handler ? reinterpret_cast<uint64_t>(handler) - module : 0),
                 static_cast<unsigned long long>(frame));
    if (!fn && module == 0) { std::fprintf(stderr, "UB_WIN32   (left the guest's code: the search ends here)\n"); break; }
    if (!frame || !context.Rip) { std::fprintf(stderr, "UB_WIN32   (no caller frame: the search ends here)\n"); break; }
  }
  // _CxxThrowException: ExceptionInformation = {magic, object, ThrowInfo, image base}; il2cpp's
  // object is an Il2CppExceptionWrapper { Il2CppException* ex }; ex->klass->name/namespaze at
  // +0x10/+0x18 of Il2CppClass, ex->message (Il2CppString: length at +0x10, UTF-16 at +0x14) at +0x18.
  auto plausible = [](uint64_t p) { return p >= 0x100000000ull && p < 0x800000000000ull && (p & 7) == 0; };
  if (rec->ExceptionCode != 0xe06d7363 || rec->NumberParameters < 3 || !plausible(rec->ExceptionInformation[1])) return;
  const uint64_t ex = *reinterpret_cast<const uint64_t*>(rec->ExceptionInformation[1]);
  if (!plausible(ex)) return;
  const uint64_t klass = *reinterpret_cast<const uint64_t*>(ex);
  if (!plausible(klass)) return;
  const uint64_t name = *reinterpret_cast<const uint64_t*>(klass + 0x10);
  const uint64_t space = *reinterpret_cast<const uint64_t*>(klass + 0x18);
  std::string message;
  const uint64_t msg = *reinterpret_cast<const uint64_t*>(ex + 0x18);
  if (plausible(msg)) {
    const int32_t len = *reinterpret_cast<const int32_t*>(msg + 0x10);
    const auto* chars = reinterpret_cast<const char16_t*>(msg + 0x14);
    for (int32_t i = 0; i < len && i < 300; ++i) message += chars[i] < 0x80 ? static_cast<char>(chars[i]) : '?';
  }
  std::fprintf(stderr, "UB_WIN32   managed exception: %s.%s: %s\n", space && name ? reinterpret_cast<const char*>(space) : "?",
               name ? reinterpret_cast<const char*>(name) : "?", message.c_str());
}

} // namespace Ntdll

// ---- HLE import wrappers -----------------------------------------------------------------------
namespace {

uint64_t RtlCaptureContext(CallContext& c) {
  Ntdll::CaptureContextFromCall(c.Ptr<Context>(0), c);
  return 0;
}

uint64_t RtlLookupFunctionEntry(CallContext& c) {
  uint64_t base = 0;
  auto* fn = Ntdll::LookupFunctionEntry(c.Arg(0), &base);
  Store<uint64_t>(c.Arg(1), base);
  return reinterpret_cast<uint64_t>(fn);
}

uint64_t RtlVirtualUnwind(CallContext& c) {
  void* handlerData = nullptr;
  uint64_t frame = 0;
  auto* handler = Ntdll::VirtualUnwind(static_cast<uint32_t>(c.Arg(0)), c.Arg(1), c.Arg(2),
                                       c.Ptr<RuntimeFunction>(3), c.Ptr<Context>(4), &handlerData, &frame);
  if (c.Arg(5)) Store<uint64_t>(c.Arg(5), reinterpret_cast<uint64_t>(handlerData));
  if (c.Arg(6)) Store<uint64_t>(c.Arg(6), frame);
  return reinterpret_cast<uint64_t>(handler);
}

uint64_t RtlPcToFileHeader(CallContext& c) {
  const uint64_t base = Ntdll::PcToFileHeader(c.Arg(0));
  Store<uint64_t>(c.Arg(1), base);
  return base;
}

uint64_t RtlCaptureStackBackTrace(CallContext& c) {
  const uint32_t skip = static_cast<uint32_t>(c.Arg(0));
  const uint32_t count = static_cast<uint32_t>(c.Arg(1));
  auto* buffer = c.Ptr<uint64_t>(2);
  Context ctx;
  Ntdll::CaptureContextFromCall(&ctx, c);
  uint32_t n = 0;
  for (uint32_t i = 0; i < skip + count && i < 1024 && buffer; ++i) {
    uint64_t base = 0;
    auto* fn = Ntdll::LookupFunctionEntry(ctx.Rip, &base);
    void* hd;
    uint64_t frame;
    Ntdll::VirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &hd, &frame);
    if (!ctx.Rip) break;
    if (i >= skip && n < count) buffer[n++] = ctx.Rip;
  }
  if (c.Arg(3)) {
    uint32_t hash = 0;
    for (uint32_t i = 0; i < n; ++i) hash += static_cast<uint32_t>(buffer[i]);
    Store<uint32_t>(c.Arg(3), hash);
  }
  return n;
}

uint64_t RaiseException(CallContext& c) {
  ExceptionRecord rec;
  std::memset(&rec, 0, sizeof(rec));
  rec.ExceptionCode = static_cast<uint32_t>(c.Arg(0));
  rec.ExceptionFlags = static_cast<uint32_t>(c.Arg(1)) & EXCEPTION_NONCONTINUABLE;
  rec.ExceptionAddress = reinterpret_cast<void*>(c.rip);
  uint32_t count = static_cast<uint32_t>(c.Arg(2));
  const auto* args = c.Ptr<const uint64_t>(3);
  if (count && args) {
    if (count > kExceptionMaximumParameters) count = kExceptionMaximumParameters;
    rec.NumberParameters = count;
    std::memcpy(rec.ExceptionInformation, args, count * sizeof(uint64_t));
  }
  Context ctx;
  Ntdll::CaptureContextFromCall(&ctx, c);
  const Context raised = ctx;
  if (Ntdll::DispatchException(&rec, &ctx)) {
    if (!Ntdll::t_resumeByBridge) Ntdll::TakeResumeInto(c);
  } else {
    std::fprintf(stderr, "UB_WIN32 unhandled exception %#x at %p\n", rec.ExceptionCode, rec.ExceptionAddress);
    // 0x406d1388 is MSVC's thread-naming exception: nobody is meant to handle it.
    if (rec.ExceptionCode != 0x406d1388) Ntdll::DescribeUnhandled(&rec, &raised);
  }
  return 0;
}

uint64_t RtlRaiseException(CallContext& c) {
  auto* rec = c.Ptr<ExceptionRecord>(0);
  rec->ExceptionAddress = reinterpret_cast<void*>(c.rip);
  Context ctx;
  Ntdll::CaptureContextFromCall(&ctx, c);
  if (Ntdll::DispatchException(rec, &ctx)) {
    if (!Ntdll::t_resumeByBridge) Ntdll::TakeResumeInto(c);
  } else {
    std::fprintf(stderr, "UB_WIN32 unhandled RtlRaiseException %#x\n", rec->ExceptionCode);
  }
  return 0;
}

uint64_t RtlUnwindEx(CallContext& c) {
  // During an active dispatch (a guest language handler called us) the unwind must start from the
  // original raise point — the handler's own frame sits above our callback machinery, not on the
  // guest's .pdata chain. A standalone RtlUnwindEx (a manual unwind) captures its own call site.
  if (!Ntdll::t_dispatchActive) {
    Context cap;
    Ntdll::CaptureContextFromCall(&cap, c);
    Ntdll::SetExceptionContext(cap);
  }
  Ntdll::UnwindEx(c.Ptr<void>(0), c.Ptr<void>(1), c.Ptr<ExceptionRecord>(2), c.Ptr<void>(3),
                  c.Ptr<UnwindHistoryTable>(5));
  if (!Ntdll::t_resumeByBridge) Ntdll::TakeResumeInto(c);
  return 0;
}

uint64_t NtContinue(CallContext& c) {
  Ntdll::RequestResume(c.Ptr<Context>(0));
  if (!Ntdll::t_resumeByBridge) Ntdll::TakeResumeInto(c);
  return 0;
}

uint64_t RtlRestoreContext(CallContext& c) {
  Ntdll::RestoreContext(c.Ptr<Context>(0), c.Ptr<ExceptionRecord>(1));
  if (!Ntdll::t_resumeByBridge) Ntdll::TakeResumeInto(c);
  return 0;
}

uint64_t CSpecificHandler(CallContext& c) {
  return static_cast<uint64_t>(static_cast<uint32_t>(Ntdll::CSpecificHandler(
      c.Ptr<ExceptionRecord>(0), c.Ptr<void>(1), c.Ptr<Context>(2), c.Ptr<DispatcherContext>(3))));
}

uint64_t RtlAddFunctionTable(CallContext&) { return kTrue; }
uint64_t RtlDeleteFunctionTable(CallContext&) { return kTrue; }
uint64_t RtlInstallFunctionTableCallback(CallContext&) { return kTrue; }

uintptr_t g_unhandledFilter = 0;  // SetUnhandledExceptionFilter's top-level filter
uint64_t SetUnhandledExceptionFilter(CallContext& c) {
  const uintptr_t previous = g_unhandledFilter;
  g_unhandledFilter = c.Arg(0);
  return previous;
}
uint64_t UnhandledExceptionFilter(CallContext&) { return 1; /* EXCEPTION_EXECUTE_HANDLER */ }

} // namespace

void RegisterNtdllSeh(std::vector<Function>& out) {
  // GameAssembly imports these from KERNEL32; our ubtest.dll imports __C_specific_handler from
  // ntdll. Register under both module names so either binding resolves.
  auto both = [&](const char* name, Handler h) {
    out.push_back({"kernel32.dll", name, h});
    out.push_back({"ntdll.dll", name, h});
  };
  both("RtlCaptureContext", &RtlCaptureContext);
  both("RtlLookupFunctionEntry", &RtlLookupFunctionEntry);
  both("RtlVirtualUnwind", &RtlVirtualUnwind);
  both("RtlUnwindEx", &RtlUnwindEx);
  both("RtlPcToFileHeader", &RtlPcToFileHeader);
  both("RtlCaptureStackBackTrace", &RtlCaptureStackBackTrace);
  both("RtlRaiseException", &RtlRaiseException);
  both("RtlRestoreContext", &RtlRestoreContext);
  both("RtlAddFunctionTable", &RtlAddFunctionTable);
  both("RtlDeleteFunctionTable", &RtlDeleteFunctionTable);
  both("RtlInstallFunctionTableCallback", &RtlInstallFunctionTableCallback);
  out.push_back({"kernel32.dll", "RaiseException", &RaiseException});
  out.push_back({"kernel32.dll", "SetUnhandledExceptionFilter", &SetUnhandledExceptionFilter});
  out.push_back({"kernel32.dll", "UnhandledExceptionFilter", &UnhandledExceptionFilter});
  out.push_back({"ntdll.dll", "RtlRaiseException", &RtlRaiseException});
  out.push_back({"ntdll.dll", "NtContinue", &NtContinue});
  out.push_back({"ntdll.dll", "__C_specific_handler", &CSpecificHandler});
}

// ---- device bridge: non-local resume realization -----------------------------------------------
void SetInvokeDepth(int depth) { Ntdll::t_invokeDepth = depth; }
void SetResumeRealizedByBridge(bool bridge) { Ntdll::t_resumeByBridge = bridge; }

ResumeAction TakePendingResume(uint64_t gpr[16], uint64_t xmm[16][2], uint64_t& rip) {
  if (!Ntdll::t_resumePending) return ResumeAction::None;
  // A nested language-handler sub-call (its HLE depth is deeper than the dispatch that owns the
  // resume): let the callback return first; the resume is realized at that outer level.
  if (Ntdll::t_invokeDepth > Ntdll::t_resumeRootDepth) return ResumeAction::Escape;
  for (int i = 0; i < 16; ++i) gpr[i] = (&Ntdll::t_resumeContext.Rax)[i];
  for (int i = 0; i < 16; ++i) std::memcpy(xmm[i], &Ntdll::t_resumeContext.FltSave.XmmRegisters[i], 16);
  rip = Ntdll::t_resumeContext.Rip;
  Ntdll::t_resumePending = false;
  Ntdll::t_dispatchActive = false;
  return ResumeAction::Apply;
}

} // namespace UnityBridge::Win32
