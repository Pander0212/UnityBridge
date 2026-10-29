// SPDX-License-Identifier: GPL-3.0-or-later
// Host test (x86-64 Linux only) for engine/il2cpp_intercept: the replacements for the il2cpp_*
// functions that take host function pointers. A fake il2cpp metadata API (classes, nested types,
// methods with flags, parameter and return types, enums, generic instances) stands in for
// GameAssembly's; native System V functions stand in for libunity's engine functions. Registers
// them through the intercepted il2cpp_add_internal_call -- before and after il2cpp_init -- and then
// calls the guest stubs the fake "real" il2cpp received the way GameAssembly would: as Microsoft x64
// functions, with junk in the upper bits of narrow arguments. Also the other host callbacks
// (memory, log, heap walk) and the signature derivation on its own.
#include "engine/icall_bridge.h"
#include "engine/il2cpp_intercept.h"
#include "win32/hle.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <sys/mman.h>
#include <vector>

extern "C" void ub_win32_host_entry();

namespace {

namespace Intercept = UnityBridge::Il2cppIntercept;
using UnityBridge::IcallBridge::Kind;
using P = void*;
using Word = uint64_t;
#define MS __attribute__((ms_abi))

int g_failures = 0;
void Check(bool ok, const char* what) {
  std::printf("%-60s %s\n", what, ok ? "ok" : "FAIL");
  std::fflush(stdout);
  if (!ok) ++g_failures;
}

// ---- a fake il2cpp metadata -------------------------------------------------------------------

struct FakeClass;
struct FakeType {
  unsigned type;
  bool byref = false;
  FakeClass* klass = nullptr;
};
struct FakeMethod {
  std::string name;
  uint32_t flags, iflags;
  std::vector<FakeType*> params;
  FakeType* ret;
};
struct FakeClass {
  std::string ns, name;
  bool isEnum = false, isValueType = false;
  FakeType* enumBase = nullptr;
  std::vector<FakeMethod*> methods;
  std::vector<FakeClass*> nested;
};
struct FakeImage {
  std::vector<FakeClass*> classes;
};

constexpr uint32_t kStatic = 0x10, kIcall = 0x1000;
FakeType tVoid {0x01}, tBool {0x02}, tI2 {0x06}, tU2 {0x07}, tI4 {0x08}, tI8 {0x0a}, tR4 {0x0c}, tR8 {0x0d},
    tString {0x0e}, tObject {0x1c}, tI4Ref {0x08, true}, tVar {0x13};
FakeClass cVector3 {"UnityEngine", "Vector3", false, true, nullptr, {}, {}};
FakeType tVector3 {0x11, false, &cVector3};
FakeClass cMode {"UnityEngine", "Mode", true, true, &tI2, {}, {}};
FakeType tMode {0x11, false, &cMode};
FakeClass cNullableInt {"System", "Nullable`1", false, true, nullptr, {}, {}};
FakeType tNullableInt {0x15, false, &cNullableInt};
FakeClass cList {"System.Collections.Generic", "List`1", false, false, nullptr, {}, {}};
FakeType tList {0x15, false, &cList};

FakeMethod mClosest {"ClosestPowerOfTwo", kStatic, kIcall, {&tI4}, &tI4};
FakeMethod mHalf {"HalfToFloat", kStatic, kIcall, {&tU2}, &tR4};
FakeMethod mMix {"Mix", kStatic, kIcall, {&tI4, &tR4, &tBool, &tR8}, &tR8};
FakeMethod mLerp {"Lerp", kStatic, 0, {&tR4, &tR4, &tR4}, &tR4};  // managed, not an InternalCall
FakeMethod mPick1 {"Pick", kStatic, kIcall, {&tI4}, &tI4};
FakeMethod mPick2 {"Pick", kStatic, kIcall, {&tI4, &tI8}, &tI8};
FakeMethod mByValue {"Normalize", kStatic, kIcall, {&tVector3}, &tVoid};
FakeMethod mByRef {"Normalize_Injected", kStatic, kIcall, {&tI4Ref, &tI4Ref}, &tVoid};
FakeMethod mNine {"Nine", kStatic, kIcall, {&tI4, &tI4, &tI4, &tI4, &tI4, &tI4, &tI4, &tI4, &tI4}, &tVoid};
FakeMethod mNullable {"Maybe", kStatic, kIcall, {&tNullableInt}, &tVoid};
FakeMethod mListArg {"Fill", kStatic, kIcall, {&tList}, &tI4};
FakeMethod mGenericArg {"Any", kStatic, kIcall, {&tVar}, &tVoid};
FakeClass cMathf {"UnityEngine", "Mathf", false, false, nullptr,
                  {&mClosest, &mHalf, &mMix, &mLerp, &mPick1, &mPick2, &mByValue, &mByRef, &mNine, &mNullable,
                   &mListArg, &mGenericArg}, {}};
FakeMethod mActive {"get_activeSelf", 0, kIcall, {}, &tBool};
FakeClass cGameObject {"UnityEngine", "GameObject", false, false, nullptr, {&mActive}, {}};
FakeMethod mTwice {"Twice", kStatic, kIcall, {&tMode}, &tI2};
FakeClass cInner {"", "Inner", false, false, nullptr, {&mTwice}, {}};
FakeClass cOuter {"UnityEngine.Rendering", "Outer", false, false, nullptr, {}, {&cInner}};
FakeMethod mPing {"Ping", kStatic, kIcall, {&tString}, &tObject};
FakeClass cThing {"Ns", "Thing", false, false, nullptr, {&mPing}, {}};

FakeImage g_core {{&cMathf, &cGameObject, &cOuter}};
FakeImage g_other {{&cThing}};
P g_assemblies[2] = {&g_core, &g_other};  // an "assembly" here is its image
int g_domain;

// Results that are bool/int/uint32 in il2cpp come back with junk above them, as from MSVC code.
constexpr Word kJunk = 0xa5a5a50000000000ull;

P FakeDomainGet() { return &g_domain; }
P* FakeDomainGetAssemblies(P, size_t* size) {
  *size = 2;
  return g_assemblies;
}
P FakeAssemblyGetImage(P assembly) { return assembly; }
P FakeClassFromName(P image, const char* ns, const char* name) {
  for (FakeClass* c : static_cast<FakeImage*>(image)->classes) {
    if (c->ns == ns && c->name == name) return c;
  }
  return nullptr;
}
template <typename T> P Iterate(const std::vector<T*>& items, P* iter) {
  const size_t next = reinterpret_cast<size_t>(*iter);
  if (next >= items.size()) return nullptr;
  *iter = reinterpret_cast<P>(next + 1);
  return items[next];
}
P FakeClassGetNestedTypes(P klass, P* iter) { return Iterate(static_cast<FakeClass*>(klass)->nested, iter); }
const char* FakeClassGetName(P klass) { return static_cast<FakeClass*>(klass)->name.c_str(); }
P FakeClassGetMethods(P klass, P* iter) { return Iterate(static_cast<FakeClass*>(klass)->methods, iter); }
const char* FakeMethodGetName(P m) { return static_cast<FakeMethod*>(m)->name.c_str(); }
Word FakeMethodGetFlags(P m, uint32_t* iflags) {
  *iflags = static_cast<FakeMethod*>(m)->iflags;
  return kJunk | static_cast<FakeMethod*>(m)->flags;
}
Word FakeMethodGetParamCount(P m) { return kJunk | static_cast<FakeMethod*>(m)->params.size(); }
P FakeMethodGetParam(P m, Word i) {
  auto& params = static_cast<FakeMethod*>(m)->params;
  return i < params.size() ? params[i] : nullptr;
}
P FakeMethodGetReturnType(P m) { return static_cast<FakeMethod*>(m)->ret; }
Word FakeTypeGetType(P t) { return 0x7700 | static_cast<FakeType*>(t)->type; }  // junk above the enum byte
Word FakeTypeIsByref(P t) { return 0x5500 | static_cast<FakeType*>(t)->byref; }
P FakeClassFromType(P t) { return static_cast<FakeType*>(t)->klass; }
Word FakeClassIsEnum(P k) { return 0x3300 | static_cast<FakeClass*>(k)->isEnum; }
Word FakeClassIsValueType(P k) { return 0x3300 | static_cast<FakeClass*>(k)->isValueType; }
P FakeClassEnumBasetype(P k) { return static_cast<FakeClass*>(k)->enumBase; }

const std::map<std::string, P> g_api = {
    {"il2cpp_domain_get", reinterpret_cast<P>(&FakeDomainGet)},
    {"il2cpp_domain_get_assemblies", reinterpret_cast<P>(&FakeDomainGetAssemblies)},
    {"il2cpp_assembly_get_image", reinterpret_cast<P>(&FakeAssemblyGetImage)},
    {"il2cpp_class_from_name", reinterpret_cast<P>(&FakeClassFromName)},
    {"il2cpp_class_get_nested_types", reinterpret_cast<P>(&FakeClassGetNestedTypes)},
    {"il2cpp_class_get_name", reinterpret_cast<P>(&FakeClassGetName)},
    {"il2cpp_class_get_methods", reinterpret_cast<P>(&FakeClassGetMethods)},
    {"il2cpp_method_get_name", reinterpret_cast<P>(&FakeMethodGetName)},
    {"il2cpp_method_get_flags", reinterpret_cast<P>(&FakeMethodGetFlags)},
    {"il2cpp_method_get_param_count", reinterpret_cast<P>(&FakeMethodGetParamCount)},
    {"il2cpp_method_get_param", reinterpret_cast<P>(&FakeMethodGetParam)},
    {"il2cpp_method_get_return_type", reinterpret_cast<P>(&FakeMethodGetReturnType)},
    {"il2cpp_type_get_type", reinterpret_cast<P>(&FakeTypeGetType)},
    {"il2cpp_type_is_byref", reinterpret_cast<P>(&FakeTypeIsByref)},
    {"il2cpp_class_from_type", reinterpret_cast<P>(&FakeClassFromType)},
    {"il2cpp_class_is_enum", reinterpret_cast<P>(&FakeClassIsEnum)},
    {"il2cpp_class_is_valuetype", reinterpret_cast<P>(&FakeClassIsValueType)},
    {"il2cpp_class_enum_basetype", reinterpret_cast<P>(&FakeClassEnumBasetype)},
};

// ---- the fake "real" exports the replacements forward to ----------------------------------------

std::map<std::string, uintptr_t> g_registered;  // name -> the stub il2cpp received
int g_initCalls = 0;
uintptr_t g_memory[7] = {};
uintptr_t g_logStub = 0;
P g_heapUserData = nullptr;

void RealAddInternalCall(const char* name, uintptr_t stub) { g_registered[name] = stub; }
Word RealInit(P) {
  ++g_initCalls;
  return kJunk | 1;
}
void RealSetMemoryCallbacks(const uintptr_t* callbacks) { std::memcpy(g_memory, callbacks, sizeof(g_memory)); }
void RealRegisterLogCallback(uintptr_t stub) { g_logStub = stub; }
// Walks the "heap" at once, calling the callback the way the guest would (Microsoft x64).
void RealGcForeachHeap(uintptr_t func, P userData) {
  g_heapUserData = userData;
  if (func) reinterpret_cast<void(MS*)(P, uintptr_t, uintptr_t)>(func)(userData, 0x1000, 0x2000);
}

int g_backtraceInstalls = 0;
void RealOverrideStackBacktrace(uintptr_t) { ++g_backtraceInstalls; }
uintptr_t g_liveness[2] = {};
P RealLivenessAllocateStruct(P filter, Word max, uintptr_t callback, P, uintptr_t reallocate) {
  g_liveness[0] = callback;
  g_liveness[1] = reallocate;
  return max == 64 ? filter : nullptr;
}

const std::map<std::string, P> g_original = {
    {"il2cpp_override_stack_backtrace", reinterpret_cast<P>(&RealOverrideStackBacktrace)},
    {"il2cpp_unity_liveness_allocate_struct", reinterpret_cast<P>(&RealLivenessAllocateStruct)},
    {"il2cpp_add_internal_call", reinterpret_cast<P>(&RealAddInternalCall)},
    {"il2cpp_init", reinterpret_cast<P>(&RealInit)},
    {"il2cpp_set_memory_callbacks", reinterpret_cast<P>(&RealSetMemoryCallbacks)},
    {"il2cpp_register_log_callback", reinterpret_cast<P>(&RealRegisterLogCallback)},
    {"il2cpp_gc_foreach_heap", reinterpret_cast<P>(&RealGcForeachHeap)},
};

P Find(const std::map<std::string, P>& table, std::string_view name) {
  const auto it = table.find(std::string(name));
  return it == table.end() ? nullptr : it->second;
}

// ---- libunity's engine functions (native) --------------------------------------------------------

int g_object;
int32_t NClosest(int32_t v) { return v * 1000 + 7; }
float NHalfToFloat(uint16_t h) { return static_cast<float>(h) + 0.25f; }
double NMix(int32_t a, float x, bool flag, double y) { return a + 10.0 * x + (flag ? 100 : 0) + 1000.0 * y; }
int64_t NPick2(int32_t a, int64_t b) { return a * 10 + b; }
bool NActiveSelf(void* self) { return self == &g_object; }
int16_t NTwice(int16_t mode) { return static_cast<int16_t>(mode * 2); }
P NPing(P s) { return s; }
void NNothing() {}

size_t g_mallocSize = 0;
P NMalloc(size_t size) {
  g_mallocSize = size;
  return &g_mallocSize;
}
std::string g_logged;
void NLog(const char* message) { g_logged = message; }
P g_heapArgs[3] = {};
void NHeap(P a, P b, P c) {
  g_heapArgs[0] = a;
  g_heapArgs[1] = b;
  g_heapArgs[2] = c;
}

template <typename F> F Stub(const char* name) {
  const auto it = g_registered.find(name);
  return it == g_registered.end() ? nullptr : reinterpret_cast<F>(it->second);
}

bool SameKinds(const std::optional<UnityBridge::IcallBridge::Signature>& s, std::initializer_list<Kind> args, Kind ret) {
  if (!s || s->argCount != args.size() || s->ret != ret) return false;
  size_t i = 0;
  for (Kind k : args) {
    if (s->args[i++] != k) return false;
  }
  return true;
}

} // namespace

