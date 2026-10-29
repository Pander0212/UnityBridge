// SPDX-License-Identifier: GPL-3.0-or-later
// KERNEL32 functions, written from Microsoft's documented behaviour. Guest pointers are host
// pointers (FEXCore runs the guest in this process's address space); Windows' `long`/DWORD are
// 32 bits and its wchar_t is UTF-16.
#include "common.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

namespace UnityBridge::Win32 {
namespace {

thread_local DWORD t_lastError = 0;

// ---- errors ------------------------------------------------------------------------------

uint64_t GetLastError(CallContext&) { return t_lastError; }
uint64_t SetLastError(CallContext& c) { t_lastError = static_cast<DWORD>(c.Arg(0)); return 0; }

// ---- heap --------------------------------------------------------------------------------

constexpr uint64_t kProcessHeap = 0x0000'7ffe'0000'1000ull;
constexpr DWORD HEAP_ZERO_MEMORY = 0x8, HEAP_REALLOC_IN_PLACE_ONLY = 0x10;

size_t UsableSize(void* p) {
#if defined(__APPLE__)
  return malloc_size(p);
#else
  return malloc_usable_size(p);
#endif
}

uint64_t GetProcessHeap(CallContext&) { return kProcessHeap; }

uint64_t HeapAlloc(CallContext& c) {
  const auto flags = static_cast<DWORD>(c.Arg(1));
  const size_t size = c.Arg(2);
  void* p = (flags & HEAP_ZERO_MEMORY) ? std::calloc(1, size ? size : 1) : std::malloc(size ? size : 1);
  return p ? reinterpret_cast<uint64_t>(p) : SetError(ERROR_NOT_ENOUGH_MEMORY, 0);
}

uint64_t HeapFree(CallContext& c) {
  std::free(c.Ptr<void>(2));
  return kTrue;
}

uint64_t HeapReAlloc(CallContext& c) {
  const auto flags = static_cast<DWORD>(c.Arg(1));
  void* old = c.Ptr<void>(2);
  const size_t size = c.Arg(3);
  if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
    return (old && UsableSize(old) >= size) ? reinterpret_cast<uint64_t>(old) : SetError(ERROR_NOT_ENOUGH_MEMORY, 0);
  }
  const size_t before = old ? UsableSize(old) : 0;
  void* p = std::realloc(old, size ? size : 1);
  if (!p) return SetError(ERROR_NOT_ENOUGH_MEMORY, 0);
  if ((flags & HEAP_ZERO_MEMORY) && size > before) std::memset(static_cast<uint8_t*>(p) + before, 0, size - before);
  return reinterpret_cast<uint64_t>(p);
}

uint64_t HeapSize(CallContext& c) {
  void* p = c.Ptr<void>(2);
  return p ? UsableSize(p) : ~uint64_t {0};
}

uint64_t HeapQueryInformation(CallContext&) { return SetError(ERROR_INVALID_PARAMETER, kFalse); }

uint64_t GlobalAlloc(CallContext& c) {
  // GMEM_ZEROINIT = 0x40; GMEM_MOVEABLE handles are not supported (IL2CPP uses GPTR).
  const size_t size = c.Arg(1);
  void* p = (c.Arg(0) & 0x40) ? std::calloc(1, size ? size : 1) : std::malloc(size ? size : 1);
  return p ? reinterpret_cast<uint64_t>(p) : SetError(ERROR_NOT_ENOUGH_MEMORY, 0);
}

uint64_t GlobalFree(CallContext& c) {
  std::free(c.Ptr<void>(0));
  return 0;
}

// ---- virtual memory ----------------------------------------------------------------------
// Pages are never executable for the host: on iPhone FEXCore translates guest code from
// ordinary readable memory, so PAGE_EXECUTE_* maps to the matching data protection.

constexpr DWORD MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_DECOMMIT = 0x4000, MEM_RELEASE = 0x8000,
                MEM_FREE = 0x10000, MEM_PRIVATE = 0x20000;
constexpr DWORD PAGE_NOACCESS = 0x01, PAGE_READONLY = 0x02, PAGE_READWRITE = 0x04, PAGE_EXECUTE = 0x10,
                PAGE_EXECUTE_READ = 0x20, PAGE_EXECUTE_READWRITE = 0x40;

struct Reservation {
  size_t size;
  DWORD protect;
};
std::mutex g_vmMutex;
std::map<uintptr_t, Reservation> g_reservations;  // by base

int HostProtection(DWORD protect) {
  switch (protect & 0xff) {
  case PAGE_NOACCESS: return PROT_NONE;
  case PAGE_READONLY: case PAGE_EXECUTE: case PAGE_EXECUTE_READ: return PROT_READ;
  default: return PROT_READ | PROT_WRITE;
  }
}

std::map<uintptr_t, Reservation>::iterator FindReservation(uintptr_t address) {
  auto it = g_reservations.upper_bound(address);
  if (it == g_reservations.begin()) return g_reservations.end();
  --it;
  return address < it->first + it->second.size ? it : g_reservations.end();
}

uint64_t VirtualAlloc(CallContext& c) {
  const auto address = static_cast<uintptr_t>(c.Arg(0));
  const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t size = (c.Arg(1) + pageSize - 1) & ~(pageSize - 1);
  const auto type = static_cast<DWORD>(c.Arg(2));
  const auto protect = static_cast<DWORD>(c.Arg(3));
  if (size == 0) return SetError(ERROR_INVALID_PARAMETER, 0);
  std::scoped_lock lock {g_vmMutex};
  if (address && !(type & MEM_RESERVE)) {
    // Commit inside an existing reservation.
    if (FindReservation(address) == g_reservations.end()) return SetError(ERROR_INVALID_PARAMETER, 0);
    const uintptr_t start = address & ~(pageSize - 1);
    if (mprotect(reinterpret_cast<void*>(start), size, HostProtection(protect)) != 0) return SetError(ERROR_NOT_ENOUGH_MEMORY, 0);
    return start;
  }
  const int prot = (type & MEM_COMMIT) ? HostProtection(protect) : PROT_NONE;
  void* hint = reinterpret_cast<void*>(address);
  void* p = mmap(hint, size, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) return SetError(ERROR_NOT_ENOUGH_MEMORY, 0);
  if (address && p != hint) {
    munmap(p, size);
    return SetError(ERROR_INVALID_PARAMETER, 0);  // Windows fails when the requested range is taken
  }
  g_reservations[reinterpret_cast<uintptr_t>(p)] = {size, protect};
  if (size >= (64u << 20))  // the big ones, to see who holds the address space
    std::fprintf(stderr, "UB_WIN32 VirtualAlloc(%s) %zu MiB at %p\n", (type & MEM_COMMIT) ? "reserve+commit" : "reserve",
                 size >> 20, p);
  return reinterpret_cast<uint64_t>(p);
}

uint64_t VirtualFree(CallContext& c) {
  const auto address = static_cast<uintptr_t>(c.Arg(0));
  const size_t size = c.Arg(1);
  const auto type = static_cast<DWORD>(c.Arg(2));
  std::scoped_lock lock {g_vmMutex};
  auto it = FindReservation(address);
  if (it == g_reservations.end()) return SetError(ERROR_INVALID_PARAMETER, kFalse);
  if (type & MEM_RELEASE) {
    if (size != 0 || address != it->first) return SetError(ERROR_INVALID_PARAMETER, kFalse);
    munmap(reinterpret_cast<void*>(it->first), it->second.size);
    g_reservations.erase(it);
    return kTrue;
  }
  if (type & MEM_DECOMMIT) {
    const size_t span = size ? size : it->second.size;
    mmap(reinterpret_cast<void*>(address), span, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    return kTrue;
  }
  return SetError(ERROR_INVALID_PARAMETER, kFalse);
}

uint64_t VirtualProtect(CallContext& c) {
  const auto address = static_cast<uintptr_t>(c.Arg(0));
  const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const uintptr_t start = address & ~(pageSize - 1);
  const size_t span = ((address + c.Arg(1) + pageSize - 1) & ~(pageSize - 1)) - start;
  const auto protect = static_cast<DWORD>(c.Arg(2));
  DWORD old = PAGE_READWRITE;
  {
    std::scoped_lock lock {g_vmMutex};
    if (auto it = FindReservation(address); it != g_reservations.end()) old = it->second.protect;
  }
  // Image pages (the loaded DLL) are not in g_reservations; they report PAGE_EXECUTE_READWRITE.
  if (mprotect(reinterpret_cast<void*>(start), span, HostProtection(protect)) != 0) return SetError(ERROR_INVALID_PARAMETER, kFalse);
  Store<DWORD>(c.Arg(3), old);
  return kTrue;
}

uint64_t VirtualQuery(CallContext& c) {
  // MEMORY_BASIC_INFORMATION (48 bytes): BaseAddress, AllocationBase, AllocationProtect(4),
  // PartitionId(2)+pad, RegionSize, State(4), Protect(4), Type(4).
  const auto address = static_cast<uintptr_t>(c.Arg(0));
  auto* info = c.Ptr<uint8_t>(1);
  if (c.Arg(2) < 48 || info == nullptr) return SetError(ERROR_INSUFFICIENT_BUFFER, 0);
  const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  std::memset(info, 0, 48);
  std::scoped_lock lock {g_vmMutex};
  const auto it = FindReservation(address);
  const uintptr_t page = address & ~(pageSize - 1);
  if (it == g_reservations.end()) {
    Store<uint64_t>(reinterpret_cast<uint64_t>(info), page);
    Store<uint64_t>(reinterpret_cast<uint64_t>(info + 24), pageSize);
    Store<DWORD>(reinterpret_cast<uint64_t>(info + 32), MEM_FREE);
    Store<DWORD>(reinterpret_cast<uint64_t>(info + 36), PAGE_NOACCESS);
    return 48;
  }
  Store<uint64_t>(reinterpret_cast<uint64_t>(info), page);
  Store<uint64_t>(reinterpret_cast<uint64_t>(info + 8), it->first);
  Store<DWORD>(reinterpret_cast<uint64_t>(info + 16), it->second.protect);
  Store<uint64_t>(reinterpret_cast<uint64_t>(info + 24), it->first + it->second.size - page);
  Store<DWORD>(reinterpret_cast<uint64_t>(info + 32), MEM_COMMIT);
  Store<DWORD>(reinterpret_cast<uint64_t>(info + 36), it->second.protect);
  Store<DWORD>(reinterpret_cast<uint64_t>(info + 40), MEM_PRIVATE);
  return 48;
}

// ---- TLS and FLS -------------------------------------------------------------------------
// Guest threads run on host threads, so host thread_local storage is per guest thread.

constexpr DWORD TLS_OUT_OF_INDEXES = 0xffffffffu;
constexpr size_t kTlsSlots = 1088;  // TLS_MINIMUM_AVAILABLE + expansion slots on Windows

struct SlotTable {
  std::mutex mutex;
  std::vector<bool> used = std::vector<bool>(kTlsSlots, false);
  DWORD Alloc() {
    std::scoped_lock lock {mutex};
    for (DWORD i = 0; i < used.size(); ++i) if (!used[i]) { used[i] = true; return i; }
    return TLS_OUT_OF_INDEXES;
  }
  bool Free(DWORD i) {
    std::scoped_lock lock {mutex};
    if (i >= used.size() || !used[i]) return false;
    used[i] = false;
    return true;
  }
};
SlotTable g_tls, g_fls;
thread_local uint64_t t_tls[kTlsSlots];
thread_local uint64_t t_fls[kTlsSlots];

uint64_t TlsAlloc(CallContext&) { return g_tls.Alloc(); }
uint64_t TlsFree(CallContext& c) { return g_tls.Free(static_cast<DWORD>(c.Arg(0))) ? kTrue : SetError(ERROR_INVALID_PARAMETER, kFalse); }
uint64_t TlsGetValue(CallContext& c) {
  const auto i = static_cast<DWORD>(c.Arg(0));
  if (i >= kTlsSlots) return SetError(ERROR_INVALID_PARAMETER, 0);
  return SetError(ERROR_SUCCESS, t_tls[i]);
}
uint64_t TlsSetValue(CallContext& c) {
  const auto i = static_cast<DWORD>(c.Arg(0));
  if (i >= kTlsSlots) return SetError(ERROR_INVALID_PARAMETER, kFalse);
  t_tls[i] = c.Arg(1);
  return kTrue;
}
// FLS callbacks run on FlsFree for the calling thread's value (Windows also runs them for every
// thread and at thread exit; threads are not created by the guest yet).
uintptr_t g_flsCallbacks[kTlsSlots];
uint64_t FlsAlloc(CallContext& c) {
  const DWORD i = g_fls.Alloc();
  if (i < kTlsSlots) g_flsCallbacks[i] = c.Arg(0);
  return i;
}
uint64_t FlsFree(CallContext& c) {
  const auto i = static_cast<DWORD>(c.Arg(0));
  if (i >= kTlsSlots) return SetError(ERROR_INVALID_PARAMETER, kFalse);
  const uintptr_t callback = g_flsCallbacks[i];
  const uint64_t value = t_fls[i];
  if (!g_fls.Free(i)) return SetError(ERROR_INVALID_PARAMETER, kFalse);
  g_flsCallbacks[i] = 0;
  t_fls[i] = 0;
  if (callback != 0 && value != 0) CallGuest(callback, {value});
  return kTrue;
}
uint64_t FlsGetValue(CallContext& c) {
  const auto i = static_cast<DWORD>(c.Arg(0));
  if (i >= kTlsSlots) return SetError(ERROR_INVALID_PARAMETER, 0);
  return SetError(ERROR_SUCCESS, t_fls[i]);
}
uint64_t FlsSetValue(CallContext& c) {
  const auto i = static_cast<DWORD>(c.Arg(0));
  if (i >= kTlsSlots) return SetError(ERROR_INVALID_PARAMETER, kFalse);
  t_fls[i] = c.Arg(1);
  return kTrue;
}
// The UCRT's newer raw variant: same slot value, without touching the last-error code.
uint64_t FlsGetValue2(CallContext& c) {
  const auto i = static_cast<DWORD>(c.Arg(0));
  return i < kTlsSlots ? t_fls[i] : 0;
}
// Our ANSI code page is UTF-8 (GetACP), so the *A file APIs behave as ANSI; report that.
uint64_t AreFileApisANSI(CallContext&) { return kTrue; }

// ---- synchronisation ---------------------------------------------------------------------
// The guest's CRITICAL_SECTION / SRWLOCK / CONDITION_VARIABLE memory is only used as a key;
// the host objects live in side tables.

template <typename T>
class SideTable {
public:
  T& Get(uintptr_t key) {
    std::scoped_lock lock {mutex_};
    auto& slot = objects_[key];
    if (!slot) slot = std::make_unique<T>();
    return *slot;
  }
  void Erase(uintptr_t key) {
    std::scoped_lock lock {mutex_};
    objects_.erase(key);
  }
private:
  std::mutex mutex_;
  std::map<uintptr_t, std::unique_ptr<T>> objects_;
};

SideTable<std::recursive_mutex> g_criticalSections;
SideTable<std::shared_mutex> g_srwLocks;
SideTable<std::condition_variable_any> g_conditions;

uint64_t InitializeCriticalSection(CallContext& c) {
  std::memset(c.Ptr<void>(0), 0, 40);  // sizeof(CRITICAL_SECTION) on x64
  g_criticalSections.Get(c.Arg(0));
  return kTrue;  // InitializeCriticalSectionEx/AndSpinCount return BOOL; the void variant ignores it
}
uint64_t DeleteCriticalSection(CallContext& c) { g_criticalSections.Erase(c.Arg(0)); return 0; }
uint64_t EnterCriticalSection(CallContext& c) { g_criticalSections.Get(c.Arg(0)).lock(); return 0; }
uint64_t LeaveCriticalSection(CallContext& c) { g_criticalSections.Get(c.Arg(0)).unlock(); return 0; }
uint64_t TryEnterCriticalSection(CallContext& c) { return g_criticalSections.Get(c.Arg(0)).try_lock() ? kTrue : kFalse; }

uint64_t InitializeSRWLock(CallContext& c) { Store<uint64_t>(c.Arg(0), 0); g_srwLocks.Get(c.Arg(0)); return 0; }
uint64_t AcquireSRWLockExclusive(CallContext& c) { g_srwLocks.Get(c.Arg(0)).lock(); return 0; }
uint64_t ReleaseSRWLockExclusive(CallContext& c) { g_srwLocks.Get(c.Arg(0)).unlock(); return 0; }
uint64_t AcquireSRWLockShared(CallContext& c) { g_srwLocks.Get(c.Arg(0)).lock_shared(); return 0; }
uint64_t ReleaseSRWLockShared(CallContext& c) { g_srwLocks.Get(c.Arg(0)).unlock_shared(); return 0; }

uint64_t SleepConditionVariableSRW(CallContext& c) {
  constexpr DWORD INFINITE = 0xffffffffu, CONDITION_VARIABLE_LOCKMODE_SHARED = 1, ERROR_TIMEOUT = 1460;
  auto& cv = g_conditions.Get(c.Arg(0));
  auto& lock = g_srwLocks.Get(c.Arg(1));
  const auto ms = static_cast<DWORD>(c.Arg(2));
  const bool shared = static_cast<DWORD>(c.Arg(3)) & CONDITION_VARIABLE_LOCKMODE_SHARED;
  bool signalled = true;
  auto wait = [&](auto& guard) {
    if (ms == INFINITE) cv.wait(guard);
    else signalled = cv.wait_for(guard, std::chrono::milliseconds(ms)) == std::cv_status::no_timeout;
  };
  if (shared) {
    std::shared_lock guard {lock, std::adopt_lock};
    wait(guard);
    guard.release();
  } else {
    std::unique_lock guard {lock, std::adopt_lock};
    wait(guard);
    guard.release();
  }
  return signalled ? kTrue : SetError(ERROR_TIMEOUT, kFalse);
}
uint64_t WakeAllConditionVariable(CallContext& c) { g_conditions.Get(c.Arg(0)).notify_all(); return 0; }
uint64_t WakeConditionVariable(CallContext& c) { g_conditions.Get(c.Arg(0)).notify_one(); return 0; }

uint64_t InitializeSListHead(CallContext& c) { std::memset(c.Ptr<void>(0), 0, 16); return 0; }

// ---- process and thread identity ---------------------------------------------------------

uint64_t GetCurrentProcess(CallContext&) { return ~uint64_t {0}; }        // (HANDLE)-1
uint64_t GetCurrentThread(CallContext&) { return ~uint64_t {0} - 1; }     // (HANDLE)-2
uint64_t GetCurrentProcessId(CallContext&) { return static_cast<DWORD>(getpid()); }
uint64_t GetCurrentThreadId(CallContext&) {
  uint64_t id = 0;
#if defined(__APPLE__)
  pthread_threadid_np(nullptr, &id);
#else
  id = static_cast<uint64_t>(gettid());
#endif
  return static_cast<DWORD>(id);
}
uint64_t IsDebuggerPresent(CallContext&) { return kFalse; }

// ---- time --------------------------------------------------------------------------------

uint64_t QueryPerformanceFrequency(CallContext& c) {
  Store<int64_t>(c.Arg(0), 10'000'000);  // 100 ns ticks
  return kTrue;
}
uint64_t QueryPerformanceCounter(CallContext& c) {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  Store<int64_t>(c.Arg(0), ns / 100);
  return kTrue;
}
uint64_t GetSystemTimeAsFileTime(CallContext& c) {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  Store<uint64_t>(c.Arg(0), static_cast<uint64_t>(ns / 100) + 116444736000000000ull);  // since 1601-01-01
  return 0;
}
uint64_t Sleep(CallContext& c) {
  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<DWORD>(c.Arg(0))));
  return 0;
}
uint64_t SleepEx(CallContext& c) { Sleep(c); return 0; }
uint64_t SwitchToThread(CallContext&) { std::this_thread::yield(); return kTrue; }

