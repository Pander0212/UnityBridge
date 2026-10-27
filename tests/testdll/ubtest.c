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
IMPORT HANDLE CreateEventW(void*, BOOL, BOOL, const WCHAR*);
IMPORT BOOL SetEvent(HANDLE);
IMPORT BOOL ResetEvent(HANDLE);
IMPORT HANDLE CreateMutexW(void*, BOOL, const WCHAR*);
IMPORT BOOL ReleaseMutex(HANDLE);
IMPORT HANDLE CreateSemaphoreW(void*, int, int, const WCHAR*);
IMPORT BOOL ReleaseSemaphore(HANDLE, int, int*);
IMPORT DWORD WaitForSingleObjectEx(HANDLE, DWORD, BOOL);
IMPORT DWORD WaitForMultipleObjectsEx(DWORD, const HANDLE*, BOOL, DWORD, BOOL);
IMPORT BOOL CloseHandle(HANDLE);
IMPORT DWORD GetTempPathW(DWORD, WCHAR*);
IMPORT HANDLE CreateFileW(const WCHAR*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
IMPORT BOOL WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
IMPORT BOOL ReadFile(HANDLE, void*, DWORD, DWORD*, void*);
IMPORT BOOL SetFilePointerEx(HANDLE, LONGLONG, LONGLONG*, DWORD);
IMPORT BOOL GetFileSizeEx(HANDLE, LONGLONG*);
IMPORT BOOL GetFileAttributesExW(const WCHAR*, int, void*);
IMPORT HANDLE FindFirstFileExW(const WCHAR*, int, void*, int, void*, DWORD);
IMPORT BOOL FindNextFileW(HANDLE, void*);
IMPORT BOOL FindClose(HANDLE);
IMPORT HANDLE CreateFileMappingW(HANDLE, void*, DWORD, DWORD, DWORD, const WCHAR*);
IMPORT void* MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, unsigned long long);
IMPORT BOOL UnmapViewOfFile(void*);
IMPORT BOOL DeleteFileW(const WCHAR*);
IMPORT BOOL CreateDirectoryW(const WCHAR*, void*);
IMPORT BOOL RemoveDirectoryW(const WCHAR*);

IMPORT int LCMapStringW(DWORD, DWORD, const WCHAR*, int, WCHAR*, int);
IMPORT int CompareStringW(DWORD, DWORD, const WCHAR*, int, const WCHAR*, int);
IMPORT int GetLocaleInfoW(DWORD, DWORD, WCHAR*, int);
IMPORT void* InterlockedFlushSList(void*);
IMPORT BOOL EnumSystemLocalesW(BOOL (*)(WCHAR*), DWORD);
IMPORT DWORD FlsAlloc(void (*)(void*));
IMPORT BOOL FlsSetValue(DWORD, void*);
IMPORT BOOL FlsFree(DWORD);
IMPORT HANDLE CreateThread(void*, unsigned long long, DWORD (*)(void*), void*, DWORD, DWORD*);
IMPORT DWORD ResumeThread(HANDLE);
IMPORT BOOL GetExitCodeThread(HANDLE, DWORD*);
IMPORT DWORD GetCurrentThreadId(void);
IMPORT void Sleep(DWORD);

static int Append(WCHAR* to, int at, const char* text) {
  while (*text) to[at++] = (WCHAR)*text++;
  to[at] = 0;
  return at;
}

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

// Callbacks the mini Win32 calls back into (guest -> host -> guest). The FLS one also calls an
// import itself, one level deeper (guest -> host -> guest -> host).
static int g_locales;
static BOOL LocaleSeen(WCHAR* name) {
  if (name[0] == '0' && name[5] == '4' && name[6] == '0' && name[7] == '9' && name[8] == 0) ++g_locales;
  return 1;
}
static void* volatile g_flsFreed;
static void FlsFreed(void* value) {
  g_flsFreed = value;
  SetLastError(4321);
}

