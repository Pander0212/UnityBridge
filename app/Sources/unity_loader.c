// SPDX-License-Identifier: GPL-3.0-or-later
// M3d: Android's Unity engine (libmain.so + libunity.so, ARM64) under Husk's Android runtime, next to
// the game's GameAssembly.dll under FEXCore. Built with Husk's translation layer (fetched by
// scripts/fetch-husk.sh, compiled into this app unmodified).
//
// Unity's Android libraries are not redistributable and never ship with the app: the user copies
// libmain.so, libunity.so and classes.dex from Unity's Android player for the game's exact version
// (2021.3.18f1 for Fellworld: Editor/Data/PlaybackEngines/AndroidPlayer/Variations/il2cpp/Release,
// Libs/arm64-v8a and Classes) into Files > On My iPhone > UB Probe > Unity. From them the app writes
// a minimal APK (stored zip: classes.dex, the two libraries under lib/arm64-v8a/) next to them,
// which is what Husk's Unity driver starts from.
//
// libil2cpp.so is ours: dlopen("libil2cpp.so") from Android code returns a handle of our own, and
// dlsym on it returns the il2cpp bridge's ARM64 entry points into GameAssembly.dll (ub_il2cpp_symbol).
// Everything else goes on to Husk's bionic. The interposition uses Husk's tl_ld_interpose, so every
// import of dlopen/dlsym/android_dlopen_ext the Android libraries make binds to the functions here.
//
// This step runs the Unity driver's startup up to NativeLoader.load (libmain's JNI_OnLoad, libunity
// loaded, relocated, initialised, its JNI_OnLoad): no graphics yet, and not the engine's own start
// (initJni/nativeRender), which would initialise il2cpp a second time.
#define _GNU_SOURCE  // Dl_info/dladdr on glibc (the host syntax check); Darwin has them anyway
#include "husk-tl-bionic.h"
#include "husk-tl-egl.h"
#include "husk-tl-jni.h"
#include "husk-tl-ld.h"
#include "husk-tl-unity.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <zlib.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

// Husk's runtime (husk-tl-jni-loop.c), not in its headers: call a method on a java.lang.reflect.Proxy
// the engine or C# built, through Unity's native proxy bridge.
jobj *tl_proxy_call(jobj *proxy, const char *iface, const char *name, const char *sig, jobj **args, int nargs);

// engine/unity_host.cpp
void *ub_il2cpp_symbol(const char *name);
void ub_unity_thread_enter(void);
void ub_unity_thread_leave(void);
void ub_il2cpp_engine_report(char *out, size_t capacity);
void ub_memory_dump(const char *tag);

// ---- report -----------------------------------------------------------------------------------

static char *g_out;
static size_t g_cap, g_len;

static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void line(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "UB_UNITY %s\n", buf);
    if (g_out && g_len + 1 < g_cap) {
        int n = snprintf(g_out + g_len, g_cap - g_len, "%s\n", buf);
        if (n > 0) g_len += (size_t)n < g_cap - g_len ? (size_t)n : g_cap - g_len - 1;
    }
}

// ---- the virtual libil2cpp.so ------------------------------------------------------------------

typedef void *(*dlopen_fn)(const char *, int);
typedef void *(*dlopen_ext_fn)(const char *, int, const void *);
typedef void *(*dlsym_fn)(void *, const char *);

static dlopen_fn g_bionic_dlopen;
static dlopen_ext_fn g_bionic_dlopen_ext;
static dlsym_fn g_bionic_dlsym;
static int g_il2cpp_handle;  // its address is the handle
static int g_il2cpp_opens, g_il2cpp_syms, g_il2cpp_missing;

static bool is_il2cpp(const char *path)
{
    if (!path) return false;
    const char *base = strrchr(path, '/');
    return !strcmp(base ? base + 1 : path, "libil2cpp.so");
}

static void *ub_dlopen(const char *path, int flags)
{
    if (is_il2cpp(path)) {
        if (g_il2cpp_opens++ < 4) line("dlopen(%s): the il2cpp bridge to GameAssembly.dll", path);
        return &g_il2cpp_handle;
    }
    return g_bionic_dlopen ? g_bionic_dlopen(path, flags) : NULL;
}

static void *ub_android_dlopen_ext(const char *path, int flags, const void *info)
{
    if (is_il2cpp(path)) return ub_dlopen(path, flags);
    return g_bionic_dlopen_ext ? g_bionic_dlopen_ext(path, flags, info) : NULL;
}

static void *ub_dlsym(void *handle, const char *name)
{
    if (handle == &g_il2cpp_handle) {
        void *fn = ub_il2cpp_symbol(name);
        ++g_il2cpp_syms;
        if (!fn && g_il2cpp_missing++ < 16) line("dlsym(libil2cpp.so, %s): not exported by GameAssembly.dll", name);
        return fn;
    }
    void *r = g_bionic_dlsym ? g_bionic_dlsym(handle, name) : NULL;
    // A global lookup of an il2cpp_* name: libil2cpp.so is not loaded in Husk's namespace, so answer it here.
    if (!r && name && !strncmp(name, "il2cpp_", 7) && handle == NULL) r = ub_il2cpp_symbol(name);
    return r;
}

