// SPDX-License-Identifier: GPL-3.0-or-later
// A Windows x86-64 DLL that exercises UnityBridge's mini Win32 the way real code does: compiled
// for x86_64-pc-windows-msvc, calling KERNEL32 through its import table. No CRT, no windows.h.
// Each check sets one bit of ub_test_all's result.

typedef unsigned int DWORD;
typedef int BOOL;
typedef unsigned short WCHAR;
typedef void* HANDLE;
typedef long long LONGLONG;

#define IMPORT __declspec(dllimport)
IMPORT DWORD GetLastError(void);
IMPORT void SetLastError(DWORD);
IMPORT HANDLE GetProcessHeap(void);
IMPORT void* HeapAlloc(HANDLE, DWORD, unsigned long long);
IMPORT BOOL HeapFree(HANDLE, DWORD, void*);
IMPORT DWORD TlsAlloc(void);
IMPORT BOOL TlsSetValue(DWORD, void*);
IMPORT void* TlsGetValue(DWORD);
IMPORT void InitializeCriticalSection(void*);
IMPORT void EnterCriticalSection(void*);
IMPORT void LeaveCriticalSection(void*);
IMPORT int MultiByteToWideChar(DWORD, DWORD, const char*, int, WCHAR*, int);
IMPORT int WideCharToMultiByte(DWORD, DWORD, const WCHAR*, int, char*, int, const char*, BOOL*);
IMPORT BOOL QueryPerformanceCounter(LONGLONG*);
IMPORT BOOL QueryPerformanceFrequency(LONGLONG*);
IMPORT HANDLE GetModuleHandleW(const WCHAR*);
IMPORT void* GetProcAddress(HANDLE, const char*);
IMPORT void* VirtualAlloc(void*, unsigned long long, DWORD, DWORD);
IMPORT BOOL VirtualFree(void*, unsigned long long, DWORD);
IMPORT DWORD UnityBridgeTestMissingFunction(DWORD);  // deliberately not implemented

// OLEAUT32 (imported by ordinal, see oleaut32.def), ADVAPI32, SHELL32.
typedef struct { DWORD cElements; int lLbound; } SAFEARRAYBOUND;
IMPORT WCHAR* SysAllocString(const WCHAR*);
IMPORT void SysFreeString(WCHAR*);
IMPORT unsigned SysStringLen(WCHAR*);
IMPORT void* SafeArrayCreate(unsigned short, unsigned, SAFEARRAYBOUND*);
IMPORT int SafeArrayDestroy(void*);
IMPORT int SafeArrayGetUBound(void*, unsigned, int*);
IMPORT int SafeArrayGetLBound(void*, unsigned, int*);
IMPORT int SafeArrayAccessData(void*, void**);
IMPORT int SafeArrayUnaccessData(void*);
IMPORT int SafeArrayGetVartype(void*, unsigned short*);
IMPORT BOOL CryptAcquireContextW(unsigned long long*, const WCHAR*, const WCHAR*, DWORD, DWORD);
IMPORT BOOL CryptGenRandom(unsigned long long, DWORD, unsigned char*);
IMPORT BOOL CryptReleaseContext(unsigned long long, DWORD);
IMPORT int SHGetFolderPathW(void*, int, void*, DWORD, WCHAR*);