// ---- system information ------------------------------------------------------------------

uint64_t GetSystemInfo(CallContext& c) {
  // SYSTEM_INFO, 48 bytes. The guest is x86-64 Windows with 4 KiB pages and 64 KiB granularity.
  auto* info = c.Ptr<uint8_t>(0);
  std::memset(info, 0, 48);
  Store<uint16_t>(c.Arg(0), 9);                                       // PROCESSOR_ARCHITECTURE_AMD64
  Store<DWORD>(c.Arg(0) + 4, 4096);                                   // dwPageSize
  Store<uint64_t>(c.Arg(0) + 8, 0x10000);                             // lpMinimumApplicationAddress
  Store<uint64_t>(c.Arg(0) + 16, 0x00007ffffffeffffull);              // lpMaximumApplicationAddress
  const auto cpus = std::max(1u, std::thread::hardware_concurrency());
  Store<uint64_t>(c.Arg(0) + 24, cpus >= 64 ? ~uint64_t {0} : (uint64_t {1} << cpus) - 1);
  Store<DWORD>(c.Arg(0) + 32, cpus);
  Store<DWORD>(c.Arg(0) + 36, 8664);                                  // PROCESSOR_AMD_X8664
  Store<DWORD>(c.Arg(0) + 40, 65536);                                 // dwAllocationGranularity
  Store<uint16_t>(c.Arg(0) + 44, 6);
  return 0;
}

