// SPDX-License-Identifier: GPL-3.0-or-later
#include "hle.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace UnityBridge::Win32 {
namespace {

constexpr size_t kStubSize = 32;

// win32/host_entry_x64.S fills CallContext by these offsets.
static_assert(offsetof(CallContext, id) == 0);
static_assert(offsetof(CallContext, gpr) == 8);
static_assert(offsetof(CallContext, xmm) == 40);
static_assert(offsetof(CallContext, rsp) == 104);
static_assert(offsetof(CallContext, ret) == 112);
static_assert(offsetof(CallContext, xmmRet) == 120);
static_assert(offsetof(CallContext, returnsFloat) == 136);
static_assert(offsetof(CallContext, x86) == 144);
static_assert(offsetof(CallContext, xmmFull) == 272);
static_assert(offsetof(CallContext, resumeGpr) == 528);
static_assert(offsetof(CallContext, resumeXmm) == 656);
static_assert(offsetof(CallContext, rip) == 912);
static_assert(offsetof(CallContext, resumeRip) == 920);
static_assert(offsetof(CallContext, haveState) == 928);
static_assert(offsetof(CallContext, resume) == 929);
std::mutex g_mutex;
Runtime* g_current = nullptr;

} // namespace

uint64_t CallContext::Arg(unsigned index) const {
  if (index < 4) return gpr[index];
  // Above the return address: 32 bytes of shadow space, then arguments 5, 6, ...
  uint64_t value;
  std::memcpy(&value, reinterpret_cast<const void*>(rsp + 8 + 32 + (index - 4) * 8), 8);
  return value;
}

double CallContext::ArgDouble(unsigned index) const {
  double value;
  if (index < 4) {
    std::memcpy(&value, xmm[index], 8);
  } else {
    const uint64_t bits = Arg(index);
    std::memcpy(&value, &bits, 8);
  }
  return value;
}

void CallContext::ReturnDouble(double value) {
  std::memcpy(xmmRet, &value, 8);
  xmmRet[1] = 0;
  returnsFloat = true;
}

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::span<const Function> Functions() {
  static const std::vector<Function> table = [] {
    std::vector<Function> out;
    RegisterKernel32(out);
    RegisterOtherDlls(out);
    RegisterBaselib(out);
    RegisterKernel32Sync(out);
    RegisterKernel32Files(out);
    RegisterKernel32Misc(out);
    RegisterNtdllSeh(out);
    return out;
  }();
  return table;
}

Runtime::Runtime(StubKind kind, std::span<uint8_t> stubMemory, uintptr_t hostEntry)
    : kind_ {kind}, stubs_ {stubMemory}, hostEntry_ {hostEntry} {
  std::memset(stubs_.data(), 0xcc, stubs_.size());  // int3 between stubs
}

Runtime* Runtime::Current() { return g_current; }

namespace {
GuestCaller g_guestCaller = nullptr;
}
void SetGuestCaller(GuestCaller caller) { g_guestCaller = caller; }
uint64_t CallGuest(uintptr_t function, std::initializer_list<uint64_t> arguments) {
  if (g_guestCaller == nullptr || function == 0) {
    std::fprintf(stderr, "UB_WIN32 no guest call possible to %#lx\n", static_cast<unsigned long>(function));
    return 0;
  }
  return g_guestCaller(function, {arguments.begin(), arguments.size()});
}
void Runtime::SetCurrent(Runtime* runtime) { g_current = runtime; }

const Function* Runtime::FunctionForStub(uintptr_t address) const {
  const auto base = reinterpret_cast<uintptr_t>(stubs_.data());
  if (address < base || address >= base + stubs_.size()) return nullptr;
  const size_t id = (address - base) / kStubSize;
  std::scoped_lock lock {g_mutex};
  return id < bound_.size() ? bound_[id].function : nullptr;
}

std::pair<std::string_view, std::string_view> Runtime::StubName(uint32_t id) const {
  std::scoped_lock lock {g_mutex};
  if (id < bound_.size()) return {bound_[id].dll, bound_[id].name};
  return {};
}

std::optional<uintptr_t> Runtime::BindCallback(Handler handler, std::string name) {
  std::scoped_lock lock {g_mutex};
  const auto id = static_cast<uint32_t>(bound_.size());
  if ((size_t {id} + 1) * kStubSize > stubs_.size()) return std::nullopt;
  Function& fn = callbacks_.emplace_back(Function {"callback", name, handler});
  bound_.push_back({id, &fn, "callback", std::move(name)});
  return EmitStub(id);
}

