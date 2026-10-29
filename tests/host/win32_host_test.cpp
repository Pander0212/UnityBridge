// SPDX-License-Identifier: GPL-3.0-or-later
// Host test (x86-64 Linux only): loads tests/testdll/ubtest.dll with the PE loader, binds its
// imports to HostNative stubs of the mini Win32, and calls its exports natively with the
// Microsoft x64 convention. Checks the loader, the stubs, argument passing (registers, stack,
// xmm) and the first KERNEL32 functions. On iPhone the same code runs under FEXCore instead.
#include "loader/pe_image.h"
#include "win32/hle.h"

#include <asm/prctl.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
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

// Points gs at a fresh TEB for the calling host thread so native guest code finds its TLS at
// gs:[0x58] exactly as it would under FEXCore. x86-64 Linux keeps its own TLS in fs, so gs is free.
// The caller runs the TLS callbacks, then releases the block with FreeGuestTeb once no more guest
// code runs on the thread.
struct GuestTeb {
  uint8_t* teb = nullptr;
  uint8_t* peb = nullptr;
  uintptr_t tls = 0;
};

GuestTeb SetupGuestTeb() {
  uintptr_t stackBase = 0, stackLimit = 0;
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* base = nullptr;
    size_t size = 0;
    if (pthread_attr_getstack(&attr, &base, &size) == 0) {
      stackLimit = reinterpret_cast<uintptr_t>(base);
      stackBase = stackLimit + size;
    }
    pthread_attr_destroy(&attr);
  }
  GuestTeb out;
  out.teb = new uint8_t[UnityBridge::Win32::kTebSize];
  out.peb = new uint8_t[4096] {};
  out.tls = UnityBridge::Win32::AllocThreadTls();
  uint64_t tid = static_cast<uint64_t>(syscall(SYS_gettid));
  UnityBridge::Win32::InitTeb(out.teb, stackBase, stackLimit, reinterpret_cast<uintptr_t>(out.teb),
                              reinterpret_cast<uintptr_t>(out.peb), static_cast<uint32_t>(getpid()),
                              static_cast<uint32_t>(tid), out.tls);
  syscall(SYS_arch_prctl, ARCH_SET_GS, reinterpret_cast<uintptr_t>(out.teb));
  return out;
}

void FreeGuestTeb(const GuestTeb& teb) {
  UnityBridge::Win32::FreeThreadTls(teb.tls);
  delete[] teb.teb;
  delete[] teb.peb;
}

// Guest code is native x86-64 here: call it with the Microsoft convention directly.
uint64_t CallGuestNative(uintptr_t function, std::span<const uint64_t> arguments) {
  uint64_t a[6] {};
  for (size_t i = 0; i < arguments.size() && i < 6; ++i) a[i] = arguments[i];
  using Fn = uint64_t (__attribute__((ms_abi)) *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
  return reinterpret_cast<Fn>(function)(a[0], a[1], a[2], a[3], a[4], a[5]);
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
  Win32::SetGuestCaller(&CallGuestNative);
  Win32::SetThreadRunner([](uintptr_t start, uint64_t parameter) {
    // Each guest thread gets its own TEB/TLS and runs the TLS callbacks with DLL_THREAD_ATTACH,
    // the way the Windows loader starts a thread, before the start routine.
    const GuestTeb teb = SetupGuestTeb();
    Win32::RunTlsCallbacks(Win32::DLL_THREAD_ATTACH);
    const uint64_t code = CallGuestNative(start, {&parameter, 1});
    Win32::RunTlsCallbacks(Win32::DLL_THREAD_DETACH);
    FreeGuestTeb(teb);
    return code;
  });

  // The guest's C:\Users\Player lands in a fresh temporary folder.
  char profile[] = "/tmp/ubtest-profile-XXXXXX";
  Check(mkdtemp(profile) != nullptr, "temporary profile folder");
  Win32::SetProfileRoot(profile);

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
  Check(image.imports.size() == 93, "93 imports");

  Win32::Module module;
  module.name = "ubtest.dll";
  module.path = u"C:\\UnityBridge\\ubtest.dll";
  module.base = reinterpret_cast<uintptr_t>(image.base);
  module.lookup = [&image](std::string_view name) { return image.Export(name); };
  Win32::RegisterModule(std::move(module), true);

  // Implicit TLS: register the DLL's TLS directory, give this thread a TEB with gs:[0x58] pointing
  // at its TLS block, and run the TLS callbacks with DLL_PROCESS_ATTACH, as the loader does.
  if (image.tls) Win32::RegisterTlsImage(reinterpret_cast<uintptr_t>(image.base), *image.tls);
  // Register the DLL's .pdata so RtlLookupFunctionEntry / the exception dispatcher can unwind it.
  Win32::RegisterUnwindImage(reinterpret_cast<uintptr_t>(image.base),
                             reinterpret_cast<uintptr_t>(image.base) + image.size,
                             reinterpret_cast<uintptr_t>(image.base) + image.pdataRva, image.pdataSize);
  const GuestTeb mainTeb = SetupGuestTeb();
  Win32::RunTlsCallbacks(Win32::DLL_PROCESS_ATTACH);

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
                         "events, mutex, semaphore, waits",
                         "files, find, mapping (profile folder)",
                         "locale: case mapping, compare, en-US", "InterlockedFlushSList",
                         "callback: EnumSystemLocalesW", "callback: FlsFree (nested import)",
                         "CreateThread: own TLS and last error", "CreateThread suspended + ResumeThread",
                         "implicit TLS: __declspec(thread) + callback",
                         "SEH: __try/__except catches RaiseException",
                         "SEH: __finally runs while unwinding to __except",
                         "SEH: RtlCaptureContext/LookupFunctionEntry/VirtualUnwind/PcToFileHeader",
                         "SuspendThread/GetThreadContext, DuplicateHandle(GetCurrentThread)"};
  for (int i = 0; i < 30; ++i) Check(bits & (1 << i), names[i]);

  Check(reinterpret_cast<FloatFn>(*fl)(3.0, 4.0) == 12.5, "double arguments and xmm0 return");

  Check(reinterpret_cast<MissingFn>(*missing)() == 0, "unimplemented import returns 0");
  const auto calls = runtime.UnimplementedCalls();
  Check(calls.size() == 1 && calls[0] == "kernel32.dll!UnityBridgeTestMissingFunction", "unimplemented call reported");

  Win32::RunTlsCallbacks(Win32::DLL_PROCESS_DETACH);
  FreeGuestTeb(mainTeb);
  Pe::Unload(image);
  std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
