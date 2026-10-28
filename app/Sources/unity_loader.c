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
#include "husk-tl-bionic.h"
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
#include <zlib.h>

// engine/unity_host.cpp
void *ub_il2cpp_symbol(const char *name);
void ub_unity_thread_enter(void);
void ub_unity_thread_leave(void);

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
// A stored (uncompressed) zip. Each entry's data starts on a 16 KiB boundary (padding in the local
// header's extra field), as zipalign -P 16 lays out an APK's libraries.

typedef struct {
    const char *name;
    uint8_t *data;
    size_t size;
    uint32_t crc;
    uint64_t offset;  // of the local header
} apk_entry;

static void put16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc(v >> 8, f); }
static void put32(FILE *f, uint32_t v) { put16(f, (uint16_t)v); put16(f, (uint16_t)(v >> 16)); }

static bool read_whole(const char *path, uint8_t **data, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *data = n > 0 ? malloc((size_t)n) : NULL;
    bool ok = *data && fread(*data, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { free(*data); *data = NULL; return false; }
    *size = (size_t)n;
    return true;
}

static bool write_apk(const char *apk, apk_entry *e, int count)
{
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", apk);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    for (int i = 0; i < count; i++) {
        e[i].crc = (uint32_t)crc32(0, e[i].data, (uInt)e[i].size);
        e[i].offset = (uint64_t)ftell(f);
        const size_t nlen = strlen(e[i].name);
        size_t pad = (16384 - (e[i].offset + 30 + nlen) % 16384) % 16384;
        if (pad && pad < 4) pad += 16384;  // room for the extra field's own header
        put32(f, 0x04034b50); put16(f, 10); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
        put32(f, e[i].crc); put32(f, (uint32_t)e[i].size); put32(f, (uint32_t)e[i].size);
        put16(f, (uint16_t)nlen); put16(f, (uint16_t)pad);
        fwrite(e[i].name, 1, nlen, f);
        if (pad) {  // 0xd935: Android's alignment extra field
            put16(f, 0xd935); put16(f, (uint16_t)(pad - 4));
            for (size_t k = 4; k < pad; k++) fputc(0, f);
        }
        fwrite(e[i].data, 1, e[i].size, f);
    }
    const uint64_t cd = (uint64_t)ftell(f);
    for (int i = 0; i < count; i++) {
        const size_t nlen = strlen(e[i].name);
        put32(f, 0x02014b50); put16(f, 20); put16(f, 10); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
        put32(f, e[i].crc); put32(f, (uint32_t)e[i].size); put32(f, (uint32_t)e[i].size);
        put16(f, (uint16_t)nlen); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0); put32(f, 0);
        put32(f, (uint32_t)e[i].offset);
        fwrite(e[i].name, 1, nlen, f);
    }
    const uint64_t end = (uint64_t)ftell(f);
    put32(f, 0x06054b50); put16(f, 0); put16(f, 0); put16(f, (uint16_t)count); put16(f, (uint16_t)count);
    put32(f, (uint32_t)(end - cd)); put32(f, (uint32_t)cd); put16(f, 0);
    const bool ok = !ferror(f);
    fclose(f);
    return ok && rename(tmp, apk) == 0;
}

// Writes <dir>/base.apk from <dir>/libmain.so, <dir>/libunity.so and, if present, <dir>/classes.dex
// (Unity's Java side: UnityPlayer, NativeLoader, ..., which Husk's Dalvik runs). Rewritten on every
// run: 25 MB take well under a second, and file times copied out of a zip say nothing reliable.
static bool make_apk(const char *dir, char *apk, size_t apkcap)
{
    static const struct { const char *file, *entry; bool required; } parts[] = {
        {"classes.dex", "classes.dex", false},
        {"libmain.so", "lib/arm64-v8a/libmain.so", true},
        {"libunity.so", "lib/arm64-v8a/libunity.so", true},
    };
    snprintf(apk, apkcap, "%s/base.apk", dir);
    apk_entry e[3];
    int n = 0;
    bool ok = true;
    for (int i = 0; i < 3 && ok; i++) {
        char p[1100];
        snprintf(p, sizeof(p), "%s/%s", dir, parts[i].file);
        struct stat st;
        if (stat(p, &st) != 0) {
            if (parts[i].required) { line("missing %s", p); ok = false; }
            else line("  %s: not there (Unity's Java classes; JNI_OnLoad will not find NativeLoader)", parts[i].file);
            continue;
        }
        memset(&e[n], 0, sizeof(e[n]));
        e[n].name = parts[i].entry;
        ok = read_whole(p, &e[n].data, &e[n].size);
        if (ok) line("  %s: %zu bytes", parts[i].file, e[n++].size);
    }
    ok = ok && write_apk(apk, e, n);
    for (int i = 0; i < n; i++) free(e[i].data);
    struct stat sa;
    if (ok && stat(apk, &sa) == 0) line("APK written: %s (%lld bytes, %d entries)", apk, (long long)sa.st_size, n);
    else if (n) line("could not write %s (errno %d)", apk, errno);
    return ok;
}