const Function* Runtime::Lookup(std::string_view dll, std::string_view name) const {
  const auto lowerDll = Lower(dll);
  for (const auto& f : Functions()) {
    if (f.dll == lowerDll && f.name == name) return &f;
  }
  return nullptr;
}

const Function* Runtime::LookupAnyDll(std::string_view name) const {
  for (const auto& f : Functions()) {
    if (f.name == name && f.handler) return &f;
  }
  return nullptr;
}

uintptr_t Runtime::EmitStub(uint32_t id) {
  uint8_t* p = stubs_.data() + size_t {id} * kStubSize;
  const uint32_t op = Operation(id);
  size_t n = 0;
  auto put = [&](std::initializer_list<uint8_t> bytes) { for (auto b : bytes) p[n++] = b; };
  auto put32 = [&](uint32_t v) { std::memcpy(p + n, &v, 4); n += 4; };
  auto put64 = [&](uint64_t v) { std::memcpy(p + n, &v, 8); n += 8; };
  put({0x4c, 0x8b, 0xd1});  // mov r10, rcx  (syscall overwrites rcx; the bridge reads r10)
  put({0xb8});              // mov eax, op
  put32(op);
  if (kind_ == StubKind::Bridge) {
    put({0x0f, 0x05});      // syscall
    put({0xc3});            // ret
  } else {
    put({0x49, 0xbb});      // movabs r11, hostEntry
    put64(hostEntry_);
    put({0x41, 0xff, 0xe3}); // jmp r11
  }
  return reinterpret_cast<uintptr_t>(p);
}

namespace {
std::mutex g_dataMutex;
std::map<std::string, uintptr_t> g_dataExports;  // "dll!name" (dll lower case)
} // namespace

void RegisterDataExport(std::string_view dll, std::string_view name, uintptr_t address) {
  std::scoped_lock lock {g_dataMutex};
  g_dataExports[Lower(dll) + "!" + std::string(name)] = address;
}

std::optional<uintptr_t> DataExport(std::string_view dll, std::string_view name) {
  (void)Functions();  // the Register* functions add their data exports when the table is built
  std::scoped_lock lock {g_dataMutex};
  const auto it = g_dataExports.find(Lower(dll) + "!" + std::string(name));
  if (it == g_dataExports.end()) return std::nullopt;
  return it->second;
}

std::optional<uintptr_t> Runtime::Resolve(std::string_view dll, std::string_view name) {
  if (const auto data = DataExport(dll, name)) return data;
  std::scoped_lock lock {g_mutex};
  const auto lowerDll = Lower(dll);
  for (const auto& stub : bound_) {
    if (stub.dll == lowerDll && stub.name == name) {
      return reinterpret_cast<uintptr_t>(stubs_.data() + size_t {stub.id} * kStubSize);
    }
  }
  const auto id = static_cast<uint32_t>(bound_.size());
  if ((size_t {id} + 1) * kStubSize > stubs_.size()) return std::nullopt;
  bound_.push_back({id, Lookup(dll, name), lowerDll, std::string(name)});
  return EmitStub(id);
}

void Runtime::Dispatch(CallContext& ctx) {
  const Stub* stub = nullptr;
  {
    std::scoped_lock lock {g_mutex};
    if (ctx.id < bound_.size()) stub = &bound_[ctx.id];
  }
  if (stub == nullptr) {
    std::fprintf(stderr, "UB_WIN32 call through unknown stub %u\n", ctx.id);
    ctx.ret = 0;
    return;
  }
  if (stub->function == nullptr || stub->function->handler == nullptr) {
    std::fprintf(stderr, "UB_WIN32 unimplemented %s!%s\n", stub->dll.c_str(), stub->name.c_str());
    std::scoped_lock lock {g_mutex};
    unimplemented_.push_back(stub->dll + "!" + stub->name);
    ctx.ret = 0;
    return;
  }
  ctx.ret = stub->function->handler(ctx);
}

std::vector<std::string> Runtime::UnimplementedCalls() const {
  std::scoped_lock lock {g_mutex};
  return unimplemented_;
}

} // namespace UnityBridge::Win32

// Entry for HostNative stubs (win32/host_entry_x64.S): the assembly saves the Windows argument
// registers into `frame` and calls here with the System V convention.
extern "C" void ub_win32_host_dispatch(UnityBridge::Win32::CallContext* ctx) {
  ctx->id = static_cast<uint32_t>(ctx->id & 0xffffu);
  if (auto* runtime = UnityBridge::Win32::Runtime::Current()) runtime->Dispatch(*ctx);
}
