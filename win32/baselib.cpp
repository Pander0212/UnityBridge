// SPDX-License-Identifier: GPL-3.0-or-later
// Unity's baselib.dll, the platform layer IL2CPP's GameAssembly.dll imports 13 functions from.
// Implemented natively instead of translating Unity's x86-64 DLL. Signatures are read from the
// MSVC-decorated names; the handles are one-pointer structs, passed and returned in registers.
//
// Unverified against Unity's headers (not public): the error codes written into
// Baselib_ErrorState. Success writes 0 into its first field (the code), failure a nonzero value.
#include "common.h"

#include <chrono>
#include <condition_variable>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#if defined(__APPLE__)
#include <pthread.h>
#else
#include <unistd.h>
#endif

namespace UnityBridge::Win32 {
namespace {

constexpr uint32_t kErrorFailed = 1;  // see the file comment
void SetErrorState(uint64_t state, uint32_t code) { Store<uint32_t>(state, code); }

// ---- threads -----------------------------------------------------------------------------

uint64_t Baselib_Thread_GetCurrentThreadId(CallContext&) {
  uint64_t id = 0;
#if defined(__APPLE__)
  pthread_threadid_np(nullptr, &id);
#else
  id = static_cast<uint64_t>(gettid());
#endif
  return id;
}

uint64_t Baselib_Thread_YieldExecution(CallContext&) {
  std::this_thread::yield();
  return 0;
}

// ---- system semaphores -------------------------------------------------------------------
// "Inplace": the caller supplies the memory. Its address is the handle and keys the host object.

class Semaphore {
public:
  void Acquire() {
    std::unique_lock lock {mutex_};
    cv_.wait(lock, [&] { return count_ > 0; });
    --count_;
  }
  bool TryAcquire() {
    std::scoped_lock lock {mutex_};
    if (count_ == 0) return false;
    --count_;
    return true;
  }
  bool TryAcquireFor(uint32_t ms) {
    std::unique_lock lock {mutex_};
    if (!cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return count_ > 0; })) return false;
    --count_;
    return true;
  }
  void Release(uint32_t n) {
    {
      std::scoped_lock lock {mutex_};
      count_ += n;
    }
    if (n == 1) cv_.notify_one();
    else cv_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  uint64_t count_ = 0;
};

std::mutex g_semaphoresMutex;
std::map<uint64_t, std::unique_ptr<Semaphore>> g_semaphores;

Semaphore* FindSemaphore(uint64_t handle) {
  std::scoped_lock lock {g_semaphoresMutex};
  const auto it = g_semaphores.find(handle);
  return it == g_semaphores.end() ? nullptr : it->second.get();
}

// The functions that return a Baselib_*_Handle by value (CreateInplace, DynamicLibrary_OpenUtf16,
// DynamicLibrary_FromNativeHandle) are C++ functions of il2cpp_baselib. Their handle comes back either
// in RAX (an 8-byte aggregate under the x64 ABI) or through a hidden pointer, RCX being the caller's
// return slot, the declared arguments following and RAX returning that pointer. The device's
// GameAssembly behaved like the second (M3f: the "library name" read as plain arguments was "B", and
// the error state went unseen), the ABI's rule says the first; the call tells: a return slot is a local
// of the caller, just above its stack pointer, which a semaphore's memory or a library name is not.
bool HiddenReturnSlot(const CallContext& c, const char* function, std::atomic<int>& said) {
  const uint64_t a0 = c.Arg(0);
  const bool hidden = a0 >= c.rsp && a0 < c.rsp + (64u << 10);
  if (said.fetch_add(1) < 2)
    std::fprintf(stderr, "UB_WIN32 baselib: %s returns its handle %s\n", function,
                 hidden ? "through a hidden return slot (RCX)" : "in RAX");
  return hidden;
}

uint64_t ReturnHandle(CallContext& c, bool hidden, uint64_t handle) {
  if (!hidden) return handle;
  Store<uint64_t>(c.Arg(0), handle);
  return c.Arg(0);
}

uint64_t Baselib_SystemSemaphore_CreateInplace(CallContext& c) {  // ([return slot,] memory)
  static std::atomic<int> said {0};
  const bool hidden = HiddenReturnSlot(c, "Baselib_SystemSemaphore_CreateInplace", said);
  const uint64_t memory = c.Arg(hidden ? 1 : 0);
  {
    std::scoped_lock lock {g_semaphoresMutex};
    g_semaphores[memory] = std::make_unique<Semaphore>();
  }
  return ReturnHandle(c, hidden, memory);
}

uint64_t Baselib_SystemSemaphore_FreeInplace(CallContext& c) {
  std::scoped_lock lock {g_semaphoresMutex};
  g_semaphores.erase(c.Arg(0));
  return 0;
}

uint64_t Baselib_SystemSemaphore_Acquire(CallContext& c) {
  if (auto* s = FindSemaphore(c.Arg(0))) s->Acquire();
  return 0;
}

uint64_t Baselib_SystemSemaphore_TryAcquire(CallContext& c) {
  auto* s = FindSemaphore(c.Arg(0));
  return s && s->TryAcquire() ? 1 : 0;
}

uint64_t Baselib_SystemSemaphore_TryTimedAcquire(CallContext& c) {
  auto* s = FindSemaphore(c.Arg(0));
  return s && s->TryAcquireFor(static_cast<uint32_t>(c.Arg(1))) ? 1 : 0;
}

uint64_t Baselib_SystemSemaphore_Release(CallContext& c) {
  if (auto* s = FindSemaphore(c.Arg(0))) s->Release(static_cast<uint32_t>(c.Arg(1)));
  return 0;
}

// ---- Unity 6 (M6): timer, futex, CPU features -----------------------------------------------
// Ticks are nanoseconds of the steady clock, so the conversion factor (a data export) is 1.

uint64_t Baselib_Timer_GetHighPrecisionTimerTicks(CallContext&) {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One wait queue for all addresses: Notify wakes every waiter, each rechecks its own word. Futex
// semantics allow spurious wake-ups.
std::mutex g_futexMutex;
std::condition_variable g_futex;

uint64_t Baselib_SystemFutex_Wait(CallContext& c) {  // (int32_t* address, int32_t expected, uint32_t timeoutMs)
  auto* word = c.Ptr<std::atomic<int32_t>>(0);
  const auto expected = static_cast<int32_t>(c.Arg(1));
  const auto ms = static_cast<uint32_t>(c.Arg(2));
  std::unique_lock lock {g_futexMutex};
  if (!word || word->load() != expected) return 0;
  if (ms == 0xffffffffu) g_futex.wait(lock);
  else g_futex.wait_for(lock, std::chrono::milliseconds(ms));
  return 0;
}

uint64_t Baselib_SystemFutex_Notify(CallContext&) {  // (int32_t* address, uint32_t count, strategy)
  std::scoped_lock lock {g_futexMutex};
  g_futex.notify_all();
  return 0;
}

// Data exports: the conversion factor, and the CPUID table baselib reads features from, left
// empty (no SSE4/AVX bits): il2cpp then keeps to its baseline code paths, which FEXCore runs.
alignas(16) const double g_tickToNanoseconds = 1.0;
alignas(16) const uint8_t g_cpuIdArray[4096] = {};

// ---- dynamic libraries -------------------------------------------------------------------
// The module registry decides what can be opened: loaded PE images and UnityBridge's built-in DLLs.

uint64_t Baselib_DynamicLibrary_OpenUtf16(CallContext& c) {  // ([return slot,] pathnameUtf16, errorState)
  static std::atomic<int> said {0};
  const bool hidden = HiddenReturnSlot(c, "Baselib_DynamicLibrary_OpenUtf16", said);
  const int a = hidden ? 1 : 0;
  const auto name = c.Arg(a) ? ToUtf8(ReadWide(c.Ptr<const char16_t>(a))) : std::string();
  // A built-in DLL (bcrypt.dll, ...) gets its handle on first use, as LoadLibrary does.
  const uint64_t handle = ModuleHandle(name);
  if (handle == 0) {
    // What the caller passed, raw: the three argument registers, the call site, the bytes behind each.
    const auto* main = MainModule();
    std::fprintf(stderr, "UB_WIN32 baselib: cannot open %s (rcx=%#llx rdx=%#llx r8=%#llx rsp=%#llx, called from +%#llx)\n",
                 name.c_str(), static_cast<unsigned long long>(c.Arg(0)), static_cast<unsigned long long>(c.Arg(1)),
                 static_cast<unsigned long long>(c.Arg(2)), static_cast<unsigned long long>(c.rsp),
                 static_cast<unsigned long long>(main && c.rip > main->base ? c.rip - main->base : c.rip));
    for (int i = 0; i < 2; ++i) {  // RCX and RDX: pointers under either convention
      const uint64_t p = c.Arg(i);
      if (p < 0x100000000ull || p >= 0x800000000000ull) continue;
      const auto* b = reinterpret_cast<const uint8_t*>(p);
      char hex[3 * 32 + 1];
      for (int k = 0; k < 32; ++k) std::snprintf(hex + 3 * k, 4, "%02x ", b[k]);
      std::fprintf(stderr, "UB_WIN32   arg%d -> %s\n", i, hex);
    }
    SetErrorState(c.Arg(a + 1), kErrorFailed);
    return ReturnHandle(c, hidden, 0);
  }
  SetErrorState(c.Arg(a + 1), 0);
  return ReturnHandle(c, hidden, handle);
}

uint64_t Baselib_DynamicLibrary_FromNativeHandle(CallContext& c) {  // ([return slot,] handle, type, errorState)
  static std::atomic<int> said {0};
  const bool hidden = HiddenReturnSlot(c, "Baselib_DynamicLibrary_FromNativeHandle", said);
  const int a = hidden ? 1 : 0;
  SetErrorState(c.Arg(a + 2), 0);
  return ReturnHandle(c, hidden, c.Arg(a));
}

uint64_t Baselib_DynamicLibrary_GetFunction(CallContext& c) {
  const auto* module = FindModuleByHandle(c.Arg(0));
  const char* name = c.Ptr<const char>(1);
  auto address = module && module->lookup && name ? module->lookup(name) : std::nullopt;
  // A handle we never gave out (il2cpp carries on after a failed open, M3f): a function by that name
  // in any built-in DLL, if one implements it.
  if (!module && name) {
    if (auto* runtime = Runtime::Current()) {
      if (const Function* f = runtime->LookupAnyDll(name)) {
        address = runtime->Resolve(f->dll, name);
        std::fprintf(stderr, "UB_WIN32 baselib: %s from an unknown handle %#llx: %s's\n", name,
                     static_cast<unsigned long long>(c.Arg(0)), f->dll.c_str());
      }
    }
  }
  if (!address)
    std::fprintf(stderr, "UB_WIN32 baselib: no function %s in %s\n", name ? name : "(null)",
                 module ? module->name.c_str() : "(unknown module)");
  else if (module && name)
    std::fprintf(stderr, "UB_WIN32 baselib: %s!%s\n", module->name.c_str(), name);  // P/Invokes resolve once each
  SetErrorState(c.Arg(2), address ? 0 : kErrorFailed);
  return address.value_or(0);
}

uint64_t Baselib_DynamicLibrary_Close(CallContext&) { return 0; }

uint64_t Baselib_ErrorState_Explain(CallContext& c) {
  const auto code = Load<uint32_t>(c.Arg(0));
  char text[96];
  const int n = std::snprintf(text, sizeof(text), "baselib error %u (UnityBridge)", code);
  auto* out = c.Ptr<char>(1);
  const auto size = static_cast<uint32_t>(c.Arg(2));
  if (out && size) {
    const auto copy = std::min<uint32_t>(static_cast<uint32_t>(n), size - 1);
    std::memcpy(out, text, copy);
    out[copy] = 0;
  }
  return static_cast<uint32_t>(n) + 1;  // length needed, including the terminator
}

} // namespace

void RegisterBaselib(std::vector<Function>& out) {
  const std::string b = "baselib.dll";
  out.push_back({b, "?Baselib_Thread_GetCurrentThreadId@il2cpp_baselib@@YA_JXZ", &Baselib_Thread_GetCurrentThreadId});
  out.push_back({b, "?Baselib_Thread_YieldExecution@il2cpp_baselib@@YAXXZ", &Baselib_Thread_YieldExecution});
  out.push_back({b, "?Baselib_SystemSemaphore_CreateInplace@il2cpp_baselib@@YA?AUBaselib_SystemSemaphore_Handle@1@PEAX@Z",
                 &Baselib_SystemSemaphore_CreateInplace});
  out.push_back({b, "?Baselib_SystemSemaphore_FreeInplace@il2cpp_baselib@@YAXUBaselib_SystemSemaphore_Handle@1@@Z",
                 &Baselib_SystemSemaphore_FreeInplace});
  out.push_back({b, "?Baselib_SystemSemaphore_Acquire@il2cpp_baselib@@YAXUBaselib_SystemSemaphore_Handle@1@@Z",
                 &Baselib_SystemSemaphore_Acquire});
  out.push_back({b, "?Baselib_SystemSemaphore_TryAcquire@il2cpp_baselib@@YA_NUBaselib_SystemSemaphore_Handle@1@@Z",
                 &Baselib_SystemSemaphore_TryAcquire});
  out.push_back({b, "?Baselib_SystemSemaphore_TryTimedAcquire@il2cpp_baselib@@YA_NUBaselib_SystemSemaphore_Handle@1@I@Z",
                 &Baselib_SystemSemaphore_TryTimedAcquire});
  out.push_back({b, "?Baselib_SystemSemaphore_Release@il2cpp_baselib@@YAXUBaselib_SystemSemaphore_Handle@1@I@Z",
                 &Baselib_SystemSemaphore_Release});
  out.push_back({b, "?Baselib_DynamicLibrary_OpenUtf16@il2cpp_baselib@@YA?AUBaselib_DynamicLibrary_Handle@1@PEBGPEAUBaselib_ErrorState@1@@Z",
                 &Baselib_DynamicLibrary_OpenUtf16});
  out.push_back({b, "?Baselib_DynamicLibrary_FromNativeHandle@il2cpp_baselib@@YA?AUBaselib_DynamicLibrary_Handle@1@_KIPEAUBaselib_ErrorState@1@@Z",
                 &Baselib_DynamicLibrary_FromNativeHandle});
  out.push_back({b, "?Baselib_DynamicLibrary_GetFunction@il2cpp_baselib@@YAPEAXUBaselib_DynamicLibrary_Handle@1@PEBDPEAUBaselib_ErrorState@1@@Z",
                 &Baselib_DynamicLibrary_GetFunction});
  out.push_back({b, "?Baselib_DynamicLibrary_Close@il2cpp_baselib@@YAXUBaselib_DynamicLibrary_Handle@1@@Z",
                 &Baselib_DynamicLibrary_Close});
  out.push_back({b, "?Baselib_ErrorState_Explain@il2cpp_baselib@@YAIPEBUBaselib_ErrorState@1@QEADIW4Baselib_ErrorState_ExplainVerbosity@1@@Z",
                 &Baselib_ErrorState_Explain});
  // Unity 6
  out.push_back({b, "?Baselib_Timer_GetHighPrecisionTimerTicks@il2cpp_baselib@@YA_KXZ", &Baselib_Timer_GetHighPrecisionTimerTicks});
  out.push_back({b, "?Baselib_SystemFutex_Wait@il2cpp_baselib@@YAXPEAHHI@Z", &Baselib_SystemFutex_Wait});
  out.push_back({b, "?Baselib_SystemFutex_Notify@il2cpp_baselib@@YAXPEAHIW4Baselib_WakeupFallbackStrategy@1@@Z",
                 &Baselib_SystemFutex_Notify});
  RegisterDataExport(b, "?Baselib_Timer_TickToNanosecondsConversionFactor@il2cpp_baselib@@3NB",
                     reinterpret_cast<uintptr_t>(&g_tickToNanoseconds));
  RegisterDataExport(b, "?Detail_Baselib_Cpu_FeatureData_CpuIdArray@il2cpp_baselib@@3QBUDetail_Baselib_Cpu_FeatureData_CpuId@1@B",
                     reinterpret_cast<uintptr_t>(g_cpuIdArray));
}

} // namespace UnityBridge::Win32
