// SPDX-License-Identifier: GPL-3.0-or-later
// Replacements for the il2cpp_* functions that take host function pointers: see il2cpp_intercept.h.
//
// Every il2cpp API function reached from here is a native function pointer (on the device an
// Il2cppBridge thunk running the guest export). Results that are bool or 32-bit in il2cpp are
// declared as full 64-bit words and masked: an MSVC-compiled callee leaves the upper bits of a
// narrow result undefined.
#include "engine/il2cpp_intercept.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace UnityBridge::Il2cppIntercept {
namespace {

using IcallBridge::Kind;
using IcallBridge::Signature;
using P = void*;
using Word = uint64_t;

// Il2CppTypeEnum (il2cpp-blob.h, the ECMA-335 element types).
enum : unsigned {
  kVoid = 0x01, kBoolean = 0x02, kChar = 0x03, kI1 = 0x04, kU1 = 0x05, kI2 = 0x06, kU2 = 0x07,
  kI4 = 0x08, kU4 = 0x09, kI8 = 0x0a, kU8 = 0x0b, kR4 = 0x0c, kR8 = 0x0d, kString = 0x0e,
  kPtr = 0x0f, kValueType = 0x11, kClass = 0x12, kVar = 0x13, kArray = 0x14, kGenericInst = 0x15,
  kI = 0x18, kU = 0x19, kFnPtr = 0x1b, kObject = 0x1c, kSzArray = 0x1d, kMVar = 0x1e,
};
constexpr uint32_t kMethodStatic = 0x0010;        // METHOD_ATTRIBUTE_STATIC
constexpr uint32_t kMethodInternalCall = 0x1000;  // METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL

// The metadata API the lookups need.
struct Api {
  P (*domain_get)();
  P* (*domain_get_assemblies)(P domain, size_t* size);
  P (*assembly_get_image)(P assembly);
  P (*class_from_name)(P image, const char* ns, const char* name);
  P (*class_get_nested_types)(P klass, P* iter);
  const char* (*class_get_name)(P klass);
  P (*class_get_methods)(P klass, P* iter);
  const char* (*method_get_name)(P method);
  Word (*method_get_flags)(P method, uint32_t* iflags);
  Word (*method_get_param_count)(P method);
  P (*method_get_param)(P method, Word index);
  P (*method_get_return_type)(P method);
  Word (*type_get_type)(P type);
  Word (*type_is_byref)(P type);
  P (*class_from_type)(P type);
  Word (*class_is_enum)(P klass);
  Word (*class_is_valuetype)(P klass);
  P (*class_enum_basetype)(P klass);
};

constexpr std::string_view kIntercepted[] = {
    "il2cpp_add_internal_call",
    "il2cpp_init",
    "il2cpp_init_utf16",
    "il2cpp_set_memory_callbacks",
    "il2cpp_register_log_callback",
    "il2cpp_set_find_plugin_callback",
    "il2cpp_gc_foreach_heap",
    "il2cpp_gchandle_foreach_get_target",
    "il2cpp_gc_set_external_allocation_tracker",
    "il2cpp_gc_set_external_wbarrier_tracker",
};

// Il2CppMemoryCallbacks (il2cpp-api-types.h): seven allocator functions, all integer/pointer.
struct MemoryCallbacks {
  P malloc_func, aligned_malloc_func, free_func, aligned_free_func, calloc_func, realloc_func,
      aligned_realloc_func;
};
constexpr uint8_t kMemoryCallbackArgs[7] = {1, 2, 1, 1, 2, 2, 3};

std::mutex g_mutex;  // guards everything below; never held while guest code runs il2cpp_init
Resolve g_api, g_original;
bool g_runtimeUp = false;
std::optional<Api> g_metadata;           // bound on first lookup
std::vector<P> g_images;                 // the domain's images, read once the runtime is up
std::vector<std::pair<std::string, P>> g_pending;  // InternalCalls registered before il2cpp_init
std::map<std::pair<uintptr_t, std::string>, uintptr_t> g_wrapped;  // (fn, signature) -> stub
MemoryCallbacks g_memoryCallbacks {};    // what il2cpp gets: stays valid for the process
Stats g_stats;

template <typename F> F Original(std::string_view name) { return reinterpret_cast<F>(g_original ? g_original(name) : nullptr); }

// ---- signatures from the metadata --------------------------------------------------------------

bool BindMetadata(std::string* error) {
  if (g_metadata) return true;
  Api a {};
  bool ok = true;
  auto get = [&](auto& field, const char* name) {
    field = reinterpret_cast<std::remove_reference_t<decltype(field)>>(g_api ? g_api(name) : nullptr);
    if (field == nullptr) {
      if (error) *error = std::string("metadata API missing: ") + name;
      ok = false;
    }
  };
  get(a.domain_get, "il2cpp_domain_get");
  get(a.domain_get_assemblies, "il2cpp_domain_get_assemblies");
  get(a.assembly_get_image, "il2cpp_assembly_get_image");
  get(a.class_from_name, "il2cpp_class_from_name");
  get(a.class_get_nested_types, "il2cpp_class_get_nested_types");
  get(a.class_get_name, "il2cpp_class_get_name");
  get(a.class_get_methods, "il2cpp_class_get_methods");
  get(a.method_get_name, "il2cpp_method_get_name");
  get(a.method_get_flags, "il2cpp_method_get_flags");
  get(a.method_get_param_count, "il2cpp_method_get_param_count");
  get(a.method_get_param, "il2cpp_method_get_param");
  get(a.method_get_return_type, "il2cpp_method_get_return_type");
  get(a.type_get_type, "il2cpp_type_get_type");
  get(a.type_is_byref, "il2cpp_type_is_byref");
  get(a.class_from_type, "il2cpp_class_from_type");
  get(a.class_is_enum, "il2cpp_class_is_enum");
  get(a.class_is_valuetype, "il2cpp_class_is_valuetype");
  get(a.class_enum_basetype, "il2cpp_class_enum_basetype");
  if (!ok) return false;
  g_metadata = a;
  P domain = a.domain_get();
  size_t count = 0;
  P* assemblies = domain ? a.domain_get_assemblies(domain, &count) : nullptr;
  for (size_t i = 0; assemblies && i < count; ++i) {
    if (P image = a.assembly_get_image(assemblies[i])) g_images.push_back(image);
  }
  return true;
}

struct ParsedName {
  std::string ns, outer;             // the outermost type
  std::vector<std::string> nested;   // "Outer/Inner/..." below it
  std::string method;
  std::optional<size_t> paramCount;  // from a "(Type,Type)" list, if given
};

std::optional<ParsedName> Parse(std::string_view name) {
  const size_t sep = name.find("::");
  if (sep == std::string_view::npos || sep == 0) return std::nullopt;
  ParsedName out;
  std::string_view type = name.substr(0, sep), rest = name.substr(sep + 2);
  if (const size_t open = rest.find('('); open != std::string_view::npos) {
    const std::string_view list = rest.substr(open + 1, rest.rfind(')') == std::string_view::npos
                                                            ? std::string_view::npos
                                                            : rest.rfind(')') - open - 1);
    rest = rest.substr(0, open);
    size_t count = 0;
    int depth = 0;
    bool any = false;
    for (char c : list) {
      if (c == '<' || c == '[' || c == '(') ++depth;
      else if (c == '>' || c == ']' || c == ')') --depth;
      else if (c == ',' && depth == 0) ++count;
      if (c != ' ') any = true;
    }
    out.paramCount = any ? count + 1 : 0;
  }
  if (rest.empty()) return std::nullopt;
  out.method = std::string(rest);
  size_t slash = type.find('/');
  const std::string_view outer = type.substr(0, slash);
  while (slash != std::string_view::npos) {
    const size_t next = type.find('/', slash + 1);
    out.nested.emplace_back(type.substr(slash + 1, next == std::string_view::npos ? next : next - slash - 1));
    slash = next;
  }
  if (const size_t dot = outer.rfind('.'); dot != std::string_view::npos) {
    out.ns = std::string(outer.substr(0, dot));
    out.outer = std::string(outer.substr(dot + 1));
  } else {
    out.outer = std::string(outer);
  }
  return out;
}

P FindClass(const Api& a, const ParsedName& n) {
  P klass = nullptr;
  for (P image : g_images) {
    if ((klass = a.class_from_name(image, n.ns.c_str(), n.outer.c_str()))) break;
  }
  for (const auto& inner : n.nested) {
    if (klass == nullptr) return nullptr;
    P found = nullptr, iter = nullptr;
    while (P k = a.class_get_nested_types(klass, &iter)) {
      if (inner == a.class_get_name(k)) {
        found = k;
        break;
      }
    }
    klass = found;
  }
  return klass;
}

std::optional<Kind> Classify(const Api& a, P type, bool isReturn, std::string* error, int depth = 0) {
  if (type == nullptr) {
    *error = "no type";
    return std::nullopt;
  }
  if (a.type_is_byref(type) & 0xff) return Kind::Int;
  const unsigned t = static_cast<unsigned>(a.type_get_type(type) & 0xff);
  switch (t) {
  case kVoid:
    if (isReturn) return Kind::Int;
    break;
  case kBoolean:
  case kU1: return Kind::UInt8;
  case kI1: return Kind::Int8;
  case kChar:
  case kU2: return Kind::UInt16;
  case kI2: return Kind::Int16;
  case kR4:
  case kR8: return Kind::Float;
  case kI4: case kU4: case kI8: case kU8: case kString: case kPtr: case kClass: case kArray:
  case kI: case kU: case kFnPtr: case kObject: case kSzArray:
    return Kind::Int;
  case kValueType: {
    P klass = a.class_from_type(type);
    if (klass && (a.class_is_enum(klass) & 0xff) && depth < 2) {
      return Classify(a, a.class_enum_basetype(klass), isReturn, error, depth + 1);
    }
    *error = std::string("value type ") + (klass ? a.class_get_name(klass) : "?") + " by value";
    return std::nullopt;
  }
  case kGenericInst: {
    P klass = a.class_from_type(type);
    if (klass && !(a.class_is_valuetype(klass) & 0xff)) return Kind::Int;
    *error = "generic value type by value";
    return std::nullopt;
  }
  case kVar:
  case kMVar:
    *error = "generic parameter";
    return std::nullopt;
  default:
    break;
  }
  *error = "type " + std::to_string(t) + (isReturn ? " as return" : " as argument");
  return std::nullopt;
}

std::optional<Signature> SignatureOf(const Api& a, P method, std::string* error) {
  uint32_t iflags = 0;
  const uint32_t flags = static_cast<uint32_t>(a.method_get_flags(method, &iflags));
  const unsigned params = static_cast<unsigned>(a.method_get_param_count(method) & 0xffffffffu);
  Signature sig;
  const unsigned total = params + ((flags & kMethodStatic) ? 0 : 1);
  if (total > IcallBridge::kMaxArgs) {
    *error = std::to_string(total) + " arguments";
    return std::nullopt;
  }
  if (!(flags & kMethodStatic)) sig.args[sig.argCount++] = Kind::Int;  // this
  for (unsigned i = 0; i < params; ++i) {
    const auto k = Classify(a, a.method_get_param(method, i), false, error);
    if (!k) {
      *error = "parameter " + std::to_string(i) + ": " + *error;
      return std::nullopt;
    }
    sig.args[sig.argCount++] = *k;
  }
  const auto r = Classify(a, a.method_get_return_type(method), true, error);
  if (!r) {
    *error = "return: " + *error;
    return std::nullopt;
  }
  sig.ret = *r;
  if (!IcallBridge::FitsRegisters(sig)) {
    *error = "needs AAPCS64 stack arguments (more than 8 of one register kind)";
    return std::nullopt;
  }
  return sig;
}

bool SameSignature(const Signature& x, const Signature& y) {
  if (x.argCount != y.argCount || x.ret != y.ret) return false;
  for (unsigned i = 0; i < x.argCount; ++i) {
    if (x.args[i] != y.args[i]) return false;
  }
  return true;
}

std::string Encode(const Signature& s) {
  std::string out(1, static_cast<char>('a' + static_cast<int>(s.ret)));
  for (unsigned i = 0; i < s.argCount; ++i) out += static_cast<char>('a' + static_cast<int>(s.args[i]));
  return out;
}

// Caller holds g_mutex. `absent` is set when the name is simply not in the metadata.
std::optional<Signature> DeriveLocked(std::string_view name, std::string* error, bool* absent) {
  *absent = false;
  if (!g_runtimeUp) {
    *error = "il2cpp_init has not run yet";
    return std::nullopt;
  }
  if (!BindMetadata(error)) return std::nullopt;
  const Api& a = *g_metadata;
  const auto parsed = Parse(name);
  if (!parsed) {
    *error = "not of the form Namespace.Class::Method";
    return std::nullopt;
  }
  P klass = FindClass(a, *parsed);
  if (klass == nullptr) {
    *error = "class not in the metadata";
    *absent = true;
    return std::nullopt;
  }
  // Every InternalCall overload of that name (with the parameter count, if the name gives one).
  std::optional<Signature> chosen;
  std::string firstError;
  size_t candidates = 0;
  P iter = nullptr;
  while (P m = a.class_get_methods(klass, &iter)) {
    if (parsed->method != a.method_get_name(m)) continue;
    uint32_t iflags = 0;
    a.method_get_flags(m, &iflags);
    if (!(iflags & kMethodInternalCall)) continue;
    if (parsed->paramCount && (a.method_get_param_count(m) & 0xffffffffu) != *parsed->paramCount) continue;
    ++candidates;
    std::string why;
    const auto sig = SignatureOf(a, m, &why);
    if (!sig) {
      if (firstError.empty()) firstError = why;
      continue;
    }
    if (!chosen) chosen = sig;
    else if (!SameSignature(*chosen, *sig)) {
      std::fprintf(stderr, "UB_IL2CPP_ICALL %.*s: overloads differ in their ABI; using the first\n",
                   static_cast<int>(name.size()), name.data());
    }
  }
  if (chosen) return chosen;
  if (candidates == 0) {
    *error = "no InternalCall method of that name in the metadata";
    *absent = true;
  } else {
    *error = firstError;
  }
  return std::nullopt;
}

// A reverse thunk for `fn`, the same stub for the same function and signature. Caller holds g_mutex.
uintptr_t WrapLocked(P fn, const Signature& sig) {
  if (fn == nullptr) return 0;
  const auto key = std::make_pair(reinterpret_cast<uintptr_t>(fn), Encode(sig));
  if (const auto it = g_wrapped.find(key); it != g_wrapped.end()) return it->second;
  const auto stub = IcallBridge::Make(reinterpret_cast<uintptr_t>(fn), sig);
  if (!stub) return 0;
  g_wrapped.emplace(key, *stub);
  return *stub;
}

Signature IntSignature(unsigned args) {
  Signature s;
  s.argCount = static_cast<uint8_t>(args);
  return s;  // all Int, Int return (or void)
}

// Wraps one host callback with an all-integer signature and counts it.
uintptr_t WrapCallback(P fn, unsigned args) {
  std::scoped_lock lock {g_mutex};
  const uintptr_t stub = WrapLocked(fn, IntSignature(args));
  if (stub) ++g_stats.callbacks;
  if (fn && !stub) std::fprintf(stderr, "UB_IL2CPP_INTERCEPT no thunk for callback %p\n", fn);
  return stub;
}

// Registers one InternalCall with the real il2cpp. Caller holds g_mutex.
void RegisterLocked(const std::string& name, P fn) {
  std::string error;
  bool absent = false;
  const auto sig = DeriveLocked(name, &error, &absent);
  if (!sig) {
    if (absent) ++g_stats.absent;
    else {
      ++g_stats.failed;
      std::fprintf(stderr, "UB_IL2CPP_ICALL cannot bridge %s: %s\n", name.c_str(), error.c_str());
    }
    return;
  }
  const uintptr_t stub = WrapLocked(fn, *sig);
  auto* add = Original<void (*)(const char*, uintptr_t)>("il2cpp_add_internal_call");
  if ((fn && !stub) || add == nullptr) {
    ++g_stats.failed;
    std::fprintf(stderr, "UB_IL2CPP_ICALL cannot bridge %s: %s\n", name.c_str(),
                 add ? "no thunk left" : "il2cpp_add_internal_call not exported");
    return;
  }
  add(name.c_str(), stub);
  ++g_stats.registered;
}

void FlushPendingLocked() {
  auto pending = std::move(g_pending);
  g_pending.clear();
  for (const auto& [name, fn] : pending) RegisterLocked(name, fn);
  g_stats.pending = 0;
}

// ---- the replacements (native functions libunity calls) ----------------------------------------

void AddInternalCall(const char* name, P fn) {
  if (name == nullptr) return;
  std::scoped_lock lock {g_mutex};
  if (!g_runtimeUp) {
    g_pending.emplace_back(name, fn);
    g_stats.pending = g_pending.size();
    return;
  }
  RegisterLocked(name, fn);
}

template <size_t Which>  // 0: il2cpp_init, 1: il2cpp_init_utf16 (both take one pointer, return int)
Word InitRuntime(P domainName) {
  auto* init = Original<Word (*)(P)>(Which == 0 ? "il2cpp_init" : "il2cpp_init_utf16");
  if (init == nullptr) return 0;
  const Word result = init(domainName);  // runs guest code for a long time: no lock held
  if (result & 0xffffffffu) {
    std::scoped_lock lock {g_mutex};
    g_runtimeUp = true;
    FlushPendingLocked();
  }
  return result;
}

void SetMemoryCallbacks(const MemoryCallbacks* callbacks) {
  auto* set = Original<void (*)(const MemoryCallbacks*)>("il2cpp_set_memory_callbacks");
  if (set == nullptr) return;
  if (callbacks == nullptr) return set(nullptr);
  P in[7], out[7];
  static_assert(sizeof(in) == sizeof(MemoryCallbacks));
  std::memcpy(in, callbacks, sizeof(in));
  for (size_t i = 0; i < 7; ++i) out[i] = reinterpret_cast<P>(WrapCallback(in[i], kMemoryCallbackArgs[i]));
  {
    std::scoped_lock lock {g_mutex};
    std::memcpy(&g_memoryCallbacks, out, sizeof(out));
  }
  set(&g_memoryCallbacks);
}

void RegisterLogCallback(P callback) {  // void (*)(const char* message)
  if (auto* set = Original<void (*)(uintptr_t)>("il2cpp_register_log_callback")) set(WrapCallback(callback, 1));
}

void SetFindPluginCallback(P callback) {  // const char* (*)(const char* name)
  if (auto* set = Original<void (*)(uintptr_t)>("il2cpp_set_find_plugin_callback")) set(WrapCallback(callback, 1));
}

// The two heap walks call back with (data, userData) in the public declaration and (userData,
// start, end) in the Boehm section walk underneath; three integer slots cover both, and an
// unused slot is harmless.
void GcForeachHeap(P func, P userData) {
  if (auto* walk = Original<void (*)(uintptr_t, P)>("il2cpp_gc_foreach_heap")) walk(WrapCallback(func, 3), userData);
}

void GchandleForeachGetTarget(P func, P userData) {
  if (auto* walk = Original<void (*)(uintptr_t, P)>("il2cpp_gchandle_foreach_get_target"))
    walk(WrapCallback(func, 3), userData);
}

void GcSetExternalAllocationTracker(P func) {  // void (*)(void* ptr, size_t size, int kind)
  if (auto* set = Original<void (*)(uintptr_t)>("il2cpp_gc_set_external_allocation_tracker"))
    set(WrapCallback(func, 3));
}

void GcSetExternalWbarrierTracker(P func) {  // void (*)(void** ptr)
  if (auto* set = Original<void (*)(uintptr_t)>("il2cpp_gc_set_external_wbarrier_tracker"))
    set(WrapCallback(func, 1));
}

} // namespace