// ---- the synthetic APK -------------------------------------------------------------------------
// A stored (uncompressed) zip, written by streaming each file twice (CRC, then data), so files of any
// size up to the zip32 limits work. Each entry's data starts on a 16 KiB boundary (padding in the
// local header's extra field, Android's 0xd935 tag), as zipalign -P 16 lays out an APK.

typedef struct {
    char name[600];   // entry name in the APK
    char path[1100];  // source file
    uint64_t size;
    long mtime;
    uint32_t crc;
    uint64_t offset;  // of the local header
    int64_t patch_at;  // file offset of a 4-byte rewrite applied while copying, or -1
    uint8_t patch[4];
} apk_entry;

static apk_entry *g_entries;
static int g_nentries, g_capentries;

static bool add_entry(const char *name, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (g_nentries == g_capentries) {
        g_capentries = g_capentries ? g_capentries * 2 : 64;
        g_entries = realloc(g_entries, sizeof(apk_entry) * (size_t)g_capentries);
    }
    apk_entry *e = &g_entries[g_nentries++];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->size = (uint64_t)st.st_size;
    e->mtime = (long)st.st_mtime;
    e->patch_at = -1;
    return true;
}

// Unity's serialized files (globalgamemanagers, level*, *.assets, ...) record the build target they
// were made for, and the Android player refuses another one ("created for another build target",
// 19 = StandaloneWindows64). The field follows the Unity version string at the start of the
// metadata. If `path` is such a file built for a desktop target, the entry's copy in the APK says
// Android (13) instead; the game's own file is not changed. Header (big-endian): metadataSize,
// fileSize, version, dataOffset; from version 9 an endianness byte + 3 reserved; from 22 another
// 4 + 8 + 8 + 8 bytes. Then the version string and the int32 target (in the file's endianness).
static void plan_platform_patch(apk_entry *e)
{
    FILE *f = fopen(e->path, "rb");
    if (!f) return;
    uint8_t h[160];
    const size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n < 48) return;
    const uint32_t version = (uint32_t)h[8] << 24 | (uint32_t)h[9] << 16 | (uint32_t)h[10] << 8 | h[11];
    if (version < 9 || version > 30) return;
    const size_t header = version >= 22 ? 48 : 20;
    const bool big = h[16] != 0;
    size_t end = header;
    while (end < n && h[end] != 0) {
        if (!((h[end] >= '0' && h[end] <= '9') || h[end] == '.' || (h[end] >= 'a' && h[end] <= 'z'))) return;
        ++end;
    }
    // A Unity version like "2021.3.18f1": digits first, a dot, terminated in the bytes read.
    if (end >= n || end + 5 > n || end - header < 5 || h[header] < '1' || h[header] > '9' || !memchr(h + header, '.', end - header)) return;
    const size_t at = end + 1;
    const uint8_t *t = h + at;
    const uint32_t target = big ? ((uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3])
                                : ((uint32_t)t[3] << 24 | (uint32_t)t[2] << 16 | (uint32_t)t[1] << 8 | t[0]);
    // Desktop targets: 2 StandaloneOSX, 5 StandaloneWindows, 19 StandaloneWindows64, 24 StandaloneLinux64.
    if (target != 2 && target != 5 && target != 19 && target != 24) return;
    const uint32_t android = 13;
    for (int i = 0; i < 4; i++) e->patch[i] = (uint8_t)(big ? android >> (24 - 8 * i) : android >> (8 * i));
    e->patch_at = (int64_t)at;
    static int logged;
    if (logged++ < 3) line("  %s: build target %u -> 13 (Android) in the APK copy", e->name, target);
}

// Applies an entry's planned rewrite to the chunk [pos, pos + n) of its file.
static void apply_patch(const apk_entry *e, uint8_t *buf, uint64_t pos, size_t n)
{
    if (e->patch_at < 0) return;
    for (int i = 0; i < 4; i++) {
        const uint64_t at = (uint64_t)e->patch_at + (uint64_t)i;
        if (at >= pos && at < pos + n) buf[at - pos] = e->patch[i];
    }
}

// Every file under `dir` as `prefix/<relative path>`, skipping top-level folders in `skip`.
static void add_tree(const char *dir, const char *prefix, const char *const *skip, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        bool skipped = false;
        for (int i = 0; depth == 0 && skip && skip[i]; i++) if (!strcmp(de->d_name, skip[i])) skipped = true;
        if (skipped) continue;
        char path[1100], name[600];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        snprintf(name, sizeof(name), "%s/%s", prefix, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) add_tree(path, name, skip, depth + 1);
        else add_entry(name, path);
    }
    closedir(d);
}

