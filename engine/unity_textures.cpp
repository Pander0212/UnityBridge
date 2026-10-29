// SPDX-License-Identifier: GPL-3.0-or-later
// Texture2D objects: see unity_textures.h.
#include "engine/unity_textures.h"

#include "engine/unity_typetree.h"

namespace UnityBridge::UnityTextures {

std::string FormatName(int format) {
  static const std::map<int, const char*> names = {
      {1, "Alpha8"}, {2, "ARGB4444"}, {3, "RGB24"}, {4, "RGBA32"}, {5, "ARGB32"}, {7, "RGB565"},
      {9, "R16"}, {10, "DXT1"}, {12, "DXT5"}, {13, "RGBA4444"}, {14, "BGRA32"}, {15, "RHalf"},
      {16, "RGHalf"}, {17, "RGBAHalf"}, {18, "RFloat"}, {19, "RGFloat"}, {20, "RGBAFloat"},
      {22, "RGB9e5Float"}, {24, "BC6H"}, {25, "BC7"}, {26, "BC4"}, {27, "BC5"}, {28, "DXT1Crunched"},
      {29, "DXT5Crunched"}, {30, "PVRTC_RGB2"}, {32, "PVRTC_RGB4"}, {34, "ETC_RGB4"}, {45, "ETC2_RGB"},
      {47, "ETC2_RGBA8"}, {48, "ASTC_4x4"}, {49, "ASTC_5x5"}, {50, "ASTC_6x6"}, {51, "ASTC_8x8"},
      {52, "ASTC_10x10"}, {53, "ASTC_12x12"}, {62, "RG16"}, {63, "R8"}, {64, "ETC_RGB4Crunched"},
      {65, "ETC2_RGBA8Crunched"},
  };
  const auto it = names.find(format);
  return it != names.end() ? it->second : "format " + std::to_string(format);
}

std::map<int, int> Census(const UnityFile::File& file, const uint8_t* data, size_t size) {
  std::map<int, int> census;
  for (const auto& o : file.objects) {
    if (o.classId != 28) continue;
    if (file.dataOffset + o.byteStart + o.byteSize > size) {
      ++census[-1];
      continue;
    }
    std::string error;
    const auto v = TypeTree::Read(TypeTree::Texture2D2021_3(), data + file.dataOffset + o.byteStart, o.byteSize, &error);
    const TypeTree::Value* format = v ? v->Field("m_TextureFormat") : nullptr;
    ++census[format ? static_cast<int>(format->Int()) : -1];
  }
  return census;
}

std::string CensusLine(const std::map<int, int>& census) {
  int total = 0;
  for (const auto& [format, count] : census) total += count;
  if (total == 0) return "";
  std::string line = std::to_string(total) + " textures:";
  bool first = true;
  for (const auto& [format, count] : census) {
    line += (first ? " " : ", ") + (format < 0 ? std::string("unreadable") : FormatName(format)) + " " +
            std::to_string(count);
    first = false;
  }
  return line;
}

} // namespace UnityBridge::UnityTextures
