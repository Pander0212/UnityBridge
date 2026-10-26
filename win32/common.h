// SPDX-License-Identifier: GPL-3.0-or-later
// Helpers shared by the win32/*.cpp function sets (not part of the public interface).
#pragma once

#include "hle.h"

#include <cstring>

namespace UnityBridge::Win32 {

using DWORD = uint32_t;
using BOOL = int32_t;
inline constexpr uint64_t kTrue = 1, kFalse = 0;
inline constexpr uint64_t kInvalidHandle = ~uint64_t {0};  // INVALID_HANDLE_VALUE

// winerror.h
inline constexpr DWORD ERROR_SUCCESS = 0, ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3,
    ERROR_TOO_MANY_OPEN_FILES = 4, ERROR_ACCESS_DENIED = 5, ERROR_INVALID_HANDLE = 6,
    ERROR_NOT_ENOUGH_MEMORY = 8, ERROR_NO_MORE_FILES = 18, ERROR_NOT_SUPPORTED = 50,
    ERROR_FILE_EXISTS = 80, ERROR_INVALID_PARAMETER = 87, ERROR_CALL_NOT_IMPLEMENTED = 120,
    ERROR_INSUFFICIENT_BUFFER = 122, ERROR_DIR_NOT_EMPTY = 145, ERROR_ALREADY_EXISTS = 183,
    ERROR_ENVVAR_NOT_FOUND = 203, ERROR_TIMEOUT = 1460, ERROR_MOD_NOT_FOUND = 126,
    ERROR_PROC_NOT_FOUND = 127, ERROR_NO_UNICODE_TRANSLATION = 1113, ERROR_HANDLE_EOF = 38;

inline uint64_t SetError(DWORD error, uint64_t result) {
  SetLastErrorValue(error);
  return result;
}

template <typename T>
inline void Store(uint64_t address, T value) {
  if (address) std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
}

template <typename T>
inline T Load(uint64_t address) {
  T value {};
  if (address) std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
  return value;
}

// File objects live in kernel32_files.cpp; CloseHandle (kernel32_sync.cpp) asks there first.
bool CloseFileObject(uint64_t handle);

// errno -> Windows error code for file functions.
DWORD ErrorFromErrno(int error);

// Copies `text` plus terminator to a guest UTF-16 buffer of `capacity` characters. Returns the
// length without terminator, or (Windows convention) the needed size including the terminator
// when the buffer is too small.
DWORD CopyWideOut(std::u16string_view text, char16_t* out, DWORD capacity);

} // namespace UnityBridge::Win32