// ---- the run -----------------------------------------------------------------------------------

static int g_ran;
int ub_unity_probe_ran(void) { return g_ran; }

int ub_unity_probe_run(const char *unity_dir, const char *data_dir, char *out, size_t capacity)
{
    g_out = out; g_cap = capacity; g_len = 0;
    if (capacity) out[0] = 0;
    line("--- Unity's Android engine under Husk's runtime (M3d) ---");
    if (g_ran++) { line("already ran in this app session; reopen the app"); return 3; }
    char apk[1100];
    if (!make_apk(unity_dir, apk, sizeof(apk))) {
        line("Put libmain.so, libunity.so (Libs/arm64-v8a) and classes.dex (Classes) from Unity's Android player");
        line("(the game's Unity version, Variations/il2cpp/Release) into %s and run again.", unity_dir);
        return 2;
    }
    ub_unity_thread_enter();

    g_bionic_dlopen = (dlopen_fn)tl_bionic_find("dlopen");
    g_bionic_dlopen_ext = (dlopen_ext_fn)tl_bionic_find("android_dlopen_ext");
    g_bionic_dlsym = (dlsym_fn)tl_bionic_find("dlsym");
    if (!g_bionic_dlopen || !g_bionic_dlsym) { line("FAIL Husk's bionic has no dlopen/dlsym"); ub_unity_thread_leave(); return 1; }
    tl_ld_interpose("dlopen", (void *)&ub_dlopen);
    tl_ld_interpose("android_dlopen_ext", (void *)&ub_android_dlopen_ext);
    tl_ld_interpose("dlsym", (void *)&ub_dlsym);
    tl_ld_set_verbosity(1);

    // The virtual libil2cpp.so as libunity will open it.
    {
        void *h = ub_dlopen("/data/app/lib/arm64/libil2cpp.so", 2);
        void *(*domain_get)(void) = (void *(*)(void))ub_dlsym(h, "il2cpp_domain_get");
        void *domain = domain_get ? domain_get() : NULL;
        line("libil2cpp.so (virtual): il2cpp_domain_get() = %p %s", domain, domain ? "ok" : "FAIL");
    }

    mkdir(data_dir, 0755);
    tl_unity_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.apk_path = apk;
    cfg.data_dir = data_dir;
    cfg.package_name = "io.github.pander0212.unitybridge.game";
    cfg.width = 2556;
    cfg.height = 1179;
    line("tl_unity_start: libmain.so JNI_OnLoad, then NativeLoader.load (libunity.so) ...");
    const bool started = tl_unity_start(&cfg);
    line("tl_unity_start -> %s", started ? "true" : "false");

    tl_lib *unity = tl_ld_find_lib("libunity.so"), *mainlib = tl_ld_find_lib("libmain.so");
    line("libmain.so %s, libunity.so %s", mainlib ? "loaded" : "not loaded", unity ? "loaded" : "not loaded");
    if (unity) {
        line("  libunity.so at %p, JNI_OnLoad %p", tl_ld_lib_base(unity), tl_ld_sym(unity, "JNI_OnLoad"));
    }
    line("unresolved imports (bound to logging stubs): %zu", tl_ld_unresolved_count());
    line("libil2cpp.so: %d dlopen, %d dlsym (%d not exported)", g_il2cpp_opens, g_il2cpp_syms, g_il2cpp_missing);
    line("UNITY: %s", started && unity ? "engine loaded (NativeLoader.load succeeded)" : "did not get through (see above)");
    ub_unity_thread_leave();
    return started && unity ? 0 : 1;
}