uint64_t IsProcessorFeaturePresent(CallContext& c) {
  switch (static_cast<DWORD>(c.Arg(0))) {
  case 6:   // PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE)
  case 10:  // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
  case 13:  // PF_SSE3_INSTRUCTIONS_AVAILABLE
  case 36:  // PF_SSSE3_INSTRUCTIONS_AVAILABLE
  case 37:  // PF_SSE4_1_INSTRUCTIONS_AVAILABLE
  case 38:  // PF_SSE4_2_INSTRUCTIONS_AVAILABLE
    return kTrue;
  default:  // includes PF_FASTFAIL_AVAILABLE (23): no int 29 fast-fail
    return kFalse;
  }
}

uint64_t EncodePointer(CallContext& c) { return c.Arg(0); }
uint64_t DecodePointer(CallContext& c) { return c.Arg(0); }

// ---- code pages --------------------------------------------------------------------------
// The guest sees UTF-8 as its ANSI code page; that keeps every string lossless.

constexpr DWORD CP_ACP = 0, CP_OEMCP = 1, CP_UTF8 = 65001;
uint64_t GetACP(CallContext&) { return CP_UTF8; }
uint64_t GetOEMCP(CallContext&) { return CP_UTF8; }
uint64_t IsValidCodePage(CallContext& c) {
  const auto cp = static_cast<DWORD>(c.Arg(0));
  return (cp == CP_UTF8 || cp == 1252 || cp == 20127) ? kTrue : kFalse;
}

