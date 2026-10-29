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

uint64_t Baselib_SystemSemaphore_CreateInplace(CallContext& c) {
  const uint64_t memory = c.Arg(0);
  std::scoped_lock lock {g_semaphoresMutex};
  g_semaphores[memory] = std::make_unique<Semaphore>();
  return memory;
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

// ---- dynamic libraries -------------------------------------------------------------------
// The module registry decides what can be opened: loaded PE images and UnityBridge's built-in DLLs.

uint64_t Baselib_DynamicLibrary_OpenUtf16(CallContext& c) {
  const auto name = ToUtf8(ReadWide(c.Ptr<const char16_t>(0)));
  const auto* module = FindModuleByName(name);
  if (module == nullptr) {
    const auto* raw = c.Ptr<const uint8_t>(0);
    std::fprintf(stderr, "UB_WIN32 baselib: cannot open %s (first bytes %02x %02x %02x %02x %02x %02x %02x %02x)\n",
                 name.c_str(), raw ? raw[0] : 0, raw ? raw[1] : 0, raw ? raw[2] : 0, raw ? raw[3] : 0, raw ? raw[4] : 0,
                 raw ? raw[5] : 0, raw ? raw[6] : 0, raw ? raw[7] : 0);
    SetErrorState(c.Arg(1), kErrorFailed);
    return 0;
  }
  SetErrorState(c.Arg(1), 0);
  return module->base;
}

uint64_t Baselib_DynamicLibrary_FromNativeHandle(CallContext& c) {
  SetErrorState(c.Arg(2), 0);
  return c.Arg(0);
}

uint64_t Baselib_DynamicLibrary_GetFunction(CallContext& c) {
  const auto* module = FindModuleByHandle(c.Arg(0));
  const char* name = c.Ptr<const char>(1);
  const auto address = module && module->lookup && name ? module->lookup(name) : std::nullopt;
  if (!address)
    std::fprintf(stderr, "UB_WIN32 baselib: no function %s in %s\n", name ? name : "(null)",
                 module ? module->name.c_str() : "(unknown module)");
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
}

} // namespace UnityBridge::Win32
