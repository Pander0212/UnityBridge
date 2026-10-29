// SPDX-License-Identifier: GPL-3.0-or-later
// Texture2D objects of a Unity 2021.3 serialized file (M5). A Windows build compresses its textures
// in desktop formats (BC1/BC3 "DXT", BC4/BC5, BC6H/BC7), which the iPhone's GPU does not sample
// (ANGLE over Metal offers ETC2, ASTC and PVRTC there). First step: which formats a game uses.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "engine/unity_file.h"

namespace UnityBridge::UnityTextures {

// Unity's TextureFormat name for `format` ("DXT5", "BC7", "RGBA32", ...), or its number.
std::string FormatName(int format);

// The Texture2D objects of `file` by format; objects the 2021.3 type tree does not read count as -1.
std::map<int, int> Census(const UnityFile::File& file, const uint8_t* data, size_t size);

// "12 textures: DXT5 8, BC7 3, RGBA32 1", or "" for none.
std::string CensusLine(const std::map<int, int>& census);

} // namespace UnityBridge::UnityTextures
