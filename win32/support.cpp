// SPDX-License-Identifier: GPL-3.0-or-later
// Module registry and UTF-16 helpers shared by the win32/*.cpp function sets.
#include "hle.h"

#include <mutex>

namespace UnityBridge::Win32 {
namespace {

std::mutex g_modulesMutex;
std::vector<Module>& Modules() {
  static std::vector<Module> modules;
  return modules;
}
size_t g_main = SIZE_MAX;

// The DLLs whose functions UnityBridge implements itself; LoadLibrary of these succeeds.
constexpr std::string_view kBuiltins[] = {
    "kernel32.dll", "kernelbase.dll", "ntdll.dll", "user32.dll", "advapi32.dll", "ole32.dll", "oleaut32.dll",
    "shell32.dll", "ws2_32.dll", "iphlpapi.dll", "dbghelp.dll", "bcrypt.dll", "baselib.dll",
};

std::string Normalize(std::string_view name) {
  auto lower = Lower(name);
  if (const auto slash = lower.find_last_of("/\\"); slash != std::string::npos) lower.erase(0, slash + 1);
  if (lower.find('.') == std::string::npos) lower += ".dll";
  return lower;
}

} // namespace

bool IsBuiltinDll(std::string_view name) {
  const auto n = Normalize(name);
  for (auto b : kBuiltins) if (n == b) return true;
  return false;
}

void RegisterModule(Module module, bool isMainModule) {
  std::scoped_lock lock {g_modulesMutex};
  module.name = Normalize(module.name);
  Modules().push_back(std::move(module));
  if (isMainModule) g_main = Modules().size() - 1;
}

const Module* FindModuleByName(std::string_view name) {
  const auto n = Normalize(name);
  std::scoped_lock lock {g_modulesMutex};
  for (const auto& m : Modules()) if (m.name == n) return &m;
  return nullptr;
}

const Module* FindModuleByHandle(uintptr_t handle) {
  std::scoped_lock lock {g_modulesMutex};
  for (const auto& m : Modules()) if (m.base == handle) return &m;
  return nullptr;
}

const Module* MainModule() {
  std::scoped_lock lock {g_modulesMutex};
  return g_main < Modules().size() ? &Modules()[g_main] : nullptr;
}

std::u16string ReadWide(const char16_t* text) {
  if (text == nullptr) return {};
  std::u16string out;
  for (size_t i = 0; text[i] != 0 && i < (1u << 20); ++i) out.push_back(text[i]);
  return out;
}

std::string ToUtf8(std::u16string_view text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    uint32_t c = text[i];
    if (c >= 0xd800 && c < 0xdc00 && i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] < 0xe000) {
      c = 0x10000 + ((c - 0xd800) << 10) + (text[++i] - 0xdc00);
    } else if (c >= 0xd800 && c < 0xe000) {
      c = 0xfffd;  // lone surrogate
    }
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      out.push_back(static_cast<char>(0xc0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else if (c < 0x10000) {
      out.push_back(static_cast<char>(0xe0 | (c >> 12)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else {
      out.push_back(static_cast<char>(0xf0 | (c >> 18)));
      out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    }
  }
  return out;
}

std::u16string ToUtf16(std::string_view text) {
  std::u16string out;
  for (size_t i = 0; i < text.size();) {
    const auto b = static_cast<unsigned char>(text[i]);
    uint32_t c;
    size_t len;
    if (b < 0x80) { c = b; len = 1; }
    else if ((b >> 5) == 0x6) { c = b & 0x1f; len = 2; }
    else if ((b >> 4) == 0xe) { c = b & 0x0f; len = 3; }
    else if ((b >> 3) == 0x1e) { c = b & 0x07; len = 4; }
    else { out.push_back(0xfffd); ++i; continue; }
    if (i + len > text.size()) { out.push_back(0xfffd); break; }
    bool ok = true;
    for (size_t k = 1; k < len; ++k) {
      const auto cont = static_cast<unsigned char>(text[i + k]);
      if ((cont >> 6) != 0x2) { ok = false; break; }
      c = (c << 6) | (cont & 0x3f);
    }
    if (!ok) { out.push_back(0xfffd); ++i; continue; }
    i += len;
    if (c >= 0x10000) {
      c -= 0x10000;
      out.push_back(static_cast<char16_t>(0xd800 + (c >> 10)));
      out.push_back(static_cast<char16_t>(0xdc00 + (c & 0x3ff)));
    } else {
      out.push_back(static_cast<char16_t>(c));
    }
  }
  return out;
}

} // namespace UnityBridge::Win32