// baselib.dll exports C++ functions; the declarations take their MSVC-decorated names.
IMPORT void* SemCreate(void*) __asm__("?Baselib_SystemSemaphore_CreateInplace@il2cpp_baselib@@YA?AUBaselib_SystemSemaphore_Handle@1@PEAX@Z");
IMPORT void SemRelease(void*, unsigned) __asm__("?Baselib_SystemSemaphore_Release@il2cpp_baselib@@YAXUBaselib_SystemSemaphore_Handle@1@I@Z");
IMPORT _Bool SemTryAcquire(void*) __asm__("?Baselib_SystemSemaphore_TryAcquire@il2cpp_baselib@@YA_NUBaselib_SystemSemaphore_Handle@1@@Z");
IMPORT _Bool SemTryTimed(void*, unsigned) __asm__("?Baselib_SystemSemaphore_TryTimedAcquire@il2cpp_baselib@@YA_NUBaselib_SystemSemaphore_Handle@1@I@Z");
IMPORT void SemFree(void*) __asm__("?Baselib_SystemSemaphore_FreeInplace@il2cpp_baselib@@YAXUBaselib_SystemSemaphore_Handle@1@@Z");
IMPORT long long BaselibThreadId(void) __asm__("?Baselib_Thread_GetCurrentThreadId@il2cpp_baselib@@YA_JXZ");

int _fltused = 0;  // MSVC's marker that floating point is used; normally from the CRT

static unsigned char g_cs[40];
// An absolute pointer in data: the linker emits a DIR64 base relocation for it.
static const char g_text[] = "relocated";
static const char* volatile g_textPointer = g_text;

// noinline + many live values: forces the compiler to keep data in rdi/rsi/rbx and xmm6+ across
// imported calls, which the host bridge must preserve (Windows callee-saved registers).
__declspec(noinline) static int KeepAlive(int a, int b, double x, double y) {
  int sum = 0;
  for (int i = 0; i < 4; ++i) {
    SetLastError(1000 + i);
    sum += a * i + b;
    x = x * 1.5 + y;
  }
  return sum + (int)x;
}

