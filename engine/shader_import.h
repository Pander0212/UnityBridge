// SPDX-License-Identifier: GPL-3.0-or-later
// The device's shader import (M4d), for app/Sources/unity_loader.c: a serialized file of the game
// whose Shader objects carry D3D11 programs only gets a GLES3x platform (engine/shader_gles3.h), in
// a copy; the game's own file is not changed.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped when the conversion's output changes: copies made by another version are made again.
int ub_shader_import_version(void);

// Writes the converted copy of `src` to `dst`. 1: written; 0: nothing to convert (not a serialized
// file, no Shader objects, or all have GLES3 already), `dst` untouched; -1: error. `log` (may be
// NULL) receives a summary of what was done, or why not.
int ub_shader_import(const char *src, const char *dst, char *log, size_t logcap);

#ifdef __cplusplus
}
#endif