uint64_t MultiByteToWideChar(CallContext& c) {
  const auto cp = static_cast<DWORD>(c.Arg(0));
  const char* src = c.Ptr<const char>(2);
  const auto srcLen = static_cast<int32_t>(c.Arg(3));
  auto* dst = c.Ptr<char16_t>(4);
  const auto dstLen = static_cast<int32_t>(c.Arg(5));
  if (src == nullptr || srcLen == 0) return SetError(ERROR_INVALID_PARAMETER, 0);
  if (cp != CP_ACP && cp != CP_OEMCP && cp != CP_UTF8 && cp != 1252 && cp != 20127) return SetError(ERROR_INVALID_PARAMETER, 0);
  const std::string_view in = srcLen < 0 ? std::string_view(src, std::strlen(src) + 1) : std::string_view(src, srcLen);
  const auto out = ToUtf16(in);
  if (dstLen == 0) return out.size();
  if (static_cast<size_t>(dstLen) < out.size()) return SetError(ERROR_INSUFFICIENT_BUFFER, 0);
  std::memcpy(dst, out.data(), out.size() * 2);
  return out.size();
}

uint64_t WideCharToMultiByte(CallContext& c) {
  const char16_t* src = c.Ptr<const char16_t>(2);
  const auto srcLen = static_cast<int32_t>(c.Arg(3));
  auto* dst = c.Ptr<char>(4);
  const auto dstLen = static_cast<int32_t>(c.Arg(5));
  if (src == nullptr || srcLen == 0) return SetError(ERROR_INVALID_PARAMETER, 0);
  size_t n = srcLen < 0 ? 0 : static_cast<size_t>(srcLen);
  if (srcLen < 0) { while (src[n] != 0) ++n; ++n; }  // include the terminator
  const auto out = ToUtf8(std::u16string_view(src, n));
  Store<BOOL>(c.Arg(7), kFalse);  // lpUsedDefaultChar
  if (dstLen == 0) return out.size();
  if (static_cast<size_t>(dstLen) < out.size()) return SetError(ERROR_INSUFFICIENT_BUFFER, 0);
  std::memcpy(dst, out.data(), out.size());
  return out.size();
}

