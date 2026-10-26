// SPDX-License-Identifier: GPL-3.0-or-later
// Host test (x86-64 Linux only): loads tests/testdll/ubtest.dll with the PE loader, binds its
// imports to HostNative stubs of the mini Win32, and calls its exports natively with the
// Microsoft x64 convention. Checks the loader, the stubs, argument passing (registers, stack,
// xmm) and the first KERNEL32 functions. On iPhone the same code runs under FEXCore instead.
#include "loader/pe_image.h"
#include "win32/hle.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <sys/mman.h>
#include <vector>

extern "C" void ub_win32_host_entry();

namespace {

std::vector<uint8_t> ReadFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

int g_failures = 0;
void Check(bool ok, const char* what) {
  std::printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++g_failures;
}

} // namespace

int main(int argc, char** argv) {
  using namespace UnityBridge;
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s ubtest.dll\n", argv[0]);
    return 2;
  }
  const auto file = ReadFile(argv[1]);
  Check(!file.empty(), "read test DLL");

  void* stubMemory = mmap(nullptr, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
  Win32::Runtime runtime(Win32::StubKind::HostNative, {static_cast<uint8_t*>(stubMemory), 1 << 16},
                         reinterpret_cast<uintptr_t>(&ub_win32_host_entry));
  Win32::Runtime::SetCurrent(&runtime);

  // Occupy the DLL's preferred base so the loader must relocate (as it will for GameAssembly.dll
  // whenever its range is taken).
  const auto inspected = Pe::Inspect(file);
  void* blocker = inspected.image
      ? mmap(reinterpret_cast<void*>(inspected.image->preferredBase), 1 << 16, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0)
      : MAP_FAILED;
  auto loaded = Pe::Load(file, [&](const Pe::Import& imp) { return runtime.Resolve(imp.dll, imp.Symbol()); }, true);
  Check(blocker != MAP_FAILED && loaded.image &&
            reinterpret_cast<uintptr_t>(loaded.image->base) != inspected.image->preferredBase,
        "loaded away from its preferred base (relocated)");
  Check(loaded.image.has_value(), "load and relocate");
  if (!loaded.image) {
    std::printf("error: %s\n", loaded.error.c_str());
    return 1;
  }
  auto& image = *loaded.image;
  Check(loaded.unresolved.empty(), "every import bound to a stub");
  Check(image.imports.size() == 50, "50 imports");

  Win32::Module module;
  module.name = "ubtest.dll";
  module.path = u"C:\\UnityBridge\\ubtest.dll";
  module.base = reinterpret_cast<uintptr_t>(image.base);
  module.lookup = [&image](std::string_view name) { return image.Export(name); };
  Win32::RegisterModule(std::move(module), true);

  using AllFn = int (__attribute__((ms_abi)) *)();
  using FloatFn = double (__attribute__((ms_abi)) *)(double, double);
  using MissingFn = uint32_t (__attribute__((ms_abi)) *)();
  const auto all = image.Export("ub_test_all");
  const auto fl = image.Export("ub_test_float");
  const auto missing = image.Export("ub_test_missing");
  Check(all && fl && missing, "exports found");
  if (!all || !fl || !missing) return 1;

  const int bits = reinterpret_cast<AllFn>(*all)();
  const char* names[] = {"SetLastError/GetLastError", "HeapAlloc zeroed", "HeapFree", "TlsAlloc/Set/Get",
                         "critical section (recursive)", "MultiByteToWideChar (6 args)",
                         "WideCharToMultiByte (8 args)", "QueryPerformanceCounter/Frequency",
                         "GetModuleHandleW + GetProcAddress", "VirtualAlloc/VirtualFree",
                         "callee-saved registers across calls", "DIR64 relocation applied",
                         "BSTR (OLEAUT32 by ordinal)", "SAFEARRAY 2-D bounds and data",
                         "CryptGenRandom", "SHGetFolderPathW", "baselib semaphore (C++ names)",
                         "events, mutex, semaphore, waits"};
  for (int i = 0; i < 18; ++i) Check(bits & (1 << i), names[i]);

  Check(reinterpret_cast<FloatFn>(*fl)(3.0, 4.0) == 12.5, "double arguments and xmm0 return");

  Check(reinterpret_cast<MissingFn>(*missing)() == 0, "unimplemented import returns 0");
  const auto calls = runtime.UnimplementedCalls();
  Check(calls.size() == 1 && calls[0] == "kernel32.dll!UnityBridgeTestMissingFunction", "unimplemented call reported");

  Pe::Unload(image);
  std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
