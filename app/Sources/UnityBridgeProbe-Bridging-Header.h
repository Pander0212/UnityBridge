// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdbool.h>
#include <stddef.h>

// engine/probe_runner.cpp
bool ub_probe_jit_attached(void);
bool ub_probe_debugger_attached(void);
int ub_probe_run(char *out, size_t capacity);

// engine/win32_device.cpp
int ub_win32_device_run(const char *dllPath, const char *profileDir, char *out, size_t capacity);
int ub_win32_device_has_run(void);  // nonzero once it ran in this process

// engine/il2cpp_probe.cpp
int ub_il2cpp_probe_run(const char *dllPath, const char *dataDir, const char *profileDir, char *out, size_t capacity);
int ub_il2cpp_probe_started(void);  // nonzero once il2cpp runs in this process (it stays up until exit)
void ub_il2cpp_probe_set_engine_mode(int on);  // M3e: stop before il2cpp_init, the engine starts it

// engine/unity_host.cpp: the executable region for Husk's loader (requested once, kept)
void *husk_ios_jit_get_mapping(void);

// app/Sources/unity_loader.c (Husk's Android runtime)
int ub_unity_probe_run(const char *unityDir, const char *dataDir, char *out, size_t capacity);
int ub_unity_probe_ran(void);
int ub_unity_engine_run(const char *unityDir, const char *gameData, const char *appData, char *out, size_t capacity);