// ---- modules -----------------------------------------------------------------------------

std::mutex g_builtinMutex;
uintptr_t g_nextBuiltin = 0x0000'7ffd'0000'0000ull;

// HMODULE for a built-in DLL, registering it on first use. GetProcAddress on it hands out stubs for
// the functions we implement and NULL for the rest, as Windows does for a name a DLL does not export:
// callers probe (il2cpp tries BCryptGenRandomW before BCryptGenRandom), and a stub that only returns 0
// read as success there (no random bytes: all-zero GUIDs, M4d).
uintptr_t BuiltinHandle(std::string_view name) {
  std::scoped_lock lock {g_builtinMutex};
  if (const auto* m = FindModuleByName(name)) return m->base;
  Module m;
  m.name = std::string(name);
  m.path = u"C:\\Windows\\System32\\" + ToUtf16(Lower(name));
  m.base = g_nextBuiltin;
  g_nextBuiltin += 0x10000;
  const std::string dll = Lower(name);
  m.lookup = [dll](std::string_view symbol) -> std::optional<uintptr_t> {
    auto* runtime = Runtime::Current();
    if (runtime == nullptr) return std::nullopt;
    const std::string file = dll.find('.') == std::string::npos ? dll + ".dll" : dll;
    const Function* f = runtime->Lookup(file, symbol);
    if (f == nullptr || f->handler == nullptr) return std::nullopt;
    return runtime->Resolve(file, symbol);
  };
  const auto base = m.base;
  RegisterModule(std::move(m));
  return base;
}

