// SPDX-License-Identifier: GPL-3.0-or-later
// KERNEL32: locale (the guest runs as en-US with UTF-8), string comparison and case mapping,
// time zone, error messages, and process-level odds and ends.
#include "common.h"

#include <cstdio>
#include <filesystem>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

namespace UnityBridge::Win32 {
namespace {

constexpr DWORD ERROR_INVALID_FLAGS = 1004;
constexpr DWORD kLcidEnUs = 0x409, kLcidInvariant = 0x7f;

// ---- characters --------------------------------------------------------------------------
// Case mapping for ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic; other characters map
// to themselves. Enough for identifiers, file names and the usual game text.

// Latin Extended-A pairs letters as (upper, lower); the upper case sits on the even code point
// in U+0100-0137 and U+014A-0177, on the odd one in U+0139-0148 and U+0179-017E.
bool LatinExtUpperIsEven(char16_t c) { return (c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177); }
bool LatinExtUpperIsOdd(char16_t c) { return (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e); }

char16_t ToUpper(char16_t c) {
  if (c >= u'a' && c <= u'z') return c - 32;
  if ((c >= 0xe0 && c <= 0xfe && c != 0xf7)) return c - 32;
  if (c == 0xff) return 0x178;
  if (LatinExtUpperIsEven(c) && (c & 1) && c != 0x131) return c - 1;
  if (LatinExtUpperIsOdd(c) && !(c & 1)) return c - 1;
  if (c >= 0x3b1 && c <= 0x3c9 && c != 0x3c2) return c - 32;
  if (c >= 0x430 && c <= 0x44f) return c - 32;
  if (c >= 0x450 && c <= 0x45f) return c - 80;
  return c;
}

char16_t ToLower(char16_t c) {
  if (c >= u'A' && c <= u'Z') return c + 32;
  if ((c >= 0xc0 && c <= 0xde && c != 0xd7)) return c + 32;
  if (c == 0x178) return 0xff;
  if (LatinExtUpperIsEven(c) && !(c & 1) && c != 0x130) return c + 1;
  if (LatinExtUpperIsOdd(c) && (c & 1)) return c + 1;
  if (c >= 0x391 && c <= 0x3a9 && c != 0x3a2) return c + 32;
  if (c >= 0x410 && c <= 0x42f) return c + 32;
  if (c >= 0x400 && c <= 0x40f) return c + 80;
  return c;
}

bool IsLetter(char16_t c) {
  return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= 0xc0 && c <= 0x24f && c != 0xd7 && c != 0xf7) ||
         (c >= 0x370 && c <= 0x3ff) || (c >= 0x400 && c <= 0x4ff) || (c >= 0x3040 && c <= 0x9fff) || (c >= 0xac00 && c <= 0xd7af);
}

std::u16string_view Text(const char16_t* text, int32_t length) {
  if (text == nullptr) return {};
  if (length >= 0) return {text, static_cast<size_t>(length)};
  size_t n = 0;
  while (text[n] != 0) ++n;
  return {text, n + 1};  // -1: including the terminator, as Windows counts it
}

// ---- locale ------------------------------------------------------------------------------

uint64_t GetUserDefaultLCID(CallContext&) { return kLcidEnUs; }
uint64_t GetThreadLocale(CallContext&) { return kLcidEnUs; }
uint64_t IsValidLocale(CallContext& c) {
  const auto lcid = static_cast<DWORD>(c.Arg(0));
  return (lcid == kLcidEnUs || lcid == kLcidInvariant || lcid == 0x400 || lcid == 0x800) ? kTrue : kFalse;
}
// The one locale the mini Win32 knows, as its LCID in 8 hex digits; the callback returns FALSE to stop.
uint64_t EnumSystemLocalesW(CallContext& c) {
  static const char16_t kEnUs[] = u"00000409";
  CallGuest(c.Arg(0), {reinterpret_cast<uintptr_t>(kEnUs)});
  return kTrue;
}

uint64_t GetLocaleInfoW(CallContext& c) {
  constexpr DWORD LOCALE_RETURN_NUMBER = 0x20000000u, LOCALE_NOUSEROVERRIDE = 0x80000000u;
  const auto type = static_cast<DWORD>(c.Arg(1)) & ~(LOCALE_RETURN_NUMBER | LOCALE_NOUSEROVERRIDE);
  auto* out = c.Ptr<char16_t>(2);
  const auto size = static_cast<int32_t>(c.Arg(3));
  if (static_cast<DWORD>(c.Arg(1)) & LOCALE_RETURN_NUMBER) {
    DWORD value;
    switch (type) {
    case 0x1: value = kLcidEnUs; break;                 // LOCALE_ILANGUAGE
    case 0x1004: value = 65001; break;                  // LOCALE_IDEFAULTANSICODEPAGE
    case 0xb: value = 65001; break;                     // LOCALE_IDEFAULTCODEPAGE (OEM)
    case 0x100c: value = 0; break;                      // LOCALE_IFIRSTDAYOFWEEK (Monday = 0)
    default: return SetError(ERROR_INVALID_FLAGS, 0);
    }
    if (size == 0) return 2;
    if (size < 2) return SetError(ERROR_INSUFFICIENT_BUFFER, 0);
    Store<DWORD>(c.Arg(2), value);
    return 2;
  }
  const char16_t* text;
  switch (type) {
  case 0x1: text = u"0409"; break;            // LOCALE_ILANGUAGE
  case 0x2: text = u"English (United States)"; break;  // LOCALE_SLANGUAGE
  case 0x3: text = u"ENU"; break;             // LOCALE_SABBREVLANGNAME
  case 0xe: text = u"."; break;               // LOCALE_SDECIMAL
  case 0xf: text = u","; break;               // LOCALE_STHOUSAND
  case 0x14: text = u"$"; break;              // LOCALE_SCURRENCY
  case 0x1d: text = u"/"; break;              // LOCALE_SDATE
  case 0x1e: text = u":"; break;              // LOCALE_STIME
  case 0x1f: text = u"M/d/yyyy"; break;       // LOCALE_SSHORTDATE
  case 0x50: text = u"-"; break;              // LOCALE_SNEGATIVESIGN
  case 0x59: text = u"en"; break;             // LOCALE_SISO639LANGNAME
  case 0x5a: text = u"US"; break;             // LOCALE_SISO3166CTRYNAME
  case 0x5c: text = u"en-US"; break;          // LOCALE_SNAME
  case 0x1001: text = u"English"; break;      // LOCALE_SENGLISHLANGUAGENAME
  case 0x1002: text = u"United States"; break;  // LOCALE_SENGLISHCOUNTRYNAME
  case 0x1004: text = u"65001"; break;        // LOCALE_IDEFAULTANSICODEPAGE
  default: return SetError(ERROR_INVALID_FLAGS, 0);
  }
  const std::u16string_view value(text);
  if (size == 0) return value.size() + 1;
  if (static_cast<size_t>(size) <= value.size()) return SetError(ERROR_INSUFFICIENT_BUFFER, 0);
  std::memcpy(out, value.data(), value.size() * 2);
  out[value.size()] = 0;
  return value.size() + 1;
}

// GetLocaleInfoEx(name, LCType, data, cch): same argument layout as GetLocaleInfoW from LCType on,
// and we answer as en-US regardless of the name, so it reuses the same handler.

// GetUserDefaultLocaleName(name, cch): the user default locale, en-US.
uint64_t GetUserDefaultLocaleName(CallContext& c) {
  static const char16_t kName[] = u"en-US";
  auto* out = c.Ptr<char16_t>(0);
  const auto size = static_cast<int32_t>(c.Arg(1));
  constexpr int32_t need = 6;  // "en-US" + null
  if (size == 0) return need;
  if (size < need || out == nullptr) return SetError(ERROR_INSUFFICIENT_BUFFER, 0);
  std::memcpy(out, kName, need * 2);
  return need;
}

uint64_t IsValidLocaleName(CallContext&) { return kTrue; }  // we only model en-US, but never block

uint64_t GetCPInfo(CallContext& c) {
  // CPINFO: MaxCharSize u32, DefaultChar[2], LeadByte[12].
  const auto cp = static_cast<DWORD>(c.Arg(0));
  if (cp != 0 && cp != 1 && cp != 65001 && cp != 1252 && cp != 20127) return SetError(ERROR_INVALID_PARAMETER, kFalse);
  std::memset(c.Ptr<void>(1), 0, 20);
  Store<DWORD>(c.Arg(1), (cp == 1252 || cp == 20127) ? 1 : 4);
  Store<uint8_t>(c.Arg(1) + 4, '?');
  return kTrue;
}

// LCMapString core: case mapping only (sort keys and width/kana folding are not supported).
uint64_t MapString(DWORD flags, std::u16string_view in, char16_t* out, int32_t outSize) {
  constexpr DWORD LCMAP_LOWERCASE = 0x100, LCMAP_UPPERCASE = 0x200, LCMAP_SORTKEY = 0x400;
  if (flags & LCMAP_SORTKEY) return SetError(ERROR_INVALID_FLAGS, 0);
  if (outSize == 0) return in.size();
  if (static_cast<size_t>(outSize) < in.size()) return SetError(ERROR_INSUFFICIENT_BUFFER, 0);
  for (size_t i = 0; i < in.size(); ++i) {
    char16_t ch = in[i];
    if (flags & LCMAP_UPPERCASE) ch = ToUpper(ch);
    else if (flags & LCMAP_LOWERCASE) ch = ToLower(ch);
    out[i] = ch;
  }
  return in.size();
}

uint64_t LCMapStringW(CallContext& c) {
  return MapString(static_cast<DWORD>(c.Arg(1)), Text(c.Ptr<const char16_t>(2), static_cast<int32_t>(c.Arg(3))),
                   c.Ptr<char16_t>(4), static_cast<int32_t>(c.Arg(5)));
}

uint64_t LCMapStringEx(CallContext& c) {
  return MapString(static_cast<DWORD>(c.Arg(1)), Text(c.Ptr<const char16_t>(2), static_cast<int32_t>(c.Arg(3))),
                   c.Ptr<char16_t>(4), static_cast<int32_t>(c.Arg(5)));
}

uint64_t CompareStringW(CallContext& c) {
  constexpr DWORD NORM_IGNORECASE = 0x1;
  constexpr uint64_t CSTR_LESS_THAN = 1, CSTR_EQUAL = 2, CSTR_GREATER_THAN = 3;
  const bool ignoreCase = static_cast<DWORD>(c.Arg(1)) & NORM_IGNORECASE;
  auto a = Text(c.Ptr<const char16_t>(2), static_cast<int32_t>(c.Arg(3)));
  auto b = Text(c.Ptr<const char16_t>(4), static_cast<int32_t>(c.Arg(5)));
  if (!a.empty() && a.back() == 0) a.remove_suffix(1);
  if (!b.empty() && b.back() == 0) b.remove_suffix(1);
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
    char16_t x = a[i], y = b[i];
    if (ignoreCase) { x = ToLower(x); y = ToLower(y); }
    if (x != y) return x < y ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
  }
  return a.size() == b.size() ? CSTR_EQUAL : (a.size() < b.size() ? CSTR_LESS_THAN : CSTR_GREATER_THAN);
}

