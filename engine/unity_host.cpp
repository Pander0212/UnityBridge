// SPDX-License-Identifier: GPL-3.0-or-later
// What Husk's Android runtime (M3d, app/Sources/unity_loader.c) needs from the engine side:
//
//  - executable memory for the ARM64 Android libraries it loads (libmain.so, libunity.so). Husk
//    looks for a StikDebug region by dlsym(RTLD_DEFAULT, "husk_ios_jit_get_mapping"), which in Husk
//    itself the QEMU library provides. Here it is one more BreakpointJIT region from the same
//    allocator FEXCore uses (DualMappedRegion), requested on first use and kept for the process.
//  - the virtual libil2cpp.so: dlsym on it is Il2cppBridge::Symbol, the il2cpp_* functions of the
//    game's GameAssembly.dll under FEXCore.
//  - hardware-fault reports for the thread that runs Android code (MachFault).
#include "core/ios/ios_jit_allocator.h"
#include "engine/il2cpp_bridge.h"
#include "engine/mach_fault.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace {

// Husk's tl_dual_mapping (husk-tl-internal.h): rw, rx, size.
struct HuskDualMapping {
  uint8_t* rw_addr;
  uint8_t* rx_addr;
  size_t size;
};

constexpr size_t kHuskRegionBytes = 128u << 20;  // libunity.so is ~25 MB of code; stubs and libmain on top

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
    std::fprintf(stderr, "UB_UNITY JIT region for Husk: rx=%p rw=%p %zu MiB (%lld ms)\n",
                 static_cast<void*>(region->rx_addr), static_cast<void*>(region->rw_addr), region->size >> 20,
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
