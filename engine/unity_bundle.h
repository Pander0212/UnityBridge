// SPDX-License-Identifier: GPL-3.0-or-later
// Unity asset bundles (UnityFS, format 6-8) built for a desktop player, made loadable by the Android
// player (M6). Android's engine refuses a bundle whose serialized files name a desktop build target
// ("can't be loaded because it was not built with the right version or build target"). Retarget
// decompresses the bundle (none or LZ4/LZ4HC blocks), rewrites the build target in the header of
// every serialized file inside it to Android (13), and writes the bundle back uncompressed, with four
// bytes after the files that keep the CRC-32 of its blocks what Addressables' catalog expects.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace UnityBridge::UnityBundle {

// True if `data` starts like a UnityFS bundle.
bool IsBundle(const uint8_t* data, size_t size);

struct Stats {
  int nodes = 0;       // files in the bundle
  int retargeted = 0;  // serialized files whose build target was rewritten
};

// One file in a bundle: its path ("CAB-<hash>", "CAB-<hash>.resS", ...), directory flags (4: a
// serialized file) and bytes.
struct File {
  std::string path;
  uint32_t flags = 0;
  std::vector<uint8_t> data;
};
using Edit = std::function<bool(std::vector<File>& files)>;

// The bundle unpacked, `edit` run on its files (it may change them, add some, or say false: nothing to
// write), and packed again uncompressed, its blocks' CRC-32 kept as it was (four bytes after the last
// file restore it). Nullopt if the bundle is not read (`error`) or `edit` said false.
std::optional<std::vector<uint8_t>> Transform(const uint8_t* data, size_t size, const Edit& edit, std::string* error);

// True if `data` starts like a serialized file (the version field at 8 in a plausible range).
bool IsSerializedFile(const uint8_t* data, size_t size);

// One serialized file's desktop build target rewritten to Android in place; false if it named none.
bool RetargetFile(std::vector<uint8_t>& file);

// CRC-32 (zlib's) of the uncompressed blocks: what the catalog stores and the engine checks.
std::optional<uint32_t> BlocksCrc(const uint8_t* data, size_t size, std::string* error);

// The bundle with its serialized files' desktop build targets (2, 5, 19, 24) rewritten to Android,
// uncompressed. Nullopt and `error` if it is not a bundle this reads (LZMA blocks are not), or if no
// file in it needed rewriting (`error` empty then).
std::optional<std::vector<uint8_t>> Retarget(const uint8_t* data, size_t size, Stats* stats, std::string* error);

} // namespace UnityBridge::UnityBundle
