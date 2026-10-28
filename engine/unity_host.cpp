// SPDX-License-Identifier: GPL-3.0-or-later
// What Husk's Android runtime (M3d, app/Sources/unity_loader.c) needs from the engine side:
//
//  - executable memory for the ARM64 Android libraries it loads (libmain.so, libunity.so). Husk
//    looks for a StikDebug region by dlsym(RTLD_DEFAULT, "husk_ios_jit_get_mapping"), which in Husk
//    itself the QEMU library provides. Here it is one more BreakpointJIT region from the same
//    allocator FEXCore uses (DualMappedRegion), requested on first use and kept for the process.
//    Husk's loader retargets code at the writable view with adrp, so the two views must lie within
//    +-3 GiB of each other; the allocator places its writable alias anywhere (on the first device
//    run 8 GiB away), so the alias is moved into the nearest free gap of the address space here.
//    The app requests the region at launch when Unity's libraries are present, before il2cpp
//    fragments the address space (on that run nothing below the region was free any more).
//  - the virtual libil2cpp.so: dlsym on it is Il2cppBridge::Symbol, the il2cpp_* functions of the
//    game's GameAssembly.dll under FEXCore.
//  - hardware-fault reports for the thread that runs Android code (MachFault).
#include "core/ios/ios_jit_allocator.h"
#include "engine/il2cpp_bridge.h"
#include "engine/il2cpp_intercept.h"
#include "engine/mach_fault.h"
#include "win32/hle.h"

#include <mach/mach.h>
#include <mach/vm_map.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace {

// Husk's tl_dual_mapping (husk-tl-internal.h): rw, rx, size.
struct HuskDualMapping {
  uint8_t* rw_addr;
  uint8_t* rx_addr;
  size_t size;
};

constexpr size_t kHuskRegionBytes = 128u << 20;  // libunity.so is ~25 MB of code; stubs and libmain on top

constexpr uint64_t kAdrpReach = 3ull << 30;  // Husk's limit for |rw - rx|
constexpr uint64_t kPage = 16384;

struct Gap {
  uint64_t begin, end;
};

