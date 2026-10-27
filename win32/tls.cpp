// SPDX-License-Identifier: GPL-3.0-or-later
// Implicit TLS for loaded PE modules and the x64 thread environment block.
//
// Windows gives each thread its own copy of a module's __declspec(thread) data. The module's TLS
// directory holds a template (initialised bytes followed by zero-fill), the address of a DWORD that
// receives the module's TLS index, and a list of TLS callbacks. At thread start the loader copies
// the template into fresh memory, stores the pointer in the thread's ThreadLocalStoragePointer
// array at the module's index, and runs the callbacks. Guest code then reads a thread variable as
//   mov rax, gs:[0x58]      ; ThreadLocalStoragePointer
//   mov rcx, [rax + 8*index]; this module's block
//   mov eax, [rcx + offset] ; the variable
// so the whole mechanism rests on TEB+0x58 pointing at that array.
#include "hle.h"
#include "loader/pe_image.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace UnityBridge::Win32 {
namespace {

struct TlsModule {
  uintptr_t imageBase {};
  uintptr_t rawDataStart {}, rawDataEnd {};
  uint32_t zeroFill {};
  uint32_t index {};
  std::vector<uintptr_t> callbacks;
};

std::mutex g_tlsMutex;
std::vector<TlsModule> g_modules;  // one per module with a TLS directory; index is its position

} // namespace

void RegisterTlsImage(uintptr_t imageBase, const Pe::Tls& tls) {
  std::scoped_lock lock {g_tlsMutex};
  TlsModule module;
  module.imageBase = imageBase;
  module.rawDataStart = tls.rawDataStart;
  module.rawDataEnd = tls.rawDataEnd;
  module.zeroFill = tls.zeroFill;
  module.callbacks = tls.callbacks;
  module.index = static_cast<uint32_t>(g_modules.size());
  if (tls.indexAddress) {
    const uint32_t value = module.index;
    std::memcpy(reinterpret_cast<void*>(tls.indexAddress), &value, sizeof(value));
  }
  g_modules.push_back(std::move(module));
}

uintptr_t AllocThreadTls() {
  std::scoped_lock lock {g_tlsMutex};
  if (g_modules.empty()) return 0;
  // ThreadLocalStoragePointer: one entry per module (indices are dense, assigned in order).
  auto* array = new uintptr_t[g_modules.size()] {};
  for (const auto& module : g_modules) {
    const size_t rawSize = module.rawDataEnd - module.rawDataStart;
    const size_t blockSize = rawSize + module.zeroFill;
    auto* block = new uint8_t[blockSize ? blockSize : 1] {};
    if (rawSize) std::memcpy(block, reinterpret_cast<const void*>(module.rawDataStart), rawSize);
    array[module.index] = reinterpret_cast<uintptr_t>(block);
  }
  return reinterpret_cast<uintptr_t>(array);
}

void FreeThreadTls(uintptr_t tlsPointer) {
  if (tlsPointer == 0) return;
  std::scoped_lock lock {g_tlsMutex};
  auto* array = reinterpret_cast<uintptr_t*>(tlsPointer);
  for (size_t i = 0; i < g_modules.size(); ++i) delete[] reinterpret_cast<uint8_t*>(array[i]);
  delete[] array;
}

std::vector<std::pair<uintptr_t, uintptr_t>> TlsCallbacks() {
  std::vector<std::pair<uintptr_t, uintptr_t>> toCall;  // (callback, imageBase)
  std::scoped_lock lock {g_tlsMutex};
  for (const auto& module : g_modules)
    for (const auto callback : module.callbacks) toCall.emplace_back(callback, module.imageBase);
  return toCall;
}

void RunTlsCallbacks(DllReason reason) {
  for (const auto& [callback, imageBase] : TlsCallbacks()) CallGuest(callback, {imageBase, reason, 0});
}

// x64 TEB offsets (NT_TIB first): StackBase 0x08, StackLimit 0x10, Self 0x30, ThreadLocalStorage-
// Pointer 0x58, ProcessEnvironmentBlock 0x60, ClientId (pid 0x40, tid 0x48).
void InitTeb(void* teb, uintptr_t stackBase, uintptr_t stackLimit, uintptr_t self, uintptr_t peb,
             uint32_t pid, uint32_t tid, uintptr_t tlsPointer) {
  auto* t = static_cast<uint8_t*>(teb);
  std::memset(t, 0, kTebSize);
  auto put = [&](size_t offset, uint64_t value) { std::memcpy(t + offset, &value, sizeof(value)); };
  put(0x08, stackBase);
  put(0x10, stackLimit);
  put(0x30, self);
  put(0x40, pid);
  put(0x48, tid);
  put(kTebTlsPointerOffset, tlsPointer);
  put(0x60, peb);
}

} // namespace UnityBridge::Win32