uint64_t GetStringTypeW(CallContext& c) {
  constexpr DWORD CT_CTYPE1 = 1;
  constexpr uint16_t C1_UPPER = 0x1, C1_LOWER = 0x2, C1_DIGIT = 0x4, C1_SPACE = 0x8, C1_PUNCT = 0x10,
                     C1_CNTRL = 0x20, C1_BLANK = 0x40, C1_XDIGIT = 0x80, C1_ALPHA = 0x100, C1_DEFINED = 0x200;
  if (static_cast<DWORD>(c.Arg(0)) != CT_CTYPE1) return SetError(ERROR_INVALID_FLAGS, kFalse);
  auto in = Text(c.Ptr<const char16_t>(1), static_cast<int32_t>(c.Arg(2)));
  auto* out = c.Ptr<uint16_t>(3);
  for (size_t i = 0; i < in.size(); ++i) {
    const char16_t ch = in[i];
    uint16_t t = C1_DEFINED;
    if (ch >= u'0' && ch <= u'9') t |= C1_DIGIT | C1_XDIGIT;
    if ((ch >= u'a' && ch <= u'f') || (ch >= u'A' && ch <= u'F')) t |= C1_XDIGIT;
    if (IsLetter(ch)) {
      t |= C1_ALPHA;
      if (ToLower(ch) != ch) t |= C1_UPPER;
      if (ToUpper(ch) != ch) t |= C1_LOWER;
    }
    if (ch == u' ' || (ch >= 9 && ch <= 13) || ch == 0xa0 || ch == 0x3000) t |= C1_SPACE;
    if (ch == u' ' || ch == u'\t' || ch == 0xa0 || ch == 0x3000) t |= C1_BLANK;
    if (ch < 32 || ch == 127) t |= C1_CNTRL;
    if (ch > 32 && ch < 127 && !(t & (C1_ALPHA | C1_DIGIT))) t |= C1_PUNCT;
    out[i] = t;
  }
  return kTrue;
}

