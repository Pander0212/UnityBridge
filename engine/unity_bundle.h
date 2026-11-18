// SPDX-License-Identifier: GPL-3.0-or-later
// Unity asset bundles (UnityFS, format 6-8) built for a desktop player, made loadable by the Android
// player (M6). Android's engine refuses a bundle whose serialized files name a desktop build target
// ("can't be loaded because it was not built with the right version or build target"). Retarget
// decompresses the bundle (none or LZ4/LZ4HC blocks), rewrites the build target in the header of
// every serialized file inside it to Android (13), and writes the bundle back uncompressed.
#pragma once
#include <cstdint>
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

// The bundle with its serialized files' desktop build targets (2, 5, 19, 24) rewritten to Android,
// uncompressed. Nullopt and `error` if it is not a bundle this reads (LZMA blocks are not), or if no
// file in it needed rewriting (`error` empty then).
std::optional<std::vector<uint8_t>> Retarget(const uint8_t* data, size_t size, Stats* stats, std::string* error);

} // namespace UnityBridge::UnityBundle