uint64_t ModuleHandleFor(const std::string& name) {
  if (const auto* m = FindModuleByName(name)) return m->base;
  if (IsBuiltinDll(name)) return BuiltinHandle(name);
  return 0;
}

uint64_t GetModuleHandleW(CallContext& c) {
  if (c.Arg(0) == 0) {
    const auto* main = MainModule();
    return main ? main->base : SetError(ERROR_MOD_NOT_FOUND, 0);
  }
  const auto handle = ModuleHandleFor(ToUtf8(ReadWide(c.Ptr<const char16_t>(0))));
  return handle ? handle : SetError(ERROR_MOD_NOT_FOUND, 0);
}

uint64_t GetModuleHandleExW(CallContext& c) {
  constexpr DWORD FROM_ADDRESS = 0x4;
  const auto flags = static_cast<DWORD>(c.Arg(0));
  uint64_t handle = 0;
  if (flags & FROM_ADDRESS) {
    // The module whose image contains the address; only the main module is checked so far.
    if (const auto* main = MainModule()) handle = main->base;
  } else if (c.Arg(1) == 0) {
    if (const auto* main = MainModule()) handle = main->base;
  } else {
    handle = ModuleHandleFor(ToUtf8(ReadWide(c.Ptr<const char16_t>(1))));
  }
  Store<uint64_t>(c.Arg(2), handle);
  return handle ? kTrue : SetError(ERROR_MOD_NOT_FOUND, kFalse);
}