static void put16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc(v >> 8, f); }
static void put32(FILE *f, uint32_t v) { put16(f, (uint16_t)v); put16(f, (uint16_t)(v >> 16)); }

static bool file_crc(const apk_entry *e, uint32_t *crc)
{
    FILE *f = fopen(e->path, "rb");
    if (!f) return false;
    static uint8_t buf[1 << 20];
    uLong c = crc32(0, NULL, 0);
    size_t n;
    uint64_t pos = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        apply_patch(e, buf, pos, n);
        c = crc32(c, buf, (uInt)n);
        pos += n;
    }
    const bool ok = !ferror(f);
    fclose(f);
    *crc = (uint32_t)c;
    return ok;
}

static bool copy_file(FILE *out, const apk_entry *e)
{
    FILE *f = fopen(e->path, "rb");
    if (!f) return false;
    static uint8_t buf[1 << 20];
    size_t n;
    bool ok = true;
    uint64_t pos = 0;
    while (ok && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        apply_patch(e, buf, pos, n);
        ok = fwrite(buf, 1, n, out) == n;
        pos += n;
    }
    ok = ok && !ferror(f);
    fclose(f);
    return ok;
}

static bool write_apk(const char *apk)
{
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", apk);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = true;
    for (int i = 0; i < g_nentries && ok; i++) {
        apk_entry *e = &g_entries[i];
        ok = file_crc(e, &e->crc);
        e->offset = (uint64_t)ftello(f);
        if (e->size >= 0xffffffffull || e->offset >= 0xffffffffull) { line("%s: past the zip32 limits (4 GiB)", e->name); ok = false; break; }
        const size_t nlen = strlen(e->name);
        size_t pad = (16384 - (e->offset + 30 + nlen) % 16384) % 16384;
        if (pad && pad < 4) pad += 16384;  // room for the extra field's own header
        put32(f, 0x04034b50); put16(f, 10); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
        put32(f, e->crc); put32(f, (uint32_t)e->size); put32(f, (uint32_t)e->size);
        put16(f, (uint16_t)nlen); put16(f, (uint16_t)pad);
        fwrite(e->name, 1, nlen, f);
        if (pad) {
            put16(f, 0xd935); put16(f, (uint16_t)(pad - 4));
            for (size_t k = 4; k < pad; k++) fputc(0, f);
        }
        ok = ok && copy_file(f, e);
    }
    const uint64_t cd = (uint64_t)ftello(f);
    for (int i = 0; i < g_nentries && ok; i++) {
        const apk_entry *e = &g_entries[i];
        const size_t nlen = strlen(e->name);
        put32(f, 0x02014b50); put16(f, 20); put16(f, 10); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
        put32(f, e->crc); put32(f, (uint32_t)e->size); put32(f, (uint32_t)e->size);
        put16(f, (uint16_t)nlen); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0); put32(f, 0);
        put32(f, (uint32_t)e->offset);
        fwrite(e->name, 1, nlen, f);
    }
    const uint64_t end = (uint64_t)ftello(f);
    if (ok && (g_nentries > 0xffff || end >= 0xffffffffull)) { line("APK past the zip32 limits"); ok = false; }
    put32(f, 0x06054b50); put16(f, 0); put16(f, 0); put16(f, (uint16_t)g_nentries); put16(f, (uint16_t)g_nentries);
    put32(f, (uint32_t)(end - cd)); put32(f, (uint32_t)cd); put16(f, 0);
    ok = ok && !ferror(f);
    fclose(f);
    if (!ok) { remove(tmp); return false; }
    return rename(tmp, apk) == 0;
}

// The inputs as one line per entry; an APK whose recorded inputs match is not written again.
static char *inputs_text(void)
{
    size_t cap = 256 + (size_t)g_nentries * 1800, len = 0;
    char *t = malloc(cap);
    len = (size_t)snprintf(t, cap, "apk-format 2\n");  // bump when the APK's contents change for the same inputs
    for (int i = 0; i < g_nentries; i++)
        len += (size_t)snprintf(t + len, cap - len, "%s\t%s\t%llu\t%ld\t%lld\n", g_entries[i].name, g_entries[i].path,
                                (unsigned long long)g_entries[i].size, g_entries[i].mtime, (long long)g_entries[i].patch_at);
    return t;
}

