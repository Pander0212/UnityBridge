// SPDX-License-Identifier: GPL-3.0-or-later
// KERNEL32 waitable objects: events, mutexes, semaphores, and waiting on one or several of them.
// All objects share one lock and one condition variable; every state change wakes all waiters,
// which then re-check. Simple and correct for WaitForMultipleObjects (any and all).
#include "common.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <pthread.h>
#else
#include <unistd.h>
#endif

namespace UnityBridge::Win32 {
namespace {

constexpr DWORD WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 0x102, WAIT_FAILED = 0xffffffffu, INFINITE = 0xffffffffu;
constexpr DWORD ERROR_NOT_OWNER = 288, ERROR_TOO_MANY_POSTS = 298;

uint64_t ThreadId() {
  uint64_t id = 0;
#if defined(__APPLE__)
  pthread_threadid_np(nullptr, &id);
#else
  id = static_cast<uint64_t>(gettid());
#endif
  return id;
}

std::mutex g_lock;                     // guards every object's state
std::condition_variable g_changed;     // notified on every state change

struct Object {
  virtual ~Object() = default;
  virtual bool Signalled() const = 0;  // under g_lock
  virtual void Acquire() {}            // consume the signal; under g_lock, only when Signalled()
};

struct Event final : Object {
  bool manualReset {}, set {};
  bool Signalled() const override { return set; }
  void Acquire() override { if (!manualReset) set = false; }
};

struct Mutex final : Object {
  uint64_t owner {};
  uint32_t recursion {};
  bool Signalled() const override { return owner == 0 || owner == ThreadId(); }
  void Acquire() override { owner = ThreadId(); ++recursion; }
};

struct Semaphore final : Object {
  int32_t count {}, maximum {};
  bool Signalled() const override { return count > 0; }
  void Acquire() override { --count; }
};

std::map<uint64_t, std::shared_ptr<Object>> g_handles;
uint64_t g_nextHandle = 0x1000;

uint64_t Insert(std::shared_ptr<Object> object) {
  std::scoped_lock lock {g_lock};
  const uint64_t handle = g_nextHandle;
  g_nextHandle += 4;  // Windows handles are multiples of 4
  g_handles[handle] = std::move(object);
  return handle;
}

template <typename T>
T* Find(uint64_t handle) {  // under g_lock
  const auto it = g_handles.find(handle);
  return it == g_handles.end() ? nullptr : dynamic_cast<T*>(it->second.get());
}

uint64_t CreateEventW(CallContext& c) {
  auto event = std::make_shared<Event>();
  event->manualReset = c.Arg(1) != 0;
  event->set = c.Arg(2) != 0;
  return Insert(std::move(event));
}

uint64_t SetEvent(CallContext& c) {
  {
    std::scoped_lock lock {g_lock};
    auto* event = Find<Event>(c.Arg(0));
    if (event == nullptr) return SetError(ERROR_INVALID_HANDLE, kFalse);
    event->set = true;
  }
  g_changed.notify_all();
  return kTrue;
}

uint64_t ResetEvent(CallContext& c) {
  std::scoped_lock lock {g_lock};
  auto* event = Find<Event>(c.Arg(0));
  if (event == nullptr) return SetError(ERROR_INVALID_HANDLE, kFalse);
  event->set = false;
  return kTrue;
}

uint64_t CreateMutexW(CallContext& c) {
  auto mutex = std::make_shared<Mutex>();
  if (c.Arg(1)) mutex->Acquire();  // bInitialOwner
  return Insert(std::move(mutex));
}

uint64_t ReleaseMutex(CallContext& c) {
  {
    std::scoped_lock lock {g_lock};
    auto* mutex = Find<Mutex>(c.Arg(0));
    if (mutex == nullptr) return SetError(ERROR_INVALID_HANDLE, kFalse);
    if (mutex->owner != ThreadId() || mutex->recursion == 0) return SetError(ERROR_NOT_OWNER, kFalse);
    if (--mutex->recursion == 0) mutex->owner = 0;
  }
  g_changed.notify_all();
  return kTrue;
}

uint64_t CreateSemaphoreW(CallContext& c) {
  const auto initial = static_cast<int32_t>(c.Arg(1));
  const auto maximum = static_cast<int32_t>(c.Arg(2));
  if (maximum <= 0 || initial < 0 || initial > maximum) return SetError(ERROR_INVALID_PARAMETER, 0);
  auto semaphore = std::make_shared<Semaphore>();
  semaphore->count = initial;
  semaphore->maximum = maximum;
  return Insert(std::move(semaphore));
}

uint64_t ReleaseSemaphore(CallContext& c) {
  const auto release = static_cast<int32_t>(c.Arg(1));
  {
    std::scoped_lock lock {g_lock};
    auto* semaphore = Find<Semaphore>(c.Arg(0));
    if (semaphore == nullptr) return SetError(ERROR_INVALID_HANDLE, kFalse);
    if (release <= 0 || semaphore->count > semaphore->maximum - release) return SetError(ERROR_TOO_MANY_POSTS, kFalse);
    Store<int32_t>(c.Arg(2), semaphore->count);  // lpPreviousCount
    semaphore->count += release;
  }
  g_changed.notify_all();
  return kTrue;
}

// Waits until one (any) or every (all) object is signalled, then acquires it/them atomically.
DWORD Wait(const std::vector<uint64_t>& handles, bool all, DWORD ms) {
  std::unique_lock lock {g_lock};
  std::vector<std::shared_ptr<Object>> objects;  // kept alive even if another thread closes a handle
  for (const auto h : handles) {
    const auto it = g_handles.find(h);
    if (it == g_handles.end()) {
      SetLastErrorValue(ERROR_INVALID_HANDLE);
      return WAIT_FAILED;
    }
    objects.push_back(it->second);
  }
  DWORD result = WAIT_TIMEOUT;
  auto ready = [&] {
    if (all) {
      for (const auto& o : objects) if (!o->Signalled()) return false;
      for (const auto& o : objects) o->Acquire();
      result = WAIT_OBJECT_0;
      return true;
    }
    for (size_t i = 0; i < objects.size(); ++i) {
      if (objects[i]->Signalled()) {
        objects[i]->Acquire();
        result = WAIT_OBJECT_0 + static_cast<DWORD>(i);
        return true;
      }
    }
    return false;
  };
  if (ms == INFINITE) g_changed.wait(lock, ready);
  else g_changed.wait_for(lock, std::chrono::milliseconds(ms), ready);
  return result;
}

uint64_t WaitForSingleObjectEx(CallContext& c) {
  return Wait({c.Arg(0)}, false, static_cast<DWORD>(c.Arg(1)));
}

uint64_t WaitForMultipleObjectsEx(CallContext& c) {
  const auto count = static_cast<DWORD>(c.Arg(0));
  if (count == 0 || count > 64 || c.Arg(1) == 0) return SetError(ERROR_INVALID_PARAMETER, WAIT_FAILED);
  std::vector<uint64_t> handles(count);
  for (DWORD i = 0; i < count; ++i) handles[i] = Load<uint64_t>(c.Arg(1) + i * 8);
  return Wait(handles, c.Arg(2) != 0, static_cast<DWORD>(c.Arg(3)));
}

uint64_t CloseHandle(CallContext& c) {
  std::scoped_lock lock {g_lock};
  g_handles.erase(c.Arg(0));  // other handles (files, pseudo handles) are not tracked here yet
  return kTrue;
}

uint64_t DuplicateHandle(CallContext& c) {
  const uint64_t source = c.Arg(1);
  std::scoped_lock lock {g_lock};
  const auto it = g_handles.find(source);
  if (it == g_handles.end()) {
    // Not one of ours (a pseudo handle such as GetCurrentThread()): hand back the same value.
    Store<uint64_t>(c.Arg(3), source);
    return kTrue;
  }
  const uint64_t handle = g_nextHandle;
  g_nextHandle += 4;
  g_handles[handle] = it->second;
  Store<uint64_t>(c.Arg(3), handle);
  return kTrue;
}

} // namespace

void RegisterKernel32Sync(std::vector<Function>& out) {
  const std::string k = "kernel32.dll";
#define UB_FN(name) out.push_back({k, #name, &name})
  UB_FN(CreateEventW); UB_FN(SetEvent); UB_FN(ResetEvent);
  UB_FN(CreateMutexW); UB_FN(ReleaseMutex);
  UB_FN(CreateSemaphoreW); UB_FN(ReleaseSemaphore);
  UB_FN(WaitForSingleObjectEx); UB_FN(WaitForMultipleObjectsEx);
  UB_FN(CloseHandle); UB_FN(DuplicateHandle);
  out.push_back({k, "WaitForSingleObject", &WaitForSingleObjectEx});
#undef UB_FN
}

} // namespace UnityBridge::Win32