std::span<const std::string_view> Intercepted() { return kIntercepted; }

void Init(Resolve api, Resolve original, bool runtimeUp) {
  std::scoped_lock lock {g_mutex};
  g_api = std::move(api);
  g_original = std::move(original);
  g_runtimeUp = runtimeUp;
  g_metadata.reset();
  g_images.clear();
  g_pending.clear();
  g_wrapped.clear();
  g_stats = {};
}

void* Replacement(std::string_view name) {
  if (name == "il2cpp_add_internal_call") return reinterpret_cast<void*>(&AddInternalCall);
  if (name == "il2cpp_init") return reinterpret_cast<void*>(&InitRuntime<0>);
  if (name == "il2cpp_init_utf16") return reinterpret_cast<void*>(&InitRuntime<1>);
  if (name == "il2cpp_set_memory_callbacks") return reinterpret_cast<void*>(&SetMemoryCallbacks);
  if (name == "il2cpp_register_log_callback") return reinterpret_cast<void*>(&RegisterLogCallback);
  if (name == "il2cpp_set_find_plugin_callback") return reinterpret_cast<void*>(&SetFindPluginCallback);
  if (name == "il2cpp_gc_foreach_heap") return reinterpret_cast<void*>(&GcForeachHeap);
  if (name == "il2cpp_gchandle_foreach_get_target") return reinterpret_cast<void*>(&GchandleForeachGetTarget);
  if (name == "il2cpp_gc_set_external_allocation_tracker") return reinterpret_cast<void*>(&GcSetExternalAllocationTracker);
  if (name == "il2cpp_gc_set_external_wbarrier_tracker") return reinterpret_cast<void*>(&GcSetExternalWbarrierTracker);
  return nullptr;
}

std::optional<Signature> DeriveSignature(std::string_view name, std::string* error) {
  std::scoped_lock lock {g_mutex};
  std::string local;
  bool absent = false;
  return DeriveLocked(name, error ? error : &local, &absent);
}

Stats GetStats() {
  std::scoped_lock lock {g_mutex};
  return g_stats;
}

} // namespace UnityBridge::Il2cppIntercept