// Writes <unity_dir>/base.apk: classes.dex (Unity's Java side: UnityPlayer, NativeLoader, ..., which
// Husk's Dalvik runs), libmain.so and libunity.so under lib/arm64-v8a/, and, when game_data is given,
// that <Game>_Data folder as assets/bin/Data/ (its il2cpp_data as Managed/, the Android layout;
// without the Windows Plugins). A file named "unity default resources" in
// unity_dir (from Unity's Android player) takes the place of the game's Windows copy.
static bool make_apk(const char *unity_dir, const char *game_data, char *apk, size_t apkcap)
{
    g_nentries = 0;
    snprintf(apk, apkcap, "%s/base.apk", unity_dir);
    char p[1100];
    snprintf(p, sizeof(p), "%s/classes.dex", unity_dir);
    if (!add_entry("classes.dex", p)) line("  classes.dex: not there (Unity's Java classes; JNI_OnLoad will not find NativeLoader)");
    static const char *const libs[] = {"libmain.so", "libunity.so"};
    for (int i = 0; i < 2; i++) {
        char name[64];
        snprintf(p, sizeof(p), "%s/%s", unity_dir, libs[i]);
        snprintf(name, sizeof(name), "lib/arm64-v8a/%s", libs[i]);
        if (!add_entry(name, p)) { line("missing %s", p); return false; }
    }
    if (game_data) {
        static const char *const skip[] = {"il2cpp_data", "Plugins", NULL};
        const int before = g_nentries;
        add_tree(game_data, "assets/bin/Data", skip, 0);
        // il2cpp's data (Metadata/, Resources/): <Game>_Data/il2cpp_data on Windows, assets/bin/Data/Managed
        // on Android, which the engine copies to files/il2cpp before it starts il2cpp ("Not enough
        // storage space to install required resources" when it is missing).
        snprintf(p, sizeof(p), "%s/il2cpp_data", game_data);
        add_tree(p, "assets/bin/Data/Managed", NULL, 0);
        // An Android build carries the app's GUID here; the engine logs an error without it.
        snprintf(p, sizeof(p), "%s/unity_app_guid", unity_dir);
        struct stat sg;
        if (stat(p, &sg) != 0) {
            FILE *g = fopen(p, "wb");
            if (g) { fputs("0c2a4f1e7b3d4e5f9a8b6c7d1e2f3a4b", g); fclose(g); }
        }
        add_entry("assets/bin/Data/unity_app_guid", p);
        // The engine's built-in resources (error shader, default materials, fonts): on Windows in
        // <Game>_Data/Resources/, on Android at assets/bin/Data/ itself. Unity's Android copy carries
        // the GLES3 shader programs; the game's Windows copy only D3D11 ones, so the engine loads it
        // but cannot draw with its shaders.
        static const char *const kDefaultRes = "assets/bin/Data/unity default resources";
        snprintf(p, sizeof(p), "%s/unity default resources", unity_dir);
        if (add_entry(kDefaultRes, p)) {
            line("  unity default resources: Unity's Android copy");
        } else {
            snprintf(p, sizeof(p), "%s/Resources/unity default resources", game_data);
            if (add_entry(kDefaultRes, p))
                line("  unity default resources: the game's Windows copy (D3D11 shaders only; put Unity's Android "
                     "copy into Documents/Unity for GLES3)");
            else
                line("  unity default resources: none (the engine will not find its error shader)");
        }
        int patched = 0;
        for (int i = before; i < g_nentries; i++) {
            plan_platform_patch(&g_entries[i]);
            if (g_entries[i].patch_at >= 0) ++patched;
        }
        line("  %d serialized files rewritten to build target Android (13)", patched);
        uint64_t bytes = 0;
        for (int i = before; i < g_nentries; i++) bytes += g_entries[i].size;
        line("  game data: %d files, %llu MiB as assets/bin/Data", g_nentries - before, (unsigned long long)(bytes >> 20));
    }
    char *want = inputs_text();
    char stamp[1110];
    snprintf(stamp, sizeof(stamp), "%s.inputs", apk);
    bool fresh = false;
    struct stat sa;
    FILE *sf = fopen(stamp, "rb");
    if (sf && stat(apk, &sa) == 0) {
        size_t n = strlen(want);
        char *have = malloc(n + 2);
        size_t got = fread(have, 1, n + 1, sf);
        fresh = got == n && !memcmp(have, want, n);
        free(have);
    }
    if (sf) fclose(sf);
    bool ok = true;
    if (fresh) {
        line("APK up to date: %s (%lld bytes, %d entries)", apk, (long long)sa.st_size, g_nentries);
    } else {
        ok = write_apk(apk);
        if (ok && stat(apk, &sa) == 0) {
            line("APK written: %s (%lld bytes, %d entries)", apk, (long long)sa.st_size, g_nentries);
            FILE *w = fopen(stamp, "wb");
            if (w) { fputs(want, w); fclose(w); }
        } else {
            line("could not write %s (errno %d)", apk, errno);
        }
    }
    free(want);
    return ok;
}

// ---- Android threads report their faults too --------------------------------------------------

typedef int (*pthread_create_fn)(void *, const void *, void *(*)(void *), void *);
static pthread_create_fn g_bionic_pthread_create;
typedef struct { void *(*fn)(void *); void *arg; } thread_start;

static void *thread_trampoline(void *p)
{
    thread_start s = *(thread_start *)p;
    free(p);
    ub_unity_thread_enter();
    void *r = s.fn(s.arg);
    ub_unity_thread_leave();
    return r;
}

static int g_threads;  // started by the Android libraries

