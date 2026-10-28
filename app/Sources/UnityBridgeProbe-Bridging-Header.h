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