// ---- time zone ---------------------------------------------------------------------------
// The host's current UTC offset as a fixed bias; daylight-saving rules are not reported
// (TIME_ZONE_ID_UNKNOWN), so the guest converts with today's offset.

void FillTimeZone(uint64_t tzi) {
  // TIME_ZONE_INFORMATION (172 bytes): Bias @0, StandardName[32] @4, StandardDate @68,
  // StandardBias @84, DaylightName[32] @88, DaylightDate @152, DaylightBias @168.
  std::memset(reinterpret_cast<void*>(tzi), 0, 172);
  const time_t now = std::time(nullptr);
  struct tm local {};
  localtime_r(&now, &local);
  Store<int32_t>(tzi, static_cast<int32_t>(-local.tm_gmtoff / 60));
  const auto name = ToUtf16(local.tm_zone ? local.tm_zone : "UTC").substr(0, 31);
  std::memcpy(reinterpret_cast<void*>(tzi + 4), name.data(), name.size() * 2);
  std::memcpy(reinterpret_cast<void*>(tzi + 88), name.data(), name.size() * 2);
}

uint64_t GetTimeZoneInformation(CallContext& c) {
  FillTimeZone(c.Arg(0));
  return 0;  // TIME_ZONE_ID_UNKNOWN
}

uint64_t GetDynamicTimeZoneInformation(CallContext& c) {
  // DYNAMIC_TIME_ZONE_INFORMATION: the 172 bytes above, TimeZoneKeyName[128] @172,
  // DynamicDaylightTimeDisabled @428.
  std::memset(c.Ptr<void>(0), 0, 432);
  FillTimeZone(c.Arg(0));
  const std::u16string key = u"UTC";
  std::memcpy(reinterpret_cast<void*>(c.Arg(0) + 172), key.data(), key.size() * 2);
  Store<uint8_t>(c.Arg(0) + 428, 1);
  return 0;
}