static int ub_pthread_create(void *thread, const void *attr, void *(*fn)(void *), void *arg)
{
    __atomic_add_fetch(&g_threads, 1, __ATOMIC_RELAXED);
    thread_start *s = malloc(sizeof(*s));
    if (!s) return 12;  // ENOMEM
    s->fn = fn;
    s->arg = arg;
    const int r = g_bionic_pthread_create(thread, attr, thread_trampoline, s);
    if (r != 0) free(s);
    return r;
}

// ---- fewer cores ----------------------------------------------------------------------------------
// The engine starts a job worker per core but one (and per-thread allocators with their own 256 MiB
// regions, below), which the address space of an app without the extended-virtual-addressing
// entitlement cannot hold (M3f). Husk reports TL_NCPU cores in sysconf and /proc and /sys; its
// sched_getaffinity, where Unity reads its big.LITTLE mask, does not, hence this one.
#define UB_NCPU "2"
static int ub_sched_getaffinity(int pid, size_t size, void *mask)
{
    (void)pid;
    memset(mask, 0, size);
    const long n = atol(getenv("TL_NCPU") ? getenv("TL_NCPU") : UB_NCPU);
    for (long i = 0; i < n && (size_t)(i / 8) < size; i++) ((uint8_t *)mask)[i / 8] |= (uint8_t)(1u << (i % 8));
    return 0;
}

// ---- aligned reservations without the slack -----------------------------------------------------
// Unity's low-level virtual-memory allocator gives each of its allocators regions of 256 MiB aligned
// to 256 MiB (it keys ownership by address >> 28). Baselib gets one by reserving twice that less a
// page (an mmap of 0x1fffc000, PROT_NONE) and unmapping the edges outside the aligned window. An app
// without the extended-virtual-addressing entitlement has ~8 GiB beside the shared cache (M3f): the
// sixteenth or so region finds no 512 MiB gap, and every trimmed edge leaves a hole behind. Here such
// a request gets the aligned window directly (vm_map with an alignment mask; Baselib then finds it
// aligned and skips the front unmap), and the tail it then unmaps -- never mapped -- is ignored.
typedef void *(*mmap_fn)(void *, size_t, int, int, int, long);
typedef int (*munmap_fn)(void *, size_t);
static mmap_fn g_bionic_mmap;
static munmap_fn g_bionic_munmap;
#define UB_MAX_TAILS 128
static struct { uintptr_t start, end; } g_tails[UB_MAX_TAILS];  // the unmapped tails, by range
static pthread_mutex_t g_tails_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_aligned, g_aligned_failed;

// The window size R if (len, prot, flags) is "R aligned to R" in Baselib's form: len = 2R - page, R a
// power of two of at least 64 MiB, an anonymous private reservation with no address asked for.
static size_t aligned_request(void *addr, size_t len, int prot, int flags, int fd)
{
    if (addr || prot != 0 || fd != -1 || flags != 0x22) return 0;  // MAP_PRIVATE | MAP_ANONYMOUS
    static const size_t pages[] = {4096, 16384, 65536};
    for (int i = 0; i < 3; i++) {
        const size_t r = (len + pages[i]) / 2;
        if ((len + pages[i]) % 2 == 0 && r >= ((size_t)64 << 20) && (r & (r - 1)) == 0) return r;
    }
    return 0;
}

static void *reserve_aligned(size_t r)
{
#ifdef __APPLE__
    vm_address_t a = 0;
    if (vm_map(mach_task_self(), &a, r, r - 1, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL,
               VM_INHERIT_COPY) != KERN_SUCCESS)
        return NULL;
    return (void *)a;
#else
    void *p = mmap(NULL, 2 * r, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    const uintptr_t b = (uintptr_t)p, a = (b + r - 1) & ~(uintptr_t)(r - 1);
    if (a > b) munmap(p, a - b);
    munmap((void *)(a + r), b + 2 * r - (a + r));
    return (void *)a;
#endif
}

static void *ub_mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    const size_t r = aligned_request(addr, len, prot, flags, fd);
    if (r) {
        void *a = reserve_aligned(r);
        pthread_mutex_lock(&g_tails_lock);
        int slot = -1;
        for (int i = 0; a && i < UB_MAX_TAILS && slot < 0; i++) if (!g_tails[i].start) slot = i;
        if (a && slot >= 0) {
            g_tails[slot].start = (uintptr_t)a + r;
            g_tails[slot].end = (uintptr_t)a + len;
            ++g_aligned;
        } else {
            ++g_aligned_failed;
        }
        pthread_mutex_unlock(&g_tails_lock);
        if (a && slot >= 0) {
            if (g_aligned <= 3) line("  aligned reservation: %zu MiB at %p directly (asked %#zx)", r >> 20, a, len);
            return a;
        }
        if (a) munmap(a, r);  // no slot left to remember the tail: Husk's way instead
    }
    return g_bionic_mmap(addr, len, prot, flags, fd, off);
}

