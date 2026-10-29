// SPDX-License-Identifier: GPL-3.0-or-later
// KERNEL32 waitable objects: events, mutexes, semaphores, threads, and waiting on one or several
// of them.
// All objects share one lock and one condition variable; every state change wakes all waiters,
// which then re-check. Simple and correct for WaitForMultipleObjects (any and all).
#include "common.h"
#include "winnt_seh.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
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

constexpr uint64_t kCurrentThreadPseudoHandle = ~uint64_t {0} - 1;  // GetCurrentThread(): (HANDLE)-2

// Where one host thread's guest code is (see GuestLeft in hle.h). The owning thread pushes the
// guest's registers at each call into the host and pops them on the way back; another thread reads
// them only while this one is suspended outside guest code, when nothing changes them. `inGuest` and
// `suspendCount` are checked against each other with sequentially consistent atomics (the owner sets
// inGuest then reads the count, a suspender raises the count then reads inGuest), so a thread never
// runs guest code while a suspender believes it stopped.
struct ThreadState {
  struct Exit { uint64_t gpr[16]; uint64_t rsp, rip; };
  static constexpr int kMaxDepth = 64;
  Exit exits[kMaxDepth];
  std::atomic<int> depth {0};
  std::atomic<bool> inGuest {false};
  std::atomic<int> suspendCount {0};
  std::atomic<uint64_t> stackBase {0}, stackLimit {0};
  std::atomic<bool> finished {false};
  uint64_t id {};
  std::mutex m;
  std::condition_variable resumed;
};

std::shared_ptr<ThreadState>& CurrentState() {
  thread_local std::shared_ptr<ThreadState> state = [] {
    auto s = std::make_shared<ThreadState>();
    s->id = ThreadId();
    return s;
  }();
  return state;
}

// Blocks while the thread is suspended, then marks it running guest code.
void EnterGuestCode(ThreadState& s) {
  for (;;) {
    s.inGuest.store(true);
    if (s.suspendCount.load() == 0) return;
    s.inGuest.store(false);
    std::unique_lock lock {s.m};
    s.resumed.wait(lock, [&] { return s.suspendCount.load() == 0; });
  }
}