uint64_t GetSystemTimes(CallContext& c) {
  for (unsigned i = 0; i < 3; ++i) Store<uint64_t>(c.Arg(i), 0);
  return kTrue;
}

// ---- error messages ----------------------------------------------------------------------

uint64_t FormatMessageW(CallContext& c) {
  constexpr DWORD FORMAT_MESSAGE_ALLOCATE_BUFFER = 0x100, FORMAT_MESSAGE_FROM_SYSTEM = 0x1000;
  const auto flags = static_cast<DWORD>(c.Arg(0));
  if (!(flags & FORMAT_MESSAGE_FROM_SYSTEM)) return SetError(ERROR_NOT_SUPPORTED, 0);
  const auto message = ToUtf16("Windows error " + std::to_string(static_cast<DWORD>(c.Arg(2))) + "\r\n");
  if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
    auto* buffer = static_cast<char16_t*>(std::malloc((message.size() + 1) * 2));
    if (buffer == nullptr) return SetError(ERROR_NOT_ENOUGH_MEMORY, 0);
    std::memcpy(buffer, message.data(), message.size() * 2);
    buffer[message.size()] = 0;
    Store<uint64_t>(c.Arg(4), reinterpret_cast<uint64_t>(buffer));  // freed with LocalFree
    return message.size();
  }
  const auto size = static_cast<DWORD>(c.Arg(5));
  if (size <= message.size()) return SetError(ERROR_INSUFFICIENT_BUFFER, 0);
  auto* out = c.Ptr<char16_t>(4);
  std::memcpy(out, message.data(), message.size() * 2);
  out[message.size()] = 0;
  return message.size();
}