static int ub_munmap(void *addr, size_t len)
{
    const uintptr_t a = (uintptr_t)addr, e = a + len;
    pthread_mutex_lock(&g_tails_lock);
    for (int i = 0; i < UB_MAX_TAILS; i++) {
        if (!g_tails[i].start || a >= g_tails[i].end || e <= g_tails[i].start) continue;
        // Within a tail: nothing there is ours. The tail goes once all of it was given back.
        const bool whole = a <= g_tails[i].start && e >= g_tails[i].end;
        const uintptr_t keep_end = g_tails[i].start;
        if (whole) g_tails[i].start = g_tails[i].end = 0;
        pthread_mutex_unlock(&g_tails_lock);
        // A range that also covers mapped memory before the tail: that part is unmapped for real.
        if (a < keep_end) return g_bionic_munmap(addr, keep_end - a);
        return 0;
    }
    pthread_mutex_unlock(&g_tails_lock);
    return g_bionic_munmap(addr, len);
}

// ---- dialogs are answered -----------------------------------------------------------------------
// Husk's Java world shows an AlertDialog by logging it ("DIALOG ...") and never presses a button, so
// an engine that waits for the answer (Unity's "device does not match the hardware requirements"
// warning, M3f) waits forever. These HLE methods, registered before Husk's (the first table wins),
// keep the positive button's listener and press it half a second after show(), on a thread of its
// own (the engine may hold a lock while it shows the dialog), as a user tapping OK would.