// Free ranges of this task's address space between `lo` and `hi`, from vm_region's map entries.
std::vector<Gap> FreeGaps(uint64_t lo, uint64_t hi) {
  std::vector<Gap> gaps;
  uint64_t cursor = lo;
  vm_address_t address = static_cast<vm_address_t>(lo);
  while (address < hi) {
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t info {};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const kern_return_t kr = vm_region_64(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                                          reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (kr != KERN_SUCCESS) break;  // nothing mapped above: the rest is free
    if (address > cursor) gaps.push_back({cursor, std::min<uint64_t>(address, hi)});
    cursor = std::max<uint64_t>(cursor, address + size);
    address += size;
  }
  if (cursor < hi) gaps.push_back({cursor, hi});
  return gaps;
}

// A writable alias of [rx, rx+size) within kAdrpReach of rx, or 0. Tries the full size, then shorter
// ones (the region is used from its start, so a shorter alias is a shorter region); *size is updated.
uint8_t* NearAlias(uint8_t* rx, size_t* size) {
  const uint64_t x = reinterpret_cast<uint64_t>(rx);
  const uint64_t lo = x > kAdrpReach ? x - kAdrpReach : 0, hi = x + kAdrpReach;
  const auto gaps = FreeGaps(lo, hi);
  std::fprintf(stderr, "UB_UNITY %zu free gaps within 3 GiB of rx %p:", gaps.size(), static_cast<void*>(rx));
  for (const auto& g : gaps) {
    if (g.end - g.begin >= (16u << 20)) std::fprintf(stderr, " %#llx+%lluM", static_cast<unsigned long long>(g.begin),
                                                     static_cast<unsigned long long>((g.end - g.begin) >> 20));
  }
  std::fprintf(stderr, "\n");
  for (size_t want = *size; want >= (48u << 20); want -= (16u << 20)) {
    // The gap whose usable start is nearest to rx (both views must fit the reach entirely).
    uint64_t best = 0, bestDistance = ~0ull;
    for (const auto& g : gaps) {
      uint64_t begin = (g.begin + kPage - 1) & ~(kPage - 1);
      if (begin < (1ull << 32)) begin = 1ull << 32;  // __PAGEZERO
      if (begin >= g.end || g.end - begin < want) continue;
      // Within the gap, the placement closest to rx: its top end below rx, or its start above.
      const uint64_t candidate = g.end <= x ? ((g.end - want) & ~(kPage - 1)) : begin;
      if (candidate < begin) continue;
      const uint64_t distance = candidate > x ? candidate - x : x - candidate;
      if (distance + want > kAdrpReach) continue;
      if (distance < bestDistance) { best = candidate; bestDistance = distance; }
    }
    if (best == 0) continue;
    vm_address_t rw = static_cast<vm_address_t>(best);
    vm_prot_t cur = VM_PROT_NONE, max = VM_PROT_NONE;
    kern_return_t kr = vm_remap(mach_task_self(), &rw, want, 0, VM_FLAGS_FIXED, mach_task_self(),
                                reinterpret_cast<vm_address_t>(rx), FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
      std::fprintf(stderr, "UB_UNITY remap at %#llx (%zu MiB) failed: %d\n", static_cast<unsigned long long>(best),
                   want >> 20, kr);
      continue;
    }
    kr = vm_protect(mach_task_self(), rw, want, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    if (kr != KERN_SUCCESS) {
      std::fprintf(stderr, "UB_UNITY vm_protect(RW) of the near alias failed: %d\n", kr);
      vm_deallocate(mach_task_self(), rw, want);
      continue;
    }
    *size = want;
    return reinterpret_cast<uint8_t*>(rw);
  }
  return nullptr;
}

std::once_flag g_regionOnce;
Core::DualMappedRegion* g_region = nullptr;  // never released: the libraries run for the process
HuskDualMapping g_mapping {};

} // namespace

extern "C" __attribute__((visibility("default"), used)) HuskDualMapping* husk_ios_jit_get_mapping() {
  std::call_once(g_regionOnce, [] {
    const auto start = std::chrono::steady_clock::now();
    auto* region = new Core::DualMappedRegion(Core::DualMappedRegion::Allocate(kHuskRegionBytes));
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (!region->IsValid()) {
      std::fprintf(stderr, "UB_UNITY no JIT region for Husk (%zu MiB requested, %lld ms)\n", kHuskRegionBytes >> 20,
                   static_cast<long long>(ms));
      delete region;
      return;
    }
    g_region = region;
    g_mapping = {region->rw_addr, region->rx_addr, region->size};
    const int64_t delta = reinterpret_cast<int64_t>(region->rw_addr) - reinterpret_cast<int64_t>(region->rx_addr);
    if (delta > static_cast<int64_t>(kAdrpReach) || delta < -static_cast<int64_t>(kAdrpReach)) {
      size_t size = region->size;
      if (uint8_t* rw = NearAlias(region->rx_addr, &size)) {
        std::fprintf(stderr, "UB_UNITY writable view moved from %p (%lld MiB away) to %p (%lld MiB), %zu MiB\n",
                     static_cast<void*>(region->rw_addr), static_cast<long long>(delta >> 20), static_cast<void*>(rw),
                     static_cast<long long>((reinterpret_cast<int64_t>(rw) - reinterpret_cast<int64_t>(region->rx_addr)) >> 20),
                     size >> 20);
        g_mapping = {rw, region->rx_addr, size};
      } else {
        std::fprintf(stderr, "UB_UNITY no free gap for the writable view within 3 GiB of the executable one\n");
      }
    }
    std::fprintf(stderr, "UB_UNITY JIT region for Husk: rx=%p rw=%p %zu MiB (%lld ms)\n",
                 static_cast<void*>(g_mapping.rx_addr), static_cast<void*>(g_mapping.rw_addr), g_mapping.size >> 20,
                 static_cast<long long>(ms));
  });
  return g_region ? &g_mapping : nullptr;
}

// dlsym(libil2cpp.so, name) for libunity: the bridge to GameAssembly.dll.
extern "C" void* ub_il2cpp_symbol(const char* name) {
  return name ? UnityBridge::Il2cppBridge::Symbol(name) : nullptr;
}

extern "C" void ub_unity_thread_enter() { UnityBridge::MachFault::RegisterThisThread(); }
extern "C" void ub_unity_thread_leave() { UnityBridge::MachFault::UnregisterThisThread(); }

// What the engine did with il2cpp (M3e): InternalCalls registered through the intercept, and the
// Windows imports GameAssembly called that the mini Win32 does not implement.
extern "C" void ub_il2cpp_engine_report(char* out, size_t capacity) {
  const auto st = UnityBridge::Il2cppIntercept::GetStats();
  int n = std::snprintf(out, capacity,
                        "InternalCalls: %zu registered, %zu absent from the metadata, %zu not bridgeable, %zu pending; "
                        "%zu callbacks wrapped",
                        st.registered, st.absent, st.failed, st.pending, st.callbacks);
  if (auto* rt = UnityBridge::Win32::Runtime::Current()) {
    const auto calls = rt->UnimplementedCalls();
    if (n > 0 && static_cast<size_t>(n) < capacity)
      n += std::snprintf(out + n, capacity - n, "; unimplemented imports called: %zu", calls.size());
    for (const auto& c : calls) {
      if (n <= 0 || static_cast<size_t>(n) >= capacity) break;
      n += std::snprintf(out + n, capacity - n, " %s", c.c_str());
    }
  }
}