// ---- process -----------------------------------------------------------------------------

constexpr uint64_t kCurrentProcess = ~uint64_t {0};
constexpr DWORD STILL_ACTIVE = 259;

uint64_t ExitProcess(CallContext& c) {
  std::fprintf(stderr, "UB_WIN32 ExitProcess(%u)\n", static_cast<DWORD>(c.Arg(0)));
  std::fflush(stderr);
  std::_Exit(static_cast<int>(c.Arg(0)));
}

uint64_t TerminateProcess(CallContext& c) {
  if (c.Arg(0) != kCurrentProcess) return SetError(ERROR_ACCESS_DENIED, kFalse);
  std::fprintf(stderr, "UB_WIN32 TerminateProcess(%u)\n", static_cast<DWORD>(c.Arg(1)));
  std::fflush(stderr);
  std::_Exit(static_cast<int>(c.Arg(1)));
}

uint64_t GetExitCodeProcess(CallContext& c) {
  if (c.Arg(0) != kCurrentProcess) return SetError(ERROR_INVALID_HANDLE, kFalse);
  Store<DWORD>(c.Arg(1), STILL_ACTIVE);
  return kTrue;
}

uint64_t K32GetProcessImageFileNameW(CallContext& c) {
  const auto* main = MainModule();
  if (c.Arg(0) != kCurrentProcess || main == nullptr) return SetError(ERROR_INVALID_HANDLE, 0);
  const auto copied = CopyWideOut(main->path, c.Ptr<char16_t>(1), static_cast<DWORD>(c.Arg(2)));
  return copied == main->path.size() ? copied : SetError(ERROR_INSUFFICIENT_BUFFER, 0);
}

uint64_t SetStdHandle(CallContext&) { return kTrue; }
uint64_t ReadConsoleW(CallContext&) { return SetError(ERROR_INVALID_HANDLE, kFalse); }

thread_local DWORD t_errorMode = 0;
uint64_t SetThreadErrorMode(CallContext& c) {
  Store<DWORD>(c.Arg(1), t_errorMode);
  t_errorMode = static_cast<DWORD>(c.Arg(0));
  return kTrue;
}

// SLIST_HEADER on x64: first quadword Depth:16 | Sequence:48, second quadword
// Reserved:4 | NextEntry:60 (the first entry's address >> 4). The guest pushes and pops with
// cmpxchg16b itself; flushing swaps in an empty header with a 128-bit compare-and-swap too.
uint64_t InterlockedFlushSList(CallContext& c) {
  // __sync builtins: GCC inlines them as cmpxchg16b with -mcx16 (it would call libatomic for the
  // __atomic forms), Clang on arm64 as casp/ldxp loops.
  auto* header = c.Ptr<unsigned __int128>(0);
  unsigned __int128 old = __sync_val_compare_and_swap(header, 0, 0);  // atomic read
  for (;;) {
    const auto next = static_cast<uint64_t>(old >> 64) >> 4 << 4;
    if (next == 0) return 0;
    const uint64_t sequence = static_cast<uint64_t>(old) & ~uint64_t {0xffff};  // keep Sequence, Depth = 0
    const unsigned __int128 empty = sequence;
    const unsigned __int128 seen = __sync_val_compare_and_swap(header, old, empty);
    if (seen == old) return next;
    old = seen;
  }
}

// ---- Unity 6 (M6) ------------------------------------------------------------------------

// LocalAlloc(flags, bytes): LMEM_ZEROINIT (0x40) zeroes; LMEM_MOVEABLE handles are plain pointers here.
uint64_t LocalAlloc(CallContext& c) {
  const size_t n = c.Arg(1) ? size_t(c.Arg(1)) : 1;
  return reinterpret_cast<uint64_t>((c.Arg(0) & 0x40) ? std::calloc(1, n) : std::malloc(n));
}
uint64_t LocalReAlloc(CallContext& c) {  // (memory, bytes, flags)
  return reinterpret_cast<uint64_t>(std::realloc(c.Ptr<void>(0), c.Arg(1) ? size_t(c.Arg(1)) : 1));
}
uint64_t LocalFree(CallContext& c) { std::free(c.Ptr<void>(0)); return 0; }

