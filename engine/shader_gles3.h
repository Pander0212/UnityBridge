// SPDX-License-Identifier: GPL-3.0-or-later
// Gives Unity 2021.3 Shader objects that have D3D11 programs a GLES3x platform (M4c), the C++ form of
// tools/research/add_gles3.py: per pass and keyword variant the D3D11 vertex and pixel programs are
// translated together (ShaderXlat, HLSLcc) into Unity's GLES3 layout, one combined source on the
// vertex stage with a parameter record; the records go into a new platform-9 blob. Runs on the
// device, on the game's files, when it is imported.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "engine/unity_typetree.h"

namespace UnityBridge::ShaderGles3 {

struct Stats {
  int shaders = 0;     // Shader objects given a GLES3x platform
  int programs = 0;    // vertex + fragment pairs translated
  int failed = 0;      // pairs that could not be translated
  std::string log;     // a line per shader, and the reasons for failures
  // Called before each shader is translated (and with each failure), so a crash inside HLSLcc can be
  // placed from the log.
  std::function<void(const std::string&)> progress;
};

// The LZ4 block `src` decompressed to `size` bytes; nullopt if it is malformed.
std::optional<std::vector<uint8_t>> Lz4Decompress(const uint8_t* src, size_t n, size_t size);
// An LZ4 block holding `data` as literals only (any LZ4 decoder reads it; nothing is compressed).
std::vector<uint8_t> Lz4Literals(const std::vector<uint8_t>& data);

// Adds the GLES3x platform to `shader` (a Value read with TypeTree::Shader2021_3()). With `replace`,
// an existing GLES3x platform is dropped first (to compare with Unity's own); without, a shader that
// has one is left alone. False if the shader was not changed.
bool AddGles3(TypeTree::Value& shader, bool replace, Stats* stats);

// The serialized file `data` with every Shader converted; nullopt if nothing changed (or the file
// is not one this reads: `error` says why).
std::optional<std::vector<uint8_t>> ConvertFile(const uint8_t* data, size_t size, bool replace, Stats* stats,
                                                std::string* error);

} // namespace UnityBridge::ShaderGles3