static jvalue jl(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static void ub_Builder_setPositive(tl_jcall *c)
{
    tl_jni_set_field(c->self, "ub_positive", "Ljava/lang/Object;", jl(c->args[1].l ? tl_jni_ref(c->args[1].l) : NULL));
    c->ret = jl(tl_jni_ref(c->self));
}
static jobj *dialog_from(jobj *builder)
{
    jobj *d = tl_jni_new_object(tl_jni_class("android/app/AlertDialog"));
    static const char *const fields[][2] = {{"title", "Ljava/lang/String;"}, {"message", "Ljava/lang/String;"},
                                           {"ub_positive", "Ljava/lang/Object;"}};
    for (int i = 0; i < 3; i++) {
        jobj *v = tl_jni_get_field(builder, fields[i][0], fields[i][1]).l;
        tl_jni_set_field(d, fields[i][0], fields[i][1], jl(v ? tl_jni_ref(v) : NULL));
    }
    return d;
}
typedef struct { jobj *listener, *dialog; } pending_click;
static void *press_positive(void *p)
{
    pending_click *pc = p;
    usleep(500 * 1000);
    jvalue args[2];
    args[0] = jl(pc->dialog);
    args[1].j = 0;
    args[1].i = -1;  // DialogInterface.BUTTON_POSITIVE
    line("dialog answered: OK (the positive button's onClick)");
    const char *sig = "(Landroid/content/DialogInterface;I)V";
    if (!strcmp(tl_jni_class_name(pc->listener), "java/lang/reflect/Proxy")) {
        // The engine's own listener: a Proxy from ReflectionHelper whose calls go to the native
        // nativeProxyInvoke(handle, "onClick", args), with the int boxed as Java would.
        jobj *which = tl_jni_new_object(tl_jni_class("java/lang/Integer"));
        tl_jni_set_field(which, "value", "I", args[1]);
        jobj *pargs[2] = {pc->dialog, which};
        jobj *r = tl_proxy_call(pc->listener, "android/content/DialogInterface$OnClickListener", "onClick", sig, pargs, 2);
        line("  through the engine's proxy (nativeProxyInvoke)%s", r ? ", returned an object" : "");
        if (r) tl_jni_unref(r);
        tl_jni_unref(which);
    } else {
        tl_jni_call(pc->listener, "onClick", sig, args);
    }
    if (tl_jni_pending()) { line("  onClick threw; cleared"); tl_jni_clear(); }
    free(pc);
    return NULL;
}
static void ub_Dialog_show(tl_jcall *c)
{
    const char *title = tl_jni_string(tl_jni_get_field(c->self, "title", "Ljava/lang/String;").l);
    const char *message = tl_jni_string(tl_jni_get_field(c->self, "message", "Ljava/lang/String;").l);
    line("DIALOG \"%s\": %s", title ? title : "", message ? message : "");
    jobj *listener = tl_jni_get_field(c->self, "ub_positive", "Ljava/lang/Object;").l;
    if (!listener) { line("  (no positive button to press)"); return; }
    pending_click *pc = malloc(sizeof(*pc));
    pc->listener = tl_jni_ref(listener);
    pc->dialog = tl_jni_ref(c->self);
    pthread_t t;
    if (pthread_create(&t, NULL, press_positive, pc) == 0) pthread_detach(t);
    else free(pc);
}
static void ub_Builder_create(tl_jcall *c) { c->ret = jl(dialog_from(c->self)); }
static void ub_Builder_show(tl_jcall *c)
{
    jobj *d = dialog_from(c->self);
    ub_Dialog_show(&(tl_jcall){.self = d});
    c->ret = jl(d);
}
static const tl_jhle g_dialog_hle[] = {
    {"android/app/AlertDialog$Builder", "setPositiveButton",
     "(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;", ub_Builder_setPositive},
    {"android/app/AlertDialog$Builder", "create", "()Landroid/app/AlertDialog;", ub_Builder_create},
    {"android/app/AlertDialog$Builder", "show", "()Landroid/app/AlertDialog;", ub_Builder_show},
    {"android/app/Dialog", "show", "()V", ub_Dialog_show},
    {"android/app/AlertDialog", "show", "()V", ub_Dialog_show},
    {NULL, NULL, NULL, NULL},
};

// ---- the run -----------------------------------------------------------------------------------

static int g_ran;
int ub_unity_probe_ran(void) { return g_ran; }

static bool interpose(void)
{
    g_bionic_dlopen = (dlopen_fn)tl_bionic_find("dlopen");
    g_bionic_dlopen_ext = (dlopen_ext_fn)tl_bionic_find("android_dlopen_ext");
    g_bionic_dlsym = (dlsym_fn)tl_bionic_find("dlsym");
    g_bionic_pthread_create = (pthread_create_fn)tl_bionic_find("pthread_create");
    g_bionic_mmap = (mmap_fn)tl_bionic_find("mmap");
    g_bionic_munmap = (munmap_fn)tl_bionic_find("munmap");
    if (!g_bionic_dlopen || !g_bionic_dlsym) { line("FAIL Husk's bionic has no dlopen/dlsym"); return false; }
    tl_ld_interpose("dlopen", (void *)&ub_dlopen);
    tl_ld_interpose("android_dlopen_ext", (void *)&ub_android_dlopen_ext);
    tl_ld_interpose("dlsym", (void *)&ub_dlsym);
    if (g_bionic_pthread_create) tl_ld_interpose("pthread_create", (void *)&ub_pthread_create);
    tl_ld_interpose("sched_getaffinity", (void *)&ub_sched_getaffinity);
    if (g_bionic_mmap && g_bionic_munmap) {
        tl_ld_interpose("mmap", (void *)&ub_mmap);
        tl_ld_interpose("mmap64", (void *)&ub_mmap);
        tl_ld_interpose("munmap", (void *)&ub_munmap);
    }
    tl_ld_set_verbosity(1);
    return true;
}

// ANGLE (EGL + GLES over Metal, scripts/build-angle-ios.sh), embedded in the app's Frameworks folder:
// its path next to this executable, or NULL when the app carries none.
static const char *angle_path(void)
{
    static char path[1100];
    Dl_info info;
    if (!dladdr((const void *)&angle_path, &info) || !info.dli_fname) return NULL;
    const char *slash = strrchr(info.dli_fname, '/');
    if (!slash) return NULL;
    snprintf(path, sizeof(path), "%.*s/Frameworks/libANGLE-shared.dylib", (int)(slash - info.dli_fname), info.dli_fname);
    struct stat st;
    return stat(path, &st) == 0 ? path : NULL;
}

// `frames` (M3f): with ANGLE, draw off-screen and keep frames 1-3 and the latest one (every 60th
// frame) as BMP files in that folder (Husk's frame capture). NULL: no graphics (M3d/M3e).
static bool start(const char *apk, const char *app_data, const char *frames)
{
    mkdir(app_data, 0755);
    tl_unity_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.apk_path = apk;
    cfg.data_dir = app_data;
    cfg.package_name = "io.github.pander0212.unitybridge.game";
    cfg.width = 2556;
    cfg.height = 1179;
    if (frames) {
        cfg.angle_egl = angle_path();
        if (cfg.angle_egl) {
            mkdir(frames, 0755);
            cfg.frame_dir = frames;
            cfg.frame_every = -60;
            line("graphics: ANGLE %s, frames (BMP) to %s", cfg.angle_egl, frames);
        } else {
            line("graphics: no libANGLE-shared.dylib in the app's Frameworks: the engine has no EGL");
        }
    }
    line("tl_unity_start: libmain.so JNI_OnLoad, then NativeLoader.load (libunity.so) ...");
    const bool started = tl_unity_start(&cfg);
    line("tl_unity_start -> %s", started ? "true" : "false");
    tl_lib *unity = tl_ld_find_lib("libunity.so"), *mainlib = tl_ld_find_lib("libmain.so");
    line("libmain.so %s, libunity.so %s", mainlib ? "loaded" : "not loaded", unity ? "loaded" : "not loaded");
    if (unity) line("  libunity.so at %p, JNI_OnLoad %p", tl_ld_lib_base(unity), tl_ld_sym(unity, "JNI_OnLoad"));
    line("unresolved imports (bound to logging stubs): %zu", tl_ld_unresolved_count());
    return started && unity;
}

// M3d: after our own il2cpp_init and tests, the engine loaded up to NativeLoader.load.
int ub_unity_probe_run(const char *unity_dir, const char *app_data, char *out, size_t capacity)
{
    g_out = out; g_cap = capacity; g_len = 0;
    if (capacity) out[0] = 0;
    line("--- Unity's Android engine under Husk's runtime (M3d) ---");
    if (g_ran++) { line("already ran in this app session; reopen the app"); return 3; }
    char apk[1100];
    if (!make_apk(unity_dir, NULL, apk, sizeof(apk))) {
        line("Put libmain.so, libunity.so (Libs/arm64-v8a) and classes.dex (Classes) from Unity's Android player");
        line("(the game's Unity version, Variations/il2cpp/Release) into %s and run again.", unity_dir);
        return 2;
    }
    ub_unity_thread_enter();
    if (!interpose()) { ub_unity_thread_leave(); return 1; }
    {   // the virtual libil2cpp.so as libunity will open it
        void *h = ub_dlopen("/data/app/lib/arm64/libil2cpp.so", 2);
        void *(*domain_get)(void) = (void *(*)(void))ub_dlsym(h, "il2cpp_domain_get");
        void *domain = domain_get ? domain_get() : NULL;
        line("libil2cpp.so (virtual): il2cpp_domain_get() = %p %s", domain, domain ? "ok" : "FAIL");
    }
    const bool ok = start(apk, app_data, NULL);
    line("libil2cpp.so: %d dlopen, %d dlsym (%d not exported)", g_il2cpp_opens, g_il2cpp_syms, g_il2cpp_missing);
    line("UNITY: %s", ok ? "engine loaded (NativeLoader.load succeeded)" : "did not get through (see above)");
    ub_unity_thread_leave();
    return ok ? 0 : 1;
}

// M3e: GameAssembly loaded and wired but il2cpp not started (ub_il2cpp_probe_set_engine_mode); the
// engine starts it. The game's <Game>_Data goes into the APK as the engine's data, then the
// UnityPlayer start-up runs (initJni, the UnityMain thread with nativeRecreateGfxState, nativeResume,
// nativeRender), with file and dlopen traces in the log. Graphics (M3f): ANGLE when the app carries
// it, drawing off-screen into Documents/Frames.
int ub_unity_engine_run(const char *unity_dir, const char *game_data, const char *app_data, char *out, size_t capacity)
{
    g_out = out; g_cap = capacity; g_len = 0;
    if (capacity) out[0] = 0;
    line("--- Unity engine start (M3e): the engine runs il2cpp itself ---");
    if (g_ran++) { line("already ran in this app session; reopen the app"); return 3; }
    char apk[1100];
    if (!make_apk(unity_dir, game_data, apk, sizeof(apk))) return 2;
    setenv("TL_FILE_TRACE", "1", 1);
    setenv("TL_DL_TRACE", "1", 1);
    // M3f: who holds the address space. Every guest mmap/munmap; one of 64 MiB or more also names
    // the library function that asked. (TL_EGL_TRACE showed EGL working; off again, it is noisy.)
    setenv("TL_MM_TRACE", "1", 1);
    setenv("TL_NCPU", UB_NCPU, 0);  // fewer job workers: see ub_sched_getaffinity
    tl_jni_register_hle(g_dialog_hle);  // before Husk's own tables: these win
    ub_unity_thread_enter();
    if (!interpose()) { ub_unity_thread_leave(); return 1; }
    ub_memory_dump("UB_MEM before the engine:");
    char frames[1100];
    snprintf(frames, sizeof(frames), "%s/../Frames", unity_dir);
    if (!start(apk, app_data, frames)) { ub_unity_thread_leave(); line("UNITY: did not get through NativeLoader.load"); return 1; }
    ub_memory_dump("UB_MEM after NativeLoader.load:");
    line("tl_unity_run: initJni, then the UnityMain thread (surface, resume, nativeRender) ...");
    const bool running = tl_unity_run();
    line("tl_unity_run -> %s", running ? "UnityMain started" : "false");
    char report[2048];
    for (int s = 1; running && s <= 60; s++) {
        sleep(1);
        if (s == 1 || s == 5 || s == 10 || s == 30 || s == 60) {
            ub_il2cpp_engine_report(report, sizeof(report));
            line("after %d s: %lu frames (%lu presented); libil2cpp.so %d dlopen, %d dlsym (%d not exported); %d aligned "
                 "reservations (%d not); %d threads started; %s", s, tl_unity_frames(), tl_egl_frames_presented(), g_il2cpp_opens,
                 g_il2cpp_syms, g_il2cpp_missing, g_aligned, g_aligned_failed, g_threads, report);
            ub_memory_dump("UB_MEM");
        }
    }
    line("UNITY: %s, %lu frames", running ? "engine running" : "UnityMain did not start", tl_unity_frames());
    ub_unity_thread_leave();
    return running ? 0 : 1;
}