uint64_t LoadLibraryExW(CallContext& c) {
  const auto name = ToUtf8(ReadWide(c.Ptr<const char16_t>(0)));
  const auto handle = ModuleHandleFor(name);
  if (handle == 0) std::fprintf(stderr, "UB_WIN32 LoadLibrary(%s): not available\n", name.c_str());
  return handle ? handle : SetError(ERROR_MOD_NOT_FOUND, 0);
}
uint64_t LoadLibraryW(CallContext& c) { return LoadLibraryExW(c); }
uint64_t FreeLibrary(CallContext&) { return kTrue; }

uint64_t GetProcAddress(CallContext& c) {
  const auto* module = FindModuleByHandle(c.Arg(0));
  if (module == nullptr) return SetError(ERROR_INVALID_HANDLE, 0);
  if (c.Arg(1) < 0x10000) return SetError(ERROR_PROC_NOT_FOUND, 0);  // by ordinal: not supported
  const std::string name(c.Ptr<const char>(1));
  const auto address = module->lookup ? module->lookup(name) : std::nullopt;
  if (!address) std::fprintf(stderr, "UB_WIN32 GetProcAddress: no %s in %s\n", name.c_str(), module->name.c_str());
  return address ? *address : SetError(ERROR_PROC_NOT_FOUND, 0);
}

uint64_t GetModuleFileNameW(CallContext& c) {
  const auto* module = c.Arg(0) ? FindModuleByHandle(c.Arg(0)) : MainModule();
  if (module == nullptr) return SetError(ERROR_MOD_NOT_FOUND, 0);
  auto* out = c.Ptr<char16_t>(1);
  const auto size = static_cast<DWORD>(c.Arg(2));
  if (size == 0) return SetError(ERROR_INSUFFICIENT_BUFFER, 0);
  const size_t n = std::min<size_t>(module->path.size(), size - 1);
  std::memcpy(out, module->path.data(), n * 2);
  out[n] = 0;
  return n < module->path.size() ? SetError(ERROR_INSUFFICIENT_BUFFER, size) : n;
}

uint64_t GetModuleFileNameA(CallContext& c) {
  const auto* module = c.Arg(0) ? FindModuleByHandle(c.Arg(0)) : MainModule();
  if (module == nullptr) return SetError(ERROR_MOD_NOT_FOUND, 0);
  const auto path = ToUtf8(module->path);
  auto* out = c.Ptr<char>(1);
  const auto size = static_cast<DWORD>(c.Arg(2));
  if (size == 0) return SetError(ERROR_INSUFFICIENT_BUFFER, 0);
  const size_t n = std::min<size_t>(path.size(), size - 1);
  std::memcpy(out, path.data(), n);
  out[n] = 0;
  return n < path.size() ? SetError(ERROR_INSUFFICIENT_BUFFER, size) : n;
}

// ---- environment and command line --------------------------------------------------------

uint64_t GetEnvironmentVariableW(CallContext&) { return SetError(ERROR_ENVVAR_NOT_FOUND, 0); }
uint64_t SetEnvironmentVariableW(CallContext&) { return kTrue; }
uint64_t GetEnvironmentStringsW(CallContext&) {
  static const char16_t empty[2] = {0, 0};
  return reinterpret_cast<uint64_t>(empty);
}
uint64_t FreeEnvironmentStringsW(CallContext&) { return kTrue; }
uint64_t GetCommandLineW(CallContext&) {
  static const std::u16string line = u"\"GameAssembly.exe\"";
  return reinterpret_cast<uint64_t>(line.c_str());
}
uint64_t GetCommandLineA(CallContext&) { return reinterpret_cast<uint64_t>("\"GameAssembly.exe\""); }
uint64_t GetStartupInfoW(CallContext& c) {
  std::memset(c.Ptr<void>(0), 0, 104);
  Store<DWORD>(c.Arg(0), 104);  // cb
  return 0;
}

// ---- console / standard handles ----------------------------------------------------------

constexpr uint64_t kStdIn = 0x7ffc'0000'0010ull, kStdOut = 0x7ffc'0000'0011ull, kStdErr = 0x7ffc'0000'0012ull;
uint64_t GetStdHandle(CallContext& c) {
  switch (static_cast<int32_t>(c.Arg(0))) {
  case -10: return kStdIn;
  case -11: return kStdOut;
  case -12: return kStdErr;
  default: return SetError(ERROR_INVALID_HANDLE, ~uint64_t {0});
  }
}
uint64_t GetFileType(CallContext& c) {
  const auto h = c.Arg(0);
  return (h == kStdIn || h == kStdOut || h == kStdErr) ? 2 /* FILE_TYPE_CHAR */ : 1 /* FILE_TYPE_DISK */;
}
uint64_t WriteConsoleW(CallContext& c) {
  const auto text = ToUtf8(std::u16string_view(c.Ptr<const char16_t>(1), static_cast<DWORD>(c.Arg(2))));
  std::fprintf(stderr, "UB_GUEST %s", text.c_str());
  Store<DWORD>(c.Arg(3), static_cast<DWORD>(c.Arg(2)));
  return kTrue;
}
uint64_t GetConsoleMode(CallContext&) { return SetError(ERROR_INVALID_HANDLE, kFalse); }
uint64_t GetConsoleOutputCP(CallContext&) { return CP_UTF8; }

} // namespace