// A guest thread: one host thread that runs the start routine through the ThreadRunner. Signalled
// once it has finished. CREATE_SUSPENDED holds it before the start routine until ResumeThread
// (`suspendCount`); a running thread is suspended through its ThreadState.
struct Thread final : Object {
  uint64_t id {};
  int32_t suspendCount {};
  bool finished {};
  DWORD exitCode {};
  std::shared_ptr<ThreadState> state;
  bool Signalled() const override { return finished; }
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

// ---- threads -----------------------------------------------------------------------------

constexpr DWORD STILL_ACTIVE = 259, CREATE_SUSPENDED = 0x4;
ThreadRunner g_threadRunner = nullptr;

// CreateThread(attributes, stackSize, start, parameter, flags, threadId*)
uint64_t CreateThread(CallContext& c) {
  const uintptr_t start = c.Arg(2);
  const uint64_t parameter = c.Arg(3);
  const auto flags = static_cast<DWORD>(c.Arg(4));
  if (g_threadRunner == nullptr || start == 0) return SetError(ERROR_NOT_SUPPORTED, 0);
  auto thread = std::make_shared<Thread>();
  thread->suspendCount = (flags & CREATE_SUSPENDED) ? 1 : 0;
  const uint64_t handle = Insert(thread);
  // The new thread reports its id before CreateThread returns, as Windows fills lpThreadId.
  std::thread([thread, start, parameter, runner = g_threadRunner] {
    {
      std::unique_lock lock {g_lock};
      thread->id = static_cast<DWORD>(ThreadId());
      thread->state = CurrentState();
      g_changed.notify_all();
      g_changed.wait(lock, [&] { return thread->suspendCount <= 0; });
    }
    const auto code = static_cast<DWORD>(runner(start, parameter));
    CurrentState()->finished.store(true);
    {
      std::scoped_lock lock {g_lock};
      thread->exitCode = code;
      thread->finished = true;
    }
    g_changed.notify_all();
  }).detach();
  std::unique_lock lock {g_lock};
  g_changed.wait(lock, [&] { return thread->id != 0; });
  Store<DWORD>(c.Arg(5), static_cast<DWORD>(thread->id));
  return handle;
}

// The ThreadState behind a thread handle, or nullptr. Caller holds g_lock.
std::shared_ptr<ThreadState> StateOf(uint64_t handle) {
  auto* thread = Find<Thread>(handle);
  return thread ? thread->state : nullptr;
}

// Returns the previous suspend count. A thread created suspended that has not started yet is held
// by its Thread's count; a running one by its ThreadState's.
uint64_t ResumeThread(CallContext& c) {
  int32_t previous;
  std::shared_ptr<ThreadState> state;
  {
    std::scoped_lock lock {g_lock};
    auto* thread = Find<Thread>(c.Arg(0));
    if (thread == nullptr) return SetError(ERROR_INVALID_HANDLE, 0xffffffffu);
    if (thread->suspendCount > 0 || !thread->state) {
      previous = thread->suspendCount;
      if (thread->suspendCount > 0) --thread->suspendCount;
    } else {
      state = thread->state;
    }
  }
  if (!state) {
    g_changed.notify_all();
    return static_cast<DWORD>(previous);
  }
  {
    std::scoped_lock lock {state->m};
    previous = state->suspendCount.load();
    if (previous > 0) state->suspendCount.store(previous - 1);
  }
  state->resumed.notify_all();
  return static_cast<DWORD>(previous);
}

// SuspendThread: raises the count, then waits until the thread is outside guest code (at an import,
// an InternalCall, or not in guest code at all), where it stays until resumed. A thread that keeps
// running guest code without calling out is waited for up to 2 s, then reported as it was last seen.
uint64_t SuspendThread(CallContext& c) {
  std::shared_ptr<ThreadState> state;
  {
    std::scoped_lock lock {g_lock};
    if (Find<Thread>(c.Arg(0)) == nullptr) return SetError(ERROR_INVALID_HANDLE, 0xffffffffu);
    state = StateOf(c.Arg(0));
  }
  if (!state) return 0;  // created suspended, not started: already held
  int previous;
  {
    std::scoped_lock lock {state->m};
    previous = state->suspendCount.load();
    state->suspendCount.store(previous + 1);
  }
  if (state == CurrentState() || state->finished.load()) return static_cast<DWORD>(previous);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (state->inGuest.load()) {
    if (std::chrono::steady_clock::now() > deadline) {
      std::fprintf(stderr, "UB_WIN32 SuspendThread: thread %llu stays in guest code; its last exit is reported\n",
                   static_cast<unsigned long long>(state->id));
      break;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  return static_cast<DWORD>(previous);
}

// GetThreadContext: the registers where the (suspended) thread last left guest code; with no guest
// frames live, an rsp at its stack base (nothing of it to scan). Integer and control registers only.
uint64_t GetThreadContext(CallContext& c) {
  std::shared_ptr<ThreadState> state;
  {
    std::scoped_lock lock {g_lock};
    if (Find<Thread>(c.Arg(0)) == nullptr) return SetError(ERROR_INVALID_HANDLE, kFalse);
    state = StateOf(c.Arg(0));
  }
  auto* ctx = c.Ptr<Context>(1);
  if (ctx == nullptr) return SetError(ERROR_INVALID_PARAMETER, kFalse);
  const uint32_t flags = ctx->ContextFlags;
  for (int i = 0; i < 16; ++i) (&ctx->Rax)[i] = 0;
  ctx->Rip = 0;
  if (state) {
    const int depth = state->depth.load();
    if (depth > 0) {
      const auto& e = state->exits[std::min(depth, ThreadState::kMaxDepth) - 1];
      for (int i = 0; i < 16; ++i) (&ctx->Rax)[i] = e.gpr[i];
      ctx->Rsp = e.rsp;
      ctx->Rip = e.rip;
    } else {
      ctx->Rsp = state->stackBase.load();
    }
  }
  ctx->ContextFlags = flags;
  ctx->SegCs = 0x33;
  ctx->SegSs = 0x2b;
  ctx->EFlags = 0x202;
  return kTrue;
}

uint64_t GetExitCodeThread(CallContext& c) {
  std::scoped_lock lock {g_lock};
  auto* thread = Find<Thread>(c.Arg(0));
  if (thread == nullptr) return SetError(ERROR_INVALID_HANDLE, kFalse);
  Store<DWORD>(c.Arg(1), thread->finished ? thread->exitCode : STILL_ACTIVE);
  return kTrue;
}

uint64_t GetThreadId(CallContext& c) {
  std::scoped_lock lock {g_lock};
  auto* thread = Find<Thread>(c.Arg(0));
  return thread ? thread->id : SetError(ERROR_INVALID_HANDLE, 0);
}

// Priorities are accepted and not applied; THREAD_PRIORITY_NORMAL is reported.
uint64_t SetThreadPriority(CallContext&) { return kTrue; }
uint64_t GetThreadPriority(CallContext&) { return 0; }

uint64_t CloseHandle(CallContext& c) {
  if (CloseFileObject(c.Arg(0))) return kTrue;
  std::scoped_lock lock {g_lock};
  g_handles.erase(c.Arg(0));  // other handles (files, pseudo handles) are not tracked here yet
  return kTrue;
}

uint64_t DuplicateHandle(CallContext& c) {
  const uint64_t source = c.Arg(1);
  std::scoped_lock lock {g_lock};
  const auto it = g_handles.find(source);
  if (it == g_handles.end()) {
    if (source == kCurrentThreadPseudoHandle) {
      // A real handle to the calling thread (the GC registers each thread this way, to suspend it).
      auto thread = std::make_shared<Thread>();
      thread->id = ThreadId();
      thread->state = CurrentState();
      const uint64_t handle = g_nextHandle;
      g_nextHandle += 4;
      g_handles[handle] = std::move(thread);
      Store<uint64_t>(c.Arg(3), handle);
      return kTrue;
    }
    // Not one of ours (another pseudo handle, e.g. GetCurrentProcess()): hand back the same value.
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

void SetThreadRunner(ThreadRunner runner) { g_threadRunner = runner; }

void GuestLeft(const CallContext& call) {
  ThreadState& s = *CurrentState();
  const int d = s.depth.load();
  if (d < ThreadState::kMaxDepth) {
    auto& e = s.exits[d];
    for (int i = 0; i < 16; ++i) e.gpr[i] = call.x86[i];
    e.rsp = call.rsp + 8;  // the caller's rsp once the import returns, as RtlCaptureContext reports it
    e.rip = call.rip;
  }
  s.depth.store(d + 1);
  s.inGuest.store(false);
}

void GuestReturning() {
  ThreadState& s = *CurrentState();
  EnterGuestCode(s);
  s.depth.store(s.depth.load() - 1);
}

void GuestEntering() { EnterGuestCode(*CurrentState()); }

void GuestExited() { CurrentState()->inGuest.store(false); }

void NoteGuestStack(uint64_t base, uint64_t limit) {
  ThreadState& s = *CurrentState();
  s.stackBase.store(base);
  s.stackLimit.store(limit);
}

void RegisterKernel32Sync(std::vector<Function>& out) {
  const std::string k = "kernel32.dll";
#define UB_FN(name) out.push_back({k, #name, &name})
  UB_FN(CreateEventW); UB_FN(SetEvent); UB_FN(ResetEvent);
  UB_FN(CreateMutexW); UB_FN(ReleaseMutex);
  UB_FN(CreateSemaphoreW); UB_FN(ReleaseSemaphore);
  UB_FN(WaitForSingleObjectEx); UB_FN(WaitForMultipleObjectsEx);
  UB_FN(CloseHandle); UB_FN(DuplicateHandle);
  UB_FN(CreateThread); UB_FN(ResumeThread); UB_FN(SuspendThread); UB_FN(GetThreadContext); UB_FN(GetExitCodeThread); UB_FN(GetThreadId);
  UB_FN(SetThreadPriority); UB_FN(GetThreadPriority);
  out.push_back({k, "WaitForSingleObject", &WaitForSingleObjectEx});
#undef UB_FN
}

} // namespace UnityBridge::Win32
