// SPDX-License-Identifier: GPL-3.0-or-later
// Texture2D objects of a Unity 2021.3 serialized file (M5). A Windows build compresses its textures
// in desktop formats (BC1/BC3 "DXT", BC4/BC5, BC6H/BC7), which the iPhone's GPU does not sample
// (ANGLE over Metal offers ETC2, ASTC and PVRTC there). First step: which formats a game uses.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "engine/unity_file.h"
#include "engine/unity_typetree.h"

namespace UnityBridge::UnityTextures {

// Unity's TextureFormat name for `format` ("DXT5", "BC7", "RGBA32", ...), or its number.
std::string FormatName(int format);

// The Texture2D objects of `file` by format; objects the 2021.3 type tree does not read count as -1.
std::map<int, int> Census(const UnityFile::File& file, const uint8_t* data, size_t size);

// "12 textures: DXT5 8, BC7 3, RGBA32 1", or "" for none.
std::string CensusLine(const std::map<int, int>& census);

// Decodes `width` x `height` pixels of DXT1 (BC1, format 10), DXT5 (BC3, format 12) or BC7 (format 25)
// blocks at `src`
// to RGBA32 (4 bytes a pixel, rows top to bottom as the blocks are). False for another format or
// too little data.
bool DecodeBlocks(int format, const uint8_t* src, size_t size, int width, int height, uint8_t* rgba);

// Where a texture's pixels are read from and its new ones written to. Data a texture streams from a
// resource file (m_StreamData) is read through `read`; the RGBA32 data of streamed textures goes to
// one new resource file, `path` as the objects name it, appended through `write`.
struct TextureIO {
  std::function<std::optional<std::vector<uint8_t>>(const std::string& path, uint64_t offset, uint32_t size)> read;
  std::function<bool(const uint8_t* data, size_t size)> write;
  std::string path;
  uint64_t written = 0;  // bytes in the new file so far
};

// A DXT1/DXT5/BC7 Texture2D (a Value of the version's Texture2D tree) to RGBA32, every mip level: the format, the
// image size and the pixels (in the object, or in the new resource file) change. False if it is not
// one of those formats or a 2D texture; `error` says why when it is one but could not be converted.
bool ToRgba32(TypeTree::Value& texture, TextureIO& io, std::string* error);

} // namespace UnityBridge::UnityTextures