uint64_t SetThreadDescription(CallContext&) { return 0; }  // S_OK
uint64_t GetThreadDescription(CallContext& c) {            // (thread, PWSTR* description): "", LocalFree'd
  auto* empty = static_cast<char16_t*>(std::calloc(1, sizeof(char16_t)));
  Store<uint64_t>(c.Arg(1), reinterpret_cast<uint64_t>(empty));
  return 0;
}

// CopyFile2(source, destination, COPYFILE2_EXTENDED_PARAMETERS*): HRESULT. dwCopyFlags at +4;
// COPY_FILE_FAIL_IF_EXISTS = 1.
uint64_t CopyFile2(CallContext& c) {
  const auto from = HostPath(ReadWide(c.Ptr<const char16_t>(0)));
  const auto to = HostPath(ReadWide(c.Ptr<const char16_t>(1)));
  const bool failIfExists = c.Arg(2) && (Load<uint32_t>(c.Arg(2) + 4) & 1);
  std::error_code ec;
  const auto options = failIfExists ? std::filesystem::copy_options::none
                                    : std::filesystem::copy_options::overwrite_existing;
  if (std::filesystem::copy_file(from, to, options, ec)) return 0;
  const DWORD error = ec ? ErrorFromErrno(ec.value()) : 80;  // ERROR_FILE_EXISTS
  return 0x80070000u | error;                                // HRESULT_FROM_WIN32
}

uint64_t GetTimeZoneInformationForYear(CallContext& c) {  // (year, DYNAMIC_TIME_ZONE_INFORMATION*, TIME_ZONE_INFORMATION*)
  if (c.Arg(2)) std::memset(c.Ptr<void>(2), 0, 172);    // UTC, no daylight saving
  return kTrue;
}

uint64_t GetComputerNameW(CallContext& c) {  // (buffer, DWORD* size)
  static const std::u16string name = u"IPHONE";
  auto* size = c.Ptr<DWORD>(1);
  if (!size) return SetError(87, kFalse);
  if (*size <= name.size()) {
    *size = static_cast<DWORD>(name.size() + 1);
    return SetError(111, kFalse);  // ERROR_BUFFER_OVERFLOW
  }
  std::memcpy(c.Ptr<char16_t>(0), name.c_str(), (name.size() + 1) * sizeof(char16_t));
  *size = static_cast<DWORD>(name.size());
  return kTrue;
}

} // namespace

void RegisterKernel32Misc(std::vector<Function>& out) {
  const std::string k = "kernel32.dll";
#define UB_FN(name) out.push_back({k, #name, &name})
  UB_FN(GetUserDefaultLCID); UB_FN(GetThreadLocale); UB_FN(IsValidLocale); UB_FN(EnumSystemLocalesW);
  UB_FN(GetLocaleInfoW); UB_FN(GetCPInfo); UB_FN(LCMapStringW); UB_FN(LCMapStringEx); UB_FN(CompareStringW);
  UB_FN(GetStringTypeW);
  UB_FN(GetUserDefaultLocaleName); UB_FN(IsValidLocaleName);
  out.push_back({k, "GetLocaleInfoEx", &GetLocaleInfoW});  // same layout from LCType on; en-US
  UB_FN(GetTimeZoneInformation); UB_FN(GetDynamicTimeZoneInformation); UB_FN(GetSystemTimes);
  UB_FN(FormatMessageW);
  UB_FN(ExitProcess); UB_FN(TerminateProcess); UB_FN(GetExitCodeProcess); UB_FN(K32GetProcessImageFileNameW);
  UB_FN(SetStdHandle); UB_FN(ReadConsoleW); UB_FN(SetThreadErrorMode); UB_FN(InterlockedFlushSList);
  UB_FN(LocalAlloc); UB_FN(LocalReAlloc); UB_FN(LocalFree); UB_FN(SetThreadDescription); UB_FN(GetThreadDescription);
  UB_FN(CopyFile2); UB_FN(GetTimeZoneInformationForYear); UB_FN(GetComputerNameW);
#undef UB_FN
}

} // namespace UnityBridge::Win32