uint64_t ModuleHandle(const std::string& name) { return ModuleHandleFor(name); }

void SetLastErrorValue(uint32_t error) { t_lastError = error; }
uint32_t LastErrorValue() { return t_lastError; }

void RegisterKernel32(std::vector<Function>& out) {
  const std::string k = "kernel32.dll";
#define UB_FN(name) out.push_back({k, #name, &name})
  UB_FN(GetLastError); UB_FN(SetLastError);
  UB_FN(GetProcessHeap); UB_FN(HeapAlloc); UB_FN(HeapFree); UB_FN(HeapReAlloc); UB_FN(HeapSize);
  UB_FN(HeapQueryInformation); UB_FN(GlobalAlloc); UB_FN(GlobalFree);
  UB_FN(VirtualAlloc); UB_FN(VirtualFree); UB_FN(VirtualProtect); UB_FN(VirtualQuery);
  UB_FN(TlsAlloc); UB_FN(TlsFree); UB_FN(TlsGetValue); UB_FN(TlsSetValue);
  UB_FN(FlsAlloc); UB_FN(FlsFree); UB_FN(FlsGetValue); UB_FN(FlsSetValue);
  out.push_back({"kernelbase.dll", "FlsGetValue2", &FlsGetValue2});  // the UCRT imports it from here
  out.push_back({k, "FlsGetValue2", &FlsGetValue2});
  UB_FN(AreFileApisANSI);
  UB_FN(InitializeCriticalSection); UB_FN(DeleteCriticalSection); UB_FN(EnterCriticalSection);
  UB_FN(LeaveCriticalSection); UB_FN(TryEnterCriticalSection);
  out.push_back({k, "InitializeCriticalSectionEx", &InitializeCriticalSection});
  out.push_back({k, "InitializeCriticalSectionAndSpinCount", &InitializeCriticalSection});
  UB_FN(InitializeSRWLock); UB_FN(AcquireSRWLockExclusive); UB_FN(ReleaseSRWLockExclusive);
  UB_FN(AcquireSRWLockShared); UB_FN(ReleaseSRWLockShared);
  UB_FN(SleepConditionVariableSRW); UB_FN(WakeAllConditionVariable); UB_FN(WakeConditionVariable);
  UB_FN(InitializeSListHead);
  UB_FN(GetCurrentProcess); UB_FN(GetCurrentThread); UB_FN(GetCurrentProcessId); UB_FN(GetCurrentThreadId);
  UB_FN(IsDebuggerPresent);
  UB_FN(QueryPerformanceFrequency); UB_FN(QueryPerformanceCounter); UB_FN(GetSystemTimeAsFileTime);
  UB_FN(Sleep); UB_FN(SleepEx); UB_FN(SwitchToThread);
  UB_FN(GetSystemInfo); out.push_back({k, "GetNativeSystemInfo", &GetSystemInfo});
  UB_FN(IsProcessorFeaturePresent); UB_FN(EncodePointer); UB_FN(DecodePointer);
  UB_FN(GetACP); UB_FN(GetOEMCP); UB_FN(IsValidCodePage); UB_FN(MultiByteToWideChar); UB_FN(WideCharToMultiByte);
  UB_FN(GetModuleHandleW); UB_FN(GetModuleHandleExW); UB_FN(LoadLibraryExW); UB_FN(LoadLibraryW);
  UB_FN(FreeLibrary); UB_FN(GetProcAddress); UB_FN(GetModuleFileNameW); UB_FN(GetModuleFileNameA);
  UB_FN(GetEnvironmentVariableW); UB_FN(SetEnvironmentVariableW); UB_FN(GetEnvironmentStringsW);
  UB_FN(FreeEnvironmentStringsW); UB_FN(GetCommandLineW); UB_FN(GetCommandLineA); UB_FN(GetStartupInfoW);
  UB_FN(GetStdHandle); UB_FN(GetFileType); UB_FN(WriteConsoleW); UB_FN(GetConsoleMode); UB_FN(GetConsoleOutputCP);
#undef UB_FN
}

} // namespace UnityBridge::Win32