int main() {
  void* stubs = mmap(nullptr, 64 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
  Check(stubs != MAP_FAILED, "stub memory");
  UnityBridge::Win32::Runtime runtime(UnityBridge::Win32::StubKind::HostNative,
                                      {static_cast<uint8_t*>(stubs), 64 * 1024},
                                      reinterpret_cast<uintptr_t>(&ub_win32_host_entry));
  UnityBridge::Win32::Runtime::SetCurrent(&runtime);
  UnityBridge::IcallBridge::Init(&runtime);

  Intercept::Init([](std::string_view n) { return Find(g_api, n); },
                  [](std::string_view n) { return Find(g_original, n); }, false);
  using Add = void (*)(const char*, P);
  auto* add = reinterpret_cast<Add>(Intercept::Replacement("il2cpp_add_internal_call"));
  auto* init = reinterpret_cast<Word (*)(const char*)>(Intercept::Replacement("il2cpp_init"));
  Check(add && init && Intercept::Replacement("il2cpp_unity_install_unitytls_interface") == nullptr,
        "replacements for the intercepted APIs only");

  // 1. Before il2cpp_init the metadata is not loaded: the registration waits.
  add("UnityEngine.Mathf::ClosestPowerOfTwo", reinterpret_cast<P>(&NClosest));
  Check(g_registered.empty() && Intercept::GetStats().pending == 1, "registration before il2cpp_init is kept");
  Check((init("IL2CPP Root Domain") & 0xffffffff) == 1 && g_initCalls == 1, "il2cpp_init forwarded");
  auto* closest = Stub<int32_t(MS*)(uint64_t)>("UnityEngine.Mathf::ClosestPowerOfTwo");
  Check(closest && reinterpret_cast<uintptr_t>(closest) != reinterpret_cast<uintptr_t>(&NClosest) &&
            Intercept::GetStats().pending == 0,
        "... and registered with a stub once il2cpp_init succeeded");
  Check(closest && closest(0xdead000000000025ull) == 37007, "int(int) through the derived signature");

  // 2. After il2cpp_init: registered at once, signature from the metadata.
  add("UnityEngine.Mathf::HalfToFloat", reinterpret_cast<P>(&NHalfToFloat));
  add("UnityEngine.Mathf::Mix", reinterpret_cast<P>(&NMix));
  add("UnityEngine.Mathf::Pick(System.Int32,System.Int64)", reinterpret_cast<P>(&NPick2));
  add("UnityEngine.GameObject::get_activeSelf()", reinterpret_cast<P>(&NActiveSelf));
  add("UnityEngine.Rendering.Outer/Inner::Twice", reinterpret_cast<P>(&NTwice));
  add("Ns.Thing::Ping", reinterpret_cast<P>(&NPing));
  add("UnityEngine.Mathf::Normalize_Injected", reinterpret_cast<P>(&NNothing));

  auto* half = Stub<float(MS*)(uint64_t)>("UnityEngine.Mathf::HalfToFloat");
  Check(half && half(0xabcd000000001234ull) == 4660.25f, "float(ushort): narrow argument zero-extended");
  auto* mix = Stub<double(MS*)(int64_t, float, uint64_t, double)>("UnityEngine.Mathf::Mix");
  Check(mix && mix(1, 2.0f, 0xff01, 4.0) == 1 + 20 + 100 + 4000 && mix(1, 2.0f, 0x0100, 4.0) == 1 + 20 + 4000,
        "double(int,float,bool,double): interleaved, bool from al only");
  auto* pick = Stub<int64_t(MS*)(int64_t, int64_t)>("UnityEngine.Mathf::Pick(System.Int32,System.Int64)");
  Check(pick && pick(4, 2) == 42, "overload chosen by the parameter list");
  auto* active = Stub<uint64_t(MS*)(P)>("UnityEngine.GameObject::get_activeSelf()");
  Check(active && active(&g_object) == 1 && active(nullptr) == 0, "instance method: this first, bool result extended");
  auto* twice = Stub<uint64_t(MS*)(uint64_t)>("UnityEngine.Rendering.Outer/Inner::Twice");
  Check(twice && twice(0x7777fffdull) == static_cast<uint64_t>(-6),
        "nested class, enum (Int16) argument and result sign-extended");
  auto* ping = Stub<P(MS*)(P)>("Ns.Thing::Ping");
  Check(ping && ping(&g_object) == &g_object, "class found in a second image");
  Check(Stub<void(MS*)(P, P)>("UnityEngine.Mathf::Normalize_Injected") != nullptr, "by-reference value types are pointers");

  // 3. Not registered: absent from the metadata, or not bridgeable.
  const size_t before = g_registered.size();
  add("UnityEngine.Mathf::Lerp", reinterpret_cast<P>(&NNothing));        // managed method
  add("UnityEngine.Nowhere::Thing", reinterpret_cast<P>(&NNothing));     // stripped class
  add("UnityEngine.Mathf::Normalize", reinterpret_cast<P>(&NNothing));   // Vector3 by value
  add("UnityEngine.Mathf::Maybe", reinterpret_cast<P>(&NNothing));       // generic value type
  add("UnityEngine.Mathf::Any", reinterpret_cast<P>(&NNothing));         // generic parameter
  add("NoSeparator", reinterpret_cast<P>(&NNothing));
  const auto stats = Intercept::GetStats();
  Check(g_registered.size() == before, "unbridgeable or absent InternalCalls are not passed on");
  Check(stats.registered == 8 && stats.absent == 2 && stats.failed == 4, "counted: 8 registered, 2 absent, 4 failed");

  // 4. The derivation itself.
  std::string error;
  Check(SameKinds(Intercept::DeriveSignature("UnityEngine.Mathf::Mix", &error),
                  {Kind::Int, Kind::Float, Kind::UInt8, Kind::Float}, Kind::Float),
        "Mix: Int, Float, UInt8, Float -> Float");
  Check(SameKinds(Intercept::DeriveSignature("UnityEngine.Mathf::Pick(System.Int32)", &error), {Kind::Int}, Kind::Int),
        "Pick(System.Int32): the one-parameter overload");
  Check(SameKinds(Intercept::DeriveSignature("UnityEngine.Mathf::Fill(System.Collections.Generic.List<System.Int32,System.String>)", &error),
                  {Kind::Int}, Kind::Int),
        "generic class argument; commas inside <> do not count");
  Check(SameKinds(Intercept::DeriveSignature("UnityEngine.GameObject::get_activeSelf", &error), {Kind::Int}, Kind::UInt8),
        "instance pointer added for an instance method");
  Check(SameKinds(Intercept::DeriveSignature("UnityEngine.Mathf::Nine", &error),
                  {Kind::Int, Kind::Int, Kind::Int, Kind::Int, Kind::Int, Kind::Int, Kind::Int, Kind::Int, Kind::Int}, Kind::Int),
        "nine integer arguments: derived (one goes to the AAPCS64 stack)");
  Check(!Intercept::DeriveSignature("UnityEngine.Mathf::Normalize", &error) && error.find("Vector3") != std::string::npos,
        "struct by value refused, with the reason");
  Check(!Intercept::DeriveSignature("UnityEngine.Mathf::Pick(System.Int32,System.Int32,System.Int32)", &error),
        "no overload with that parameter count");

  // 5. The other host callbacks.
  using SetMemory = void (*)(const P*);
  P callbacks[7] = {reinterpret_cast<P>(&NMalloc)};  // only malloc set, the rest null
  reinterpret_cast<SetMemory>(Intercept::Replacement("il2cpp_set_memory_callbacks"))(callbacks);
  Check(g_memory[0] != 0 && g_memory[0] != reinterpret_cast<uintptr_t>(&NMalloc) && g_memory[1] == 0 && g_memory[6] == 0,
        "memory callbacks: wrapped, null entries stay null");
  Check(g_memory[0] && reinterpret_cast<P(MS*)(size_t)>(g_memory[0])(96) == &g_mallocSize && g_mallocSize == 96,
        "memory callbacks: the guest's malloc reaches ours");

  auto* registerLog = reinterpret_cast<void (*)(P)>(Intercept::Replacement("il2cpp_register_log_callback"));
  registerLog(reinterpret_cast<P>(&NLog));
  const uintptr_t firstLog = g_logStub;
  registerLog(reinterpret_cast<P>(&NLog));
  Check(firstLog != 0 && g_logStub == firstLog, "log callback: one stub for the same function");
  if (g_logStub) reinterpret_cast<void(MS*)(const char*)>(g_logStub)("hello from il2cpp");
  Check(g_logged == "hello from il2cpp", "log callback: message arrives");

  int userData = 0;
  reinterpret_cast<void (*)(P, P)>(Intercept::Replacement("il2cpp_gc_foreach_heap"))(reinterpret_cast<P>(&NHeap), &userData);
  Check(g_heapUserData == &userData && g_heapArgs[0] == &userData && g_heapArgs[1] == reinterpret_cast<P>(0x1000) &&
            g_heapArgs[2] == reinterpret_cast<P>(0x2000),
        "heap walk: callback gets its three arguments");
  Check(Intercept::GetStats().callbacks >= 3, "callbacks counted");

  // The engine's ARM64 unwinder is never handed to il2cpp; the liveness callbacks are wrapped.
  reinterpret_cast<void (*)(P)>(Intercept::Replacement("il2cpp_override_stack_backtrace"))(reinterpret_cast<P>(&NNothing));
  Check(g_backtraceInstalls == 0, "stack backtrace override: not passed on");
  using LivenessAlloc = P (*)(P, Word, P, P, P);
  P live = reinterpret_cast<LivenessAlloc>(Intercept::Replacement("il2cpp_unity_liveness_allocate_struct"))(
      &userData, 64, reinterpret_cast<P>(&NHeap), nullptr, reinterpret_cast<P>(&NMalloc));
  Check(live == &userData && g_liveness[0] != 0 && g_liveness[0] != reinterpret_cast<uintptr_t>(&NHeap) &&
            g_liveness[1] != 0 && g_liveness[1] != reinterpret_cast<uintptr_t>(&NMalloc),
        "liveness: both callbacks wrapped, arguments and result passed through");

  if (g_failures) std::printf("RESULT: %d checks failed\n", g_failures);
  else std::printf("RESULT: all checks passed\n");
  return g_failures ? 1 : 0;
}
