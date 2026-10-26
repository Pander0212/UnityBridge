// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-in for shadPS4's logging header, used by the vendored AetherPS4 files. Prints the
// format string and the class to stderr; arguments are not formatted.
#pragma once

#include <cstdio>

#define UB_LOG(level, cls, fmt, ...) std::fprintf(stderr, "[%s] %s: %s\n", level, #cls, fmt)
#define LOG_TRACE(cls, fmt, ...) UB_LOG("trace", cls, fmt)
#define LOG_DEBUG(cls, fmt, ...) UB_LOG("debug", cls, fmt)
#define LOG_INFO(cls, fmt, ...) UB_LOG("info", cls, fmt)
#define LOG_WARNING(cls, fmt, ...) UB_LOG("warning", cls, fmt)
#define LOG_ERROR(cls, fmt, ...) UB_LOG("error", cls, fmt)
#define LOG_CRITICAL(cls, fmt, ...) UB_LOG("critical", cls, fmt)