__declspec(dllexport) int ub_test_all(void) {
  int ok = 0;

  SetLastError(1234);
  if (GetLastError() == 1234) ok |= 1 << 0;

  HANDLE heap = GetProcessHeap();
  unsigned char* p = (unsigned char*)HeapAlloc(heap, 0x8 /* HEAP_ZERO_MEMORY */, 64);
  if (p && p[0] == 0 && p[63] == 0) ok |= 1 << 1;
  if (p) { p[10] = 42; if (HeapFree(heap, 0, p)) ok |= 1 << 2; }

  DWORD slot = TlsAlloc();
  if (slot != 0xffffffffu && TlsSetValue(slot, (void*)0x1234) && TlsGetValue(slot) == (void*)0x1234) ok |= 1 << 3;

  InitializeCriticalSection(g_cs);
  EnterCriticalSection(g_cs);
  EnterCriticalSection(g_cs);  // recursive
  LeaveCriticalSection(g_cs);
  LeaveCriticalSection(g_cs);
  ok |= 1 << 4;

  // 6 arguments: two on the stack above the shadow space.
  WCHAR wide[16];
  int n = MultiByteToWideChar(65001, 0, "h\xc3\xa4llo", -1, wide, 16);  // "hällo"
  if (n == 6 && wide[1] == 0xe4 && wide[5] == 0) ok |= 1 << 5;
  // 8 arguments.
  char narrow[16];
  BOOL usedDefault = 1;
  n = WideCharToMultiByte(65001, 0, wide, -1, narrow, 16, 0, &usedDefault);
  if (n == 7 && (unsigned char)narrow[1] == 0xc3 && narrow[6] == 0 && usedDefault == 0) ok |= 1 << 6;

  LONGLONG freq = 0, t0 = 0, t1 = 0;
  if (QueryPerformanceFrequency(&freq) && freq == 10000000 && QueryPerformanceCounter(&t0) &&
      QueryPerformanceCounter(&t1) && t1 >= t0) ok |= 1 << 7;

  // GetProcAddress on a built-in DLL hands out a working stub.
  static const WCHAR kernel32[] = {'k','e','r','n','e','l','3','2','.','d','l','l',0};
  HANDLE k = GetModuleHandleW(kernel32);
  DWORD (*getLastError)(void) = (DWORD (*)(void))GetProcAddress(k, "GetLastError");
  SetLastError(77);
  if (k && getLastError && getLastError() == 77) ok |= 1 << 8;

  void* mem = VirtualAlloc(0, 1 << 20, 0x3000 /* MEM_COMMIT|MEM_RESERVE */, 0x04 /* PAGE_READWRITE */);
  if (mem) { ((volatile char*)mem)[12345] = 1; if (VirtualFree(mem, 0, 0x8000 /* MEM_RELEASE */)) ok |= 1 << 9; }

  // sum = 3*(0+1+2+3) + 4*4 = 34; x: 1.0 -> 1.75 -> 2.875 -> 4.5625 -> 7.09375, (int) 7.
  if (KeepAlive(3, 4, 1.0, 0.25) == 41) ok |= 1 << 10;

  if (g_textPointer == g_text && g_textPointer[0] == 'r') ok |= 1 << 11;

  // BSTR: length prefix and terminator.
  static const WCHAR hello[] = {'h','e','l','l','o',0};
  WCHAR* bstr = SysAllocString(hello);
  if (bstr && SysStringLen(bstr) == 5 && ((DWORD*)bstr)[-1] == 10 && bstr[5] == 0) ok |= 1 << 12;
  SysFreeString(bstr);

  // SAFEARRAY of int, 2 x 3 with lower bounds 1 and -1.
  SAFEARRAYBOUND bounds[2] = {{2, 1}, {3, -1}};
  void* sa = SafeArrayCreate(3 /* VT_I4 */, 2, bounds);
  int lo1 = 0, hi1 = 0, lo2 = 0, hi2 = 0;
  unsigned short vt = 0;
  int* data = 0;
  if (sa && SafeArrayGetLBound(sa, 1, &lo1) == 0 && SafeArrayGetUBound(sa, 1, &hi1) == 0 &&
      SafeArrayGetLBound(sa, 2, &lo2) == 0 && SafeArrayGetUBound(sa, 2, &hi2) == 0 &&
      lo1 == 1 && hi1 == 2 && lo2 == -1 && hi2 == 1 && SafeArrayGetVartype(sa, &vt) == 0 && vt == 3 &&
      SafeArrayAccessData(sa, (void**)&data) == 0 && data) {
    data[5] = 99;  // 6 elements, zeroed
    if (data[0] == 0 && SafeArrayUnaccessData(sa) == 0 && SafeArrayGetUBound(sa, 3, &hi1) != 0) ok |= 1 << 13;
  }
  SafeArrayDestroy(sa);

  unsigned long long provider = 0;
  unsigned char random[32] = {0};
  int nonzero = 0;
  if (CryptAcquireContextW(&provider, 0, 0, 1, 0xf0000000u) && CryptGenRandom(provider, 32, random)) {
    for (int i = 0; i < 32; ++i) nonzero |= random[i];
    if (nonzero && CryptReleaseContext(provider, 0)) ok |= 1 << 14;
  }

  WCHAR folder[260];
  if (SHGetFolderPathW(0, 0x1c /* CSIDL_LOCAL_APPDATA */, 0, 0, folder) == 0 && folder[0] == 'C' && folder[1] == ':') ok |= 1 << 15;

  // baselib semaphore: two releases allow exactly two acquires; the third times out.
  static unsigned char semMemory[64];
  void* sem = SemCreate(semMemory);
  SemRelease(sem, 2);
  if (sem == semMemory && SemTryAcquire(sem) && SemTryAcquire(sem) && !SemTryAcquire(sem) && !SemTryTimed(sem, 10) &&
      BaselibThreadId() != 0) ok |= 1 << 16;
  SemFree(sem);

  return ok;
}

// Returns through xmm0; arguments in xmm0/xmm1.
__declspec(dllexport) double ub_test_float(double a, double b) { return a * b + 0.5; }

// Calls an import that UnityBridge does not implement: must return 0, not crash.
__declspec(dllexport) DWORD ub_test_missing(void) { return UnityBridgeTestMissingFunction(5); }
