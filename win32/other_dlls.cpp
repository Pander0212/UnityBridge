// SPDX-License-Identifier: GPL-3.0-or-later
// The smaller DLLs IL2CPP's GameAssembly.dll imports: OLEAUT32 (BSTR, SAFEARRAY), OLE32, ADVAPI32
// (random numbers), SHELL32 (folder paths), and network, debug-help and window functions that a
// game on iPhone does not need, answered the way Windows answers when they are unavailable.
#include "common.h"

#include <cstdio>
#include <cstdlib>
#if defined(__APPLE__)
#include <stdlib.h>  // arc4random_buf
#else
#include <sys/random.h>
#endif

namespace UnityBridge::Win32 {
namespace {

using HRESULT = uint32_t;
constexpr HRESULT S_OK = 0, S_FALSE = 1, E_NOTIMPL = 0x80004001u, E_INVALIDARG = 0x80070057u,
                  E_OUTOFMEMORY = 0x8007000Eu, DISP_E_BADINDEX = 0x8002000Bu;

// ---- OLEAUT32: BSTR ----------------------------------------------------------------------
// A BSTR points at UTF-16 text; the 4 bytes before it hold the length in bytes, a terminator
// follows. The pointer handed out is 4 bytes into the allocation.

uint64_t AllocBstr(const char16_t* text, uint32_t chars) {
  auto* block = static_cast<uint8_t*>(std::malloc(4 + size_t {chars} * 2 + 2));
  if (block == nullptr) return 0;
  const uint32_t bytes = chars * 2;
  std::memcpy(block, &bytes, 4);
  auto* out = reinterpret_cast<char16_t*>(block + 4);
  if (text) std::memcpy(out, text, bytes);
  else std::memset(out, 0, bytes);
  out[chars] = 0;
  return reinterpret_cast<uint64_t>(out);
}

void FreeBstr(uint64_t bstr) {
  if (bstr) std::free(reinterpret_cast<uint8_t*>(bstr) - 4);
}

uint64_t SysAllocString(CallContext& c) {
  const auto* text = c.Ptr<const char16_t>(0);
  if (text == nullptr) return 0;
  uint32_t n = 0;
  while (text[n] != 0) ++n;
  return AllocBstr(text, n);
}
uint64_t SysAllocStringLen(CallContext& c) { return AllocBstr(c.Ptr<const char16_t>(0), static_cast<uint32_t>(c.Arg(1))); }
uint64_t SysFreeString(CallContext& c) { FreeBstr(c.Arg(0)); return 0; }
uint64_t SysStringLen(CallContext& c) { return c.Arg(0) ? Load<uint32_t>(c.Arg(0) - 4) / 2 : 0; }

// ---- OLEAUT32: SAFEARRAY -----------------------------------------------------------------
// x64 layout: cDims u16 @0, fFeatures u16 @2, cbElements u32 @4, cLocks u32 @8, pvData @16,
// rgsabound[] @24 ({cElements u32, lLbound i32}, last dimension first). With FADF_HAVEVARTYPE
// the VARTYPE sits in the 4 bytes before the structure; it is allocated 16 bytes early for that.

constexpr uint16_t FADF_BSTR = 0x100, FADF_HAVEVARTYPE = 0x80;
constexpr uint16_t VT_BSTR = 8;

uint32_t ElementSize(uint16_t vt) {
  switch (vt) {
  case 16: case 17: return 1;                       // VT_I1, VT_UI1
  case 2: case 18: case 11: return 2;               // VT_I2, VT_UI2, VT_BOOL
  case 3: case 19: case 4: case 22: case 23: case 10: return 4;  // I4, UI4, R4, INT, UINT, ERROR
  case 20: case 21: case 5: case 6: case 7: return 8;            // I8, UI8, R8, CY, DATE
  case 8: case 9: case 13: return 8;                // BSTR, DISPATCH, UNKNOWN (pointers)
  case 12: return 24;                               // VARIANT
  default: return 0;
  }
}

uint64_t SafeArrayCreate(CallContext& c) {
  const auto vt = static_cast<uint16_t>(c.Arg(0));
  const auto dims = static_cast<uint32_t>(c.Arg(1));
  const uint64_t bounds = c.Arg(2);
  const uint32_t element = ElementSize(vt);
  if (dims == 0 || dims > 64 || bounds == 0 || element == 0) return 0;
  uint64_t count = 1;
  for (uint32_t i = 0; i < dims; ++i) {
    count *= Load<uint32_t>(bounds + i * 8);
    if (count > (1ull << 32)) return 0;
  }
  auto* block = static_cast<uint8_t*>(std::calloc(1, 16 + 24 + size_t {dims} * 8));
  if (block == nullptr) return 0;
  uint8_t* sa = block + 16;
  const uint64_t address = reinterpret_cast<uint64_t>(sa);
  void* data = count ? std::calloc(count, element) : nullptr;
  if (count && data == nullptr) { std::free(block); return 0; }
  Store<uint32_t>(address - 4, vt);
  Store<uint16_t>(address, static_cast<uint16_t>(dims));
  Store<uint16_t>(address + 2, static_cast<uint16_t>(FADF_HAVEVARTYPE | (vt == VT_BSTR ? FADF_BSTR : 0)));
  Store<uint32_t>(address + 4, element);
  Store<uint64_t>(address + 16, reinterpret_cast<uint64_t>(data));
  // The caller's bounds are in dimension order; SAFEARRAY stores them last dimension first.
  for (uint32_t i = 0; i < dims; ++i) Store<uint64_t>(address + 24 + (dims - 1 - i) * 8, Load<uint64_t>(bounds + i * 8));
  return address;
}

uint64_t ElementCount(uint64_t sa) {
  uint64_t count = 1;
  const auto dims = Load<uint16_t>(sa);
  for (uint32_t i = 0; i < dims; ++i) count *= Load<uint32_t>(sa + 24 + i * 8);
  return count;
}

uint64_t SafeArrayDestroy(CallContext& c) {
  const uint64_t sa = c.Arg(0);
  if (sa == 0) return S_OK;
  const auto data = Load<uint64_t>(sa + 16);
  if (Load<uint16_t>(sa + 2) & FADF_BSTR) {
    for (uint64_t i = 0, n = ElementCount(sa); i < n; ++i) FreeBstr(Load<uint64_t>(data + i * 8));
  }
  std::free(reinterpret_cast<void*>(data));
  std::free(reinterpret_cast<uint8_t*>(sa) - 16);
  return S_OK;
}

uint64_t SafeArrayGetDim(CallContext& c) { return c.Arg(0) ? Load<uint16_t>(c.Arg(0)) : 0; }

// nDim counts from 1; rgsabound holds the last dimension first.
std::optional<uint64_t> BoundSlot(uint64_t sa, uint32_t dim) {
  const auto dims = Load<uint16_t>(sa);
  if (sa == 0 || dim == 0 || dim > dims) return std::nullopt;
  return sa + 24 + (dims - dim) * 8;
}

uint64_t SafeArrayGetLBound(CallContext& c) {
  const auto slot = BoundSlot(c.Arg(0), static_cast<uint32_t>(c.Arg(1)));
  if (!slot) return DISP_E_BADINDEX;
  Store<int32_t>(c.Arg(2), Load<int32_t>(*slot + 4));
  return S_OK;
}

uint64_t SafeArrayGetUBound(CallContext& c) {
  const auto slot = BoundSlot(c.Arg(0), static_cast<uint32_t>(c.Arg(1)));
  if (!slot) return DISP_E_BADINDEX;
  Store<int32_t>(c.Arg(2), Load<int32_t>(*slot + 4) + static_cast<int32_t>(Load<uint32_t>(*slot)) - 1);
  return S_OK;
}

uint64_t SafeArrayAccessData(CallContext& c) {
  const uint64_t sa = c.Arg(0);
  if (sa == 0 || c.Arg(1) == 0) return E_INVALIDARG;
  Store<uint32_t>(sa + 8, Load<uint32_t>(sa + 8) + 1);
  Store<uint64_t>(c.Arg(1), Load<uint64_t>(sa + 16));
  return S_OK;
}

uint64_t SafeArrayUnaccessData(CallContext& c) {
  const uint64_t sa = c.Arg(0);
  if (sa == 0) return E_INVALIDARG;
  const auto locks = Load<uint32_t>(sa + 8);
  if (locks) Store<uint32_t>(sa + 8, locks - 1);
  return S_OK;
}

uint64_t SafeArrayGetVartype(CallContext& c) {
  const uint64_t sa = c.Arg(0);
  if (sa == 0 || c.Arg(1) == 0 || !(Load<uint16_t>(sa + 2) & FADF_HAVEVARTYPE)) return E_INVALIDARG;
  Store<uint16_t>(c.Arg(1), static_cast<uint16_t>(Load<uint32_t>(sa - 4)));
  return S_OK;
}

// ---- OLE32 -------------------------------------------------------------------------------

thread_local int t_comInits = 0;
uint64_t CoInitializeEx(CallContext&) { return t_comInits++ == 0 ? S_OK : S_FALSE; }
uint64_t CoUninitialize(CallContext&) { if (t_comInits > 0) --t_comInits; return 0; }
uint64_t CoTaskMemAlloc(CallContext& c) { return reinterpret_cast<uint64_t>(std::malloc(c.Arg(0) ? c.Arg(0) : 1)); }
uint64_t CoTaskMemFree(CallContext& c) { std::free(c.Ptr<void>(0)); return 0; }
uint64_t CoGetContextToken(CallContext&) { return E_NOTIMPL; }
uint64_t CoCreateFreeThreadedMarshaler(CallContext&) { return E_NOTIMPL; }
// CoGetApartmentType(pAptType, pAptQualifier): report a multithreaded apartment, no qualifier.
uint64_t CoGetApartmentType(CallContext& c) {
  Store<DWORD>(c.Arg(0), 1);  // APTTYPE_MTA
  Store<DWORD>(c.Arg(1), 0);  // APTTYPEQUALIFIER_NONE
  return S_OK;
}

// ---- ADVAPI32: random numbers ------------------------------------------------------------

constexpr uint64_t kCryptProvider = 0x7ffb'0000'0001ull;
uint64_t CryptAcquireContextW(CallContext& c) { Store<uint64_t>(c.Arg(0), kCryptProvider); return kTrue; }
uint64_t CryptReleaseContext(CallContext&) { return kTrue; }
// Fills `out` from the system's generator.
bool FillRandom(uint8_t* out, size_t length) {
#if defined(__APPLE__)
  arc4random_buf(out, length);
#else
  while (length) {
    const auto n = getrandom(out, length, 0);
    if (n <= 0) return false;
    out += n;
    length -= static_cast<size_t>(n);
  }
#endif
  return true;
}

uint64_t CryptGenRandom(CallContext& c) {
  auto* out = c.Ptr<uint8_t>(2);
  const size_t length = static_cast<DWORD>(c.Arg(1));
  if (out == nullptr && length) return SetError(ERROR_INVALID_PARAMETER, kFalse);
  if (!FillRandom(out, length)) return SetError(ERROR_NOT_SUPPORTED, kFalse);
  return kTrue;
}

// ---- BCRYPT: random numbers (.NET's RandomNumberGenerator and Guid.NewGuid P/Invoke these) -------
// NTSTATUS results. Only the RNG algorithm is provided; its handle is a constant.

constexpr uint64_t kStatusSuccess = 0, kStatusInvalidParameter = 0xc000000d, kStatusNotFound = 0xc0000225;
constexpr uint64_t kRngAlgorithm = 0x7ffb'0000'0002ull;
constexpr uint32_t kUseSystemPreferredRng = 0x2;  // BCRYPT_USE_SYSTEM_PREFERRED_RNG

uint64_t BCryptOpenAlgorithmProvider(CallContext& c) {  // (phAlgorithm, pszAlgId, pszImplementation, dwFlags)
  const auto id = c.Arg(1) ? ToUtf8(ReadWide(c.Ptr<const char16_t>(1))) : std::string();
  if (c.Arg(0) == 0) return kStatusInvalidParameter;
  if (id != "RNG") {
    std::fprintf(stderr, "UB_WIN32 BCryptOpenAlgorithmProvider(%s): only RNG is provided\n", id.c_str());
    return kStatusNotFound;
  }
  Store<uint64_t>(c.Arg(0), kRngAlgorithm);
  return kStatusSuccess;
}

uint64_t BCryptCloseAlgorithmProvider(CallContext&) { return kStatusSuccess; }

uint64_t BCryptGenRandom(CallContext& c) {  // (hAlgorithm, pbBuffer, cbBuffer, dwFlags)
  auto* out = c.Ptr<uint8_t>(1);
  const size_t length = static_cast<uint32_t>(c.Arg(2));
  const auto flags = static_cast<uint32_t>(c.Arg(3));
  if (out == nullptr && length) return kStatusInvalidParameter;
  if (!(flags & kUseSystemPreferredRng) && c.Arg(0) != kRngAlgorithm) return kStatusInvalidParameter;
  return FillRandom(out, length) ? kStatusSuccess : kStatusInvalidParameter;
}

// ---- SHELL32: folder paths ---------------------------------------------------------------
// All profile folders live under C:\Users\Player; where that ends up on the device is decided by
// the file system layer.

std::u16string FolderForCsidl(uint32_t csidl) {
  switch (csidl & 0xff) {
  case 0x05: return u"C:\\Users\\Player\\Documents";          // CSIDL_PERSONAL
  case 0x1a: return u"C:\\Users\\Player\\AppData\\Roaming";   // CSIDL_APPDATA
  case 0x1c: return u"C:\\Users\\Player\\AppData\\Local";     // CSIDL_LOCAL_APPDATA
  case 0x23: return u"C:\\ProgramData";                       // CSIDL_COMMON_APPDATA
  case 0x28: return u"C:\\Users\\Player";                     // CSIDL_PROFILE
  default: return u"C:\\Users\\Player";
  }
}

uint64_t SHGetFolderPathW(CallContext& c) {
  const auto path = FolderForCsidl(static_cast<uint32_t>(c.Arg(1)));
  auto* out = c.Ptr<char16_t>(4);  // MAX_PATH characters
  if (out == nullptr || path.size() >= 260) return E_INVALIDARG;
  std::memcpy(out, path.data(), path.size() * 2);
  out[path.size()] = 0;
  return S_OK;
}

uint64_t SHGetKnownFolderPath(CallContext& c) {
  // KNOWNFOLDERID by its first 32 bits: FOLDERID_LocalAppDataLow is the one Unity uses
  // (Application.persistentDataPath); the rest fall back to the profile root.
  const auto id = Load<uint32_t>(c.Arg(0));
  std::u16string path = u"C:\\Users\\Player";
  if (id == 0xa520a1a4u) path = u"C:\\Users\\Player\\AppData\\LocalLow";       // FOLDERID_LocalAppDataLow
  else if (id == 0xf1b32785u) path = u"C:\\Users\\Player\\AppData\\Local";     // FOLDERID_LocalAppData
  else if (id == 0x3eb685dbu) path = u"C:\\Users\\Player\\AppData\\Roaming";   // FOLDERID_RoamingAppData
  else if (id == 0xfdd39ad0u) path = u"C:\\Users\\Player\\Documents";          // FOLDERID_Documents
  auto* out = static_cast<char16_t*>(std::malloc((path.size() + 1) * 2));      // freed with CoTaskMemFree
  if (out == nullptr) return E_OUTOFMEMORY;
  std::memcpy(out, path.data(), path.size() * 2);
  out[path.size()] = 0;
  Store<uint64_t>(c.Arg(3), reinterpret_cast<uint64_t>(out));
  return S_OK;
}

// ---- WS2_32 and IPHLPAPI: no networking for now ------------------------------------------

constexpr DWORD WSAENETDOWN = 10050, ERROR_NO_DATA = 232;
uint64_t WSAStartup(CallContext& c) {
  // WSADATA (408 bytes on x64): version fields first.
  if (c.Arg(1)) {
    std::memset(c.Ptr<void>(1), 0, 408);
    Store<uint16_t>(c.Arg(1), 0x0202);
    Store<uint16_t>(c.Arg(1) + 2, 0x0202);
  }
  return 0;
}
uint64_t WSACleanup(CallContext&) { return 0; }
uint64_t WSAGetLastError(CallContext&) { return WSAENETDOWN; }
uint64_t send(CallContext&) { return static_cast<uint32_t>(-1); }  // SOCKET_ERROR
uint64_t GetAdaptersAddresses(CallContext&) { return ERROR_NO_DATA; }
uint64_t GetIfEntry(CallContext&) { return ERROR_NOT_SUPPORTED; }
uint64_t GetNetworkParams(CallContext&) { return ERROR_NOT_SUPPORTED; }

// ---- DBGHELP and USER32 ------------------------------------------------------------------

uint64_t SymInitialize(CallContext&) { return SetError(ERROR_NOT_SUPPORTED, kFalse); }
uint64_t SymFromAddr(CallContext&) { return SetError(ERROR_NOT_SUPPORTED, kFalse); }
uint64_t EnumWindows(CallContext&) { return kTrue; }  // there are no windows to enumerate
uint64_t GetWindow(CallContext&) { return 0; }
uint64_t GetWindowThreadProcessId(CallContext&) { return 0; }
uint64_t IsWindowVisible(CallContext&) { return kFalse; }
uint64_t MessageBoxA(CallContext& c) {
  std::fprintf(stderr, "UB_WIN32 MessageBox \"%s\": %s\n", c.Arg(2) ? c.Ptr<const char>(2) : "",
               c.Arg(1) ? c.Ptr<const char>(1) : "");
  return 1;  // IDOK
}

} // namespace

void RegisterOtherDlls(std::vector<Function>& out) {
  // OLEAUT32 is imported by ordinal; register both the ordinal and the name.
  const std::string oa = "oleaut32.dll";
  auto ole = [&](int ordinal, const char* name, Handler h) {
    out.push_back({oa, "#" + std::to_string(ordinal), h});
    out.push_back({oa, name, h});
  };
  ole(2, "SysAllocString", &SysAllocString);
  ole(4, "SysAllocStringLen", &SysAllocStringLen);
  ole(6, "SysFreeString", &SysFreeString);
  ole(7, "SysStringLen", &SysStringLen);
  ole(15, "SafeArrayCreate", &SafeArrayCreate);
  ole(16, "SafeArrayDestroy", &SafeArrayDestroy);
  ole(17, "SafeArrayGetDim", &SafeArrayGetDim);
  ole(19, "SafeArrayGetUBound", &SafeArrayGetUBound);
  ole(20, "SafeArrayGetLBound", &SafeArrayGetLBound);
  ole(23, "SafeArrayAccessData", &SafeArrayAccessData);
  ole(24, "SafeArrayUnaccessData", &SafeArrayUnaccessData);
  ole(77, "SafeArrayGetVartype", &SafeArrayGetVartype);

  const std::string o32 = "ole32.dll";
  out.push_back({o32, "CoInitializeEx", &CoInitializeEx});
  out.push_back({o32, "CoUninitialize", &CoUninitialize});
  out.push_back({o32, "CoTaskMemAlloc", &CoTaskMemAlloc});
  out.push_back({o32, "CoTaskMemFree", &CoTaskMemFree});
  out.push_back({o32, "CoGetContextToken", &CoGetContextToken});
  out.push_back({o32, "CoCreateFreeThreadedMarshaler", &CoCreateFreeThreadedMarshaler});
  out.push_back({o32, "CoGetApartmentType", &CoGetApartmentType});

  const std::string adv = "advapi32.dll";
  out.push_back({adv, "CryptAcquireContextW", &CryptAcquireContextW});
  out.push_back({adv, "CryptReleaseContext", &CryptReleaseContext});
  out.push_back({adv, "CryptGenRandom", &CryptGenRandom});

  const std::string bc = "bcrypt.dll";
  out.push_back({bc, "BCryptOpenAlgorithmProvider", &BCryptOpenAlgorithmProvider});
  out.push_back({bc, "BCryptCloseAlgorithmProvider", &BCryptCloseAlgorithmProvider});
  out.push_back({bc, "BCryptGenRandom", &BCryptGenRandom});

  out.push_back({"shell32.dll", "SHGetFolderPathW", &SHGetFolderPathW});
  out.push_back({"shell32.dll", "SHGetKnownFolderPath", &SHGetKnownFolderPath});

  // WS2_32 is imported by ordinal as well.
  const std::string ws = "ws2_32.dll";
  auto sock = [&](int ordinal, const char* name, Handler h) {
    out.push_back({ws, "#" + std::to_string(ordinal), h});
    out.push_back({ws, name, h});
  };
  sock(19, "send", &send);
  sock(111, "WSAGetLastError", &WSAGetLastError);
  sock(115, "WSAStartup", &WSAStartup);
  sock(116, "WSACleanup", &WSACleanup);

  out.push_back({"iphlpapi.dll", "GetAdaptersAddresses", &GetAdaptersAddresses});
  out.push_back({"iphlpapi.dll", "GetIfEntry", &GetIfEntry});
  out.push_back({"iphlpapi.dll", "GetNetworkParams", &GetNetworkParams});

  out.push_back({"dbghelp.dll", "SymInitialize", &SymInitialize});
  out.push_back({"dbghelp.dll", "SymFromAddr", &SymFromAddr});

  const std::string u32 = "user32.dll";
  out.push_back({u32, "EnumWindows", &EnumWindows});
  out.push_back({u32, "GetWindow", &GetWindow});
  out.push_back({u32, "GetWindowThreadProcessId", &GetWindowThreadProcessId});
  out.push_back({u32, "IsWindowVisible", &IsWindowVisible});
  out.push_back({u32, "MessageBoxA", &MessageBoxA});
}

} // namespace UnityBridge::Win32