// Runs on its own thread: its last-error value and TLS slot are its own.
struct ThreadTest { DWORD slot; DWORD id; volatile int ran; };
static DWORD ThreadMain(void* parameter) {
  struct ThreadTest* t = (struct ThreadTest*)parameter;
  SetLastError(99);
  t->id = GetCurrentThreadId();
  t->ran = GetLastError() == 99 && TlsGetValue(t->slot) == 0 && TlsSetValue(t->slot, (void*)7);  // TlsGetValue clears it
  return 77;
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

  // Waitable objects (single thread: signalled states and timeouts).
  {
    int good = 1;
    HANDLE autoEvent = CreateEventW(0, 0, 0, 0), manualEvent = CreateEventW(0, 1, 0, 0);
    good &= WaitForSingleObjectEx(autoEvent, 0, 0) == 0x102;              // not set: timeout
    SetEvent(autoEvent);
    good &= WaitForSingleObjectEx(autoEvent, 0, 0) == 0;                  // set: taken
    good &= WaitForSingleObjectEx(autoEvent, 0, 0) == 0x102;              // auto-reset
    SetEvent(manualEvent);
    good &= WaitForSingleObjectEx(manualEvent, 0, 0) == 0 && WaitForSingleObjectEx(manualEvent, 0, 0) == 0;
    ResetEvent(manualEvent);
    good &= WaitForSingleObjectEx(manualEvent, 0, 0) == 0x102;

    HANDLE mutex = CreateMutexW(0, 1, 0);                                 // owned by us
    good &= WaitForSingleObjectEx(mutex, 0, 0) == 0;                      // recursive
    good &= ReleaseMutex(mutex) && ReleaseMutex(mutex) && !ReleaseMutex(mutex);

    int previous = -1;
    HANDLE sem = CreateSemaphoreW(0, 1, 2, 0);
    good &= ReleaseSemaphore(sem, 1, &previous) && previous == 1 && !ReleaseSemaphore(sem, 1, 0);  // max 2

    HANDLE set[2] = {manualEvent, sem};
    good &= WaitForMultipleObjectsEx(2, set, 0, 0, 0) == 1;               // any: the semaphore
    good &= WaitForMultipleObjectsEx(2, set, 1, 5, 0) == 0x102;           // all: event not set
    SetEvent(manualEvent);
    good &= WaitForMultipleObjectsEx(2, set, 1, 0, 0) == 0;               // all
    good &= WaitForSingleObjectEx(sem, 0, 0) == 0x102;                    // both units taken

    CloseHandle(autoEvent); CloseHandle(manualEvent); CloseHandle(mutex); CloseHandle(sem);
    good &= WaitForSingleObjectEx(autoEvent, 0, 0) == 0xffffffffu;        // closed: WAIT_FAILED
    if (good) ok |= 1 << 17;
  }

  // Files: <temp>\ubtest\data.bin, written, read back, listed, mapped, removed.
  {
    int good = 1;
    WCHAR dir[300], file[300], pattern[300];
    int n = (int)GetTempPathW(260, dir);
    good &= n > 3 && dir[1] == ':' && dir[n - 1] == '\\';
    n = Append(dir, n, "ubtest");
    for (int i = 0; i <= n; ++i) file[i] = pattern[i] = dir[i];
    CreateDirectoryW(dir, 0);
    Append(file, n, "\\data.bin");
    Append(pattern, n, "\\*.BIN");  // matching ignores case, as on Windows

    HANDLE h = CreateFileW(file, 0xC0000000u /* read|write */, 0, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
    DWORD written = 0, read = 0;
    char buffer[16] = {0};
    LONGLONG size = 0, position = -1;
    good &= h != (HANDLE)-1;
    good &= WriteFile(h, "0123456789", 10, &written, 0) && written == 10;
    good &= SetFilePointerEx(h, 4, &position, 0 /* FILE_BEGIN */) && position == 4;
    good &= ReadFile(h, buffer, 16, &read, 0) && read == 6 && buffer[0] == '4';  // short read at the end
    good &= GetFileSizeEx(h, &size) && size == 10;
    CloseHandle(h);

    unsigned char attributes[36];
    good &= GetFileAttributesExW(file, 0, attributes) && ((DWORD*)attributes)[8] == 10;  // nFileSizeLow

    unsigned char found[592];
    HANDLE f = FindFirstFileExW(pattern, 0, found, 0, 0, 0);
    WCHAR* foundName = (WCHAR*)(found + 44);
    good &= f != (HANDLE)-1 && foundName[0] == 'd' && foundName[8] == 0 && !FindNextFileW(f, found) && GetLastError() == 18;
    FindClose(f);

    h = CreateFileW(file, 0x80000000u, 1, 0, 3 /* OPEN_EXISTING */, 0x80, 0);
    HANDLE mapping = CreateFileMappingW(h, 0, 0x02 /* PAGE_READONLY */, 0, 0, 0);
    const char* view = (const char*)MapViewOfFile(mapping, 0x4 /* FILE_MAP_READ */, 0, 0, 0);
    good &= view && view[9] == '9';
    UnmapViewOfFile((void*)view);
    CloseHandle(mapping);
    CloseHandle(h);

    good &= CreateFileW(file, 0x80000000u, 0, 0, 1 /* CREATE_NEW */, 0x80, 0) == (HANDLE)-1 && GetLastError() == 80;
    good &= DeleteFileW(file) && !DeleteFileW(file) && GetLastError() == 2;
    good &= RemoveDirectoryW(dir);
    if (good) ok |= 1 << 18;
  }

  // Locale: case mapping (incl. Latin Extended-A pairs of both parities), comparison, en-US.
  {
    int good = 1;
    static const WCHAR in[] = {'a', 'b', 0xe4, 0x101, 0x13a, 0x17e, 0};   // a b ä ā ĺ ž
    static const WCHAR up[] = {'A', 'B', 0xc4, 0x100, 0x139, 0x17d, 0};   // A B Ä Ā Ĺ Ž
    WCHAR out[8] = {0};
    good &= LCMapStringW(0x409, 0x200 /* LCMAP_UPPERCASE */, in, -1, out, 8) == 7;
    for (int i = 0; i < 7; ++i) good &= out[i] == up[i];
    good &= LCMapStringW(0x409, 0x100 /* LCMAP_LOWERCASE */, up, -1, out, 8) == 7;
    for (int i = 0; i < 7; ++i) good &= out[i] == in[i];
    static const WCHAR hello1[] = {'H','e','l','l','o',0}, hello2[] = {'h','E','L','L','O',0};
    good &= CompareStringW(0x409, 0x1 /* NORM_IGNORECASE */, hello1, -1, hello2, -1) == 2;
    good &= CompareStringW(0x409, 0, hello1, -1, hello2, -1) == 1;     // 'H' < 'h'
    WCHAR name[16];
    good &= GetLocaleInfoW(0x409, 0x5c /* LOCALE_SNAME */, name, 16) == 6 && name[0] == 'e' && name[2] == '-';
    if (good) ok |= 1 << 19;
  }

  // InterlockedFlushSList on a hand-built x64 SLIST_HEADER with one entry.
  {
    static __declspec(align(16)) unsigned long long header[2];
    static __declspec(align(16)) unsigned long long entry[2];
    header[0] = (0x5ull << 16) | 1;                          // Sequence 5, Depth 1
    header[1] = (unsigned long long)(void*)entry;            // NextEntry (address, low 4 bits zero)
    void* first = InterlockedFlushSList(header);
    if (first == entry && header[1] == 0 && (header[0] & 0xffff) == 0 && InterlockedFlushSList(header) == 0) ok |= 1 << 20;
  }

  // Callbacks: EnumSystemLocalesW reports en-US; FlsFree runs the slot's callback with its value.
  if (EnumSystemLocalesW(LocaleSeen, 2 /* LCID_SUPPORTED */) && g_locales == 1) ok |= 1 << 21;
  {
    DWORD slot = FlsAlloc(FlsFreed);
    SetLastError(0);
    if (slot != 0xffffffffu && FlsSetValue(slot, (void*)0x5678) && FlsFree(slot) && g_flsFreed == (void*)0x5678 &&
        GetLastError() == 4321) ok |= 1 << 22;
  }

  // Threads: CreateThread runs the start routine on a new thread with its own TLS and last error;
  // the handle becomes signalled with the exit code. CREATE_SUSPENDED waits for ResumeThread.
  {
    static struct ThreadTest t;
    DWORD id = 0, code = 0;
    t.slot = TlsAlloc();
    TlsSetValue(t.slot, (void*)3);
    SetLastError(5);
    HANDLE h = CreateThread(0, 0, ThreadMain, &t, 0, &id);
    if (h && WaitForSingleObjectEx(h, 0xffffffffu, 0) == 0 && GetExitCodeThread(h, &code) && code == 77 && t.ran &&
        id != 0 && t.id == id && id != GetCurrentThreadId() && GetLastError() == 5 && TlsGetValue(t.slot) == (void*)3)
      ok |= 1 << 23;
    if (h) CloseHandle(h);

    t.ran = 0;
    h = CreateThread(0, 0, ThreadMain, &t, 0x4 /* CREATE_SUSPENDED */, &id);
    Sleep(20);
    BOOL heldBack = h && GetExitCodeThread(h, &code) && code == 259 /* STILL_ACTIVE */ && !t.ran;
    if (heldBack && ResumeThread(h) == 1 && WaitForSingleObjectEx(h, 5000, 0) == 0 && t.ran) ok |= 1 << 24;
    if (h) CloseHandle(h);
  }

  return ok;
}

// Returns through xmm0; arguments in xmm0/xmm1.
__declspec(dllexport) double ub_test_float(double a, double b) { return a * b + 0.5; }

// Calls an import that UnityBridge does not implement: must return 0, not crash.
__declspec(dllexport) DWORD ub_test_missing(void) { return UnityBridgeTestMissingFunction(5); }
