// SPDX-License-Identifier: GPL-3.0-or-later
// Texture2D objects: see unity_textures.h.
#include "engine/unity_textures.h"

#include "engine/unity_typetree.h"

#include <algorithm>
#include <cstring>

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
  const TypeTree::Trees* trees = TypeTree::TreesFor(file.unityVersion);
  for (const auto& o : file.objects) {
    if (o.classId != 28) continue;
    if (file.dataOffset + o.byteStart + o.byteSize > size) {
      ++census[-1];
      continue;
    }
    std::string error;
    if (!trees) {
      ++census[-1];
      continue;
    }
    const auto v = TypeTree::Read(trees->texture2d, data + file.dataOffset + o.byteStart, o.byteSize, &error);
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

namespace {

uint16_t Load16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// The four colours of a BC1 colour block (8 bytes); `bc1` allows its 3-colour + transparent mode.
void ColorBlock(const uint8_t* b, bool bc1, uint8_t out[16][4]) {
  const uint16_t c0 = Load16(b), c1 = Load16(b + 2);
  uint8_t palette[4][4];
  const auto expand = [](uint16_t c, uint8_t* rgb) {
    rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    rgb[2] = uint8_t((c & 31) * 255 / 31);
    rgb[3] = 255;
  };
  expand(c0, palette[0]);
  expand(c1, palette[1]);
  if (c0 > c1 || !bc1) {
    for (int k = 0; k < 3; ++k) {
      palette[2][k] = uint8_t((2 * palette[0][k] + palette[1][k]) / 3);
      palette[3][k] = uint8_t((palette[0][k] + 2 * palette[1][k]) / 3);
    }
    palette[2][3] = palette[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) palette[2][k] = uint8_t((palette[0][k] + palette[1][k]) / 2);
    palette[2][3] = 255;
    palette[3][0] = palette[3][1] = palette[3][2] = palette[3][3] = 0;
  }
  uint32_t bits = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
  for (int i = 0; i < 16; ++i, bits >>= 2) std::memcpy(out[i], palette[bits & 3], 4);
}

// The sixteen alpha values of a BC3 alpha block (8 bytes).
void AlphaBlock(const uint8_t* b, uint8_t out[16]) {
  uint8_t a[8];
  a[0] = b[0];
  a[1] = b[1];
  if (a[0] > a[1]) {
    for (int i = 1; i < 7; ++i) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
  } else {
    for (int i = 1; i < 5; ++i) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
    a[6] = 0;
    a[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i, bits >>= 3) out[i] = a[bits & 7];
}

// ---- BC7 (format 25): eight modes, after the D3D11 BC7 format description ----------------------

struct Bc7Mode {
  int subsets, partitionBits, rotationBits, indexSelectionBits, colorBits, alphaBits, endpointPBits, sharedPBits,
      indexBits, index2Bits;
};
const Bc7Mode kBc7Modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

const uint8_t kBc7Partition2[64][16] = {
    {0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1}, {0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1}, {0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1}, {0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1}, {0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1},
    {0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1}, {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
    {0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1}, {0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0}, {0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0},
    {0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0}, {0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0}, {0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0}, {0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1},
    {0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0}, {0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0}, {0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0}, {0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0},
    {0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0}, {0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0}, {0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0}, {0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0},
    {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1}, {0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1}, {0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0}, {0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0},
    {0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0}, {0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0}, {0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1}, {0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1},
    {0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0}, {0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0}, {0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0}, {0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0},
    {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0}, {0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1}, {0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1}, {0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
    {0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0}, {0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0}, {0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0}, {0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0},
    {0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1}, {0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0}, {0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0},
    {0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1}, {0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1}, {0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1}, {0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1},
    {0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1}, {0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0}, {0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0}, {0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1},
};

const uint8_t kBc7Partition3[64][16] = {
    {0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2}, {0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1}, {0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1}, {0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2}, {0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2}, {0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1}, {0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
    {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2}, {0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2}, {0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2}, {0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
    {0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2}, {0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2}, {0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2}, {0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
    {0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2}, {0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0}, {0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2}, {0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},
    {0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2}, {0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1}, {0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2}, {0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
    {0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0}, {0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2}, {0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0}, {0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},
    {0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2}, {0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2}, {0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1}, {0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
    {0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2}, {0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1}, {0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2}, {0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
    {0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0}, {0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0}, {0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0}, {0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
    {0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1}, {0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2}, {0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1}, {0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},
    {0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1}, {0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1}, {0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1}, {0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
    {0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2}, {0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1}, {0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2}, {0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},
    {0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2}, {0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2}, {0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2}, {0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
    {0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2}, {0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2}, {0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2}, {0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
    {0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1}, {0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2}, {0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2}, {0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0},
};

const uint8_t kBc7Anchor2[64] = {15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15, 15, 2, 8, 2, 2, 8, 8,15, 2, 8, 2, 2, 8, 8, 2, 2,
                                 15,15, 6, 8, 2, 8,15,15, 2, 8, 2, 2, 2,15,15, 6, 6, 2, 6, 8,15,15, 2, 2, 15,15,15,15,15, 2, 2,15};
const uint8_t kBc7Anchor3a[64] = {3, 3,15,15, 8, 3,15,15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8,15, 3, 3, 6,10, 5, 8, 8, 6, 8, 5,15,15,
                                  8,15, 3, 5, 6,10, 8,15, 15, 3,15, 5,15,15,15,15, 3,15, 5, 5, 5, 8, 5,10, 5,10, 8,13,15,12, 3, 3};
const uint8_t kBc7Anchor3b[64] = {15, 8, 8, 3,15,15, 3, 8, 15,15,15,15,15,15,15, 8, 15, 8,15, 3,15, 8,15, 8, 3,15, 6,10,15,15,10, 8,
                                  15, 3,15,10,10, 8, 9,10, 6,15, 8,15, 3, 6, 6, 8, 15, 3,15,15,15,15,15,15, 15,15,15,15, 3,15,15, 8};

const uint8_t kBc7Weights2[4] = {0, 21, 43, 64};
const uint8_t kBc7Weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
const uint8_t kBc7Weights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

class Bits128 {
public:
  explicit Bits128(const uint8_t* b) : b_(b) {}
  uint32_t Read(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i, ++pos_) v |= uint32_t((b_[pos_ >> 3] >> (pos_ & 7)) & 1) << i;
    return v;
  }
private:
  const uint8_t* b_;
  int pos_ = 0;
};

uint8_t Bc7Interpolate(uint8_t e0, uint8_t e1, uint32_t index, int bits) {
  const uint8_t w = bits == 2 ? kBc7Weights2[index] : bits == 3 ? kBc7Weights3[index] : kBc7Weights4[index];
  return uint8_t(((64 - w) * e0 + w * e1 + 32) >> 6);
}

void Bc7Block(const uint8_t* block, uint8_t out[16][4]) {
  int mode = 0;
  while (mode < 8 && !((block[0] >> mode) & 1)) ++mode;
  if (mode == 8) {  // reserved: transparent black, as the format description asks
    std::memset(out, 0, 16 * 4);
    return;
  }
  const Bc7Mode& m = kBc7Modes[mode];
  Bits128 bits(block);
  bits.Read(mode + 1);
  const uint32_t partition = bits.Read(m.partitionBits);
  const uint32_t rotation = bits.Read(m.rotationBits);
  const uint32_t indexSelection = bits.Read(m.indexSelectionBits);
  const int endpoints = m.subsets * 2;
  uint32_t color[6][4] = {};
  for (int c = 0; c < 3; ++c)
    for (int e = 0; e < endpoints; ++e) color[e][c] = bits.Read(m.colorBits);
  for (int e = 0; e < endpoints; ++e) color[e][3] = m.alphaBits ? bits.Read(m.alphaBits) : 255;
  uint32_t pbit[6] = {};
  bool havePBits = false;
  if (m.endpointPBits) {
    for (int e = 0; e < endpoints; ++e) pbit[e] = bits.Read(1);
    havePBits = true;
  } else if (m.sharedPBits) {
    for (int s = 0; s < m.subsets; ++s) pbit[2 * s] = pbit[2 * s + 1] = bits.Read(1);
    havePBits = true;
  }
  uint8_t ends[6][4];
  for (int e = 0; e < endpoints; ++e)
    for (int c = 0; c < 4; ++c) {
      const int n = (c < 3 ? m.colorBits : m.alphaBits) + (havePBits ? 1 : 0);
      if (c == 3 && !m.alphaBits) {
        ends[e][c] = 255;
        continue;
      }
      uint32_t v = havePBits ? (color[e][c] << 1 | pbit[e]) : color[e][c];
      v <<= 8 - n;
      ends[e][c] = uint8_t(v | (v >> n));
    }
  const uint8_t* subsetOf = m.subsets == 2 ? kBc7Partition2[partition] : m.subsets == 3 ? kBc7Partition3[partition] : nullptr;
  const auto isAnchor = [&](int i) {
    if (i == 0) return true;
    if (m.subsets == 2) return i == kBc7Anchor2[partition];
    if (m.subsets == 3) return i == kBc7Anchor3a[partition] || i == kBc7Anchor3b[partition];
    return false;
  };
  uint32_t index1[16], index2[16] = {};
  for (int i = 0; i < 16; ++i) index1[i] = bits.Read(isAnchor(i) ? m.indexBits - 1 : m.indexBits);
  if (m.index2Bits)
    for (int i = 0; i < 16; ++i) index2[i] = bits.Read(i == 0 ? m.index2Bits - 1 : m.index2Bits);
  for (int i = 0; i < 16; ++i) {
    const int s = subsetOf ? subsetOf[i] : 0;
    const uint8_t* e0 = ends[2 * s];
    const uint8_t* e1 = ends[2 * s + 1];
    uint8_t px[4];
    if (m.index2Bits) {
      const bool swap = indexSelection != 0;
      const uint32_t ci = swap ? index2[i] : index1[i], ai = swap ? index1[i] : index2[i];
      const int cb = swap ? m.index2Bits : m.indexBits, ab = swap ? m.indexBits : m.index2Bits;
      for (int c = 0; c < 3; ++c) px[c] = Bc7Interpolate(e0[c], e1[c], ci, cb);
      px[3] = Bc7Interpolate(e0[3], e1[3], ai, ab);
    } else {
      for (int c = 0; c < 4; ++c) px[c] = Bc7Interpolate(e0[c], e1[c], index1[i], m.indexBits);
    }
    if (rotation) std::swap(px[3], px[rotation - 1]);
    std::memcpy(out[i], px, 4);
  }
}

int BlockBytes(int format) { return format == 10 ? 8 : (format == 12 || format == 25) ? 16 : 0; }

size_t BlockSize(int format, int width, int height) {
  return size_t((width + 3) / 4) * size_t((height + 3) / 4) * size_t(BlockBytes(format));
}

} // namespace

bool DecodeBlocks(int format, const uint8_t* src, size_t size, int width, int height, uint8_t* rgba) {
  const int bytes = BlockBytes(format);
  if (!bytes || width <= 0 || height <= 0 || size < BlockSize(format, width, height)) return false;
  const int bw = (width + 3) / 4, bh = (height + 3) / 4;
  for (int by = 0; by < bh; ++by)
    for (int bx = 0; bx < bw; ++bx) {
      const uint8_t* block = src + (size_t(by) * size_t(bw) + size_t(bx)) * size_t(bytes);
      uint8_t texel[16][4];
      if (format == 10) {
        ColorBlock(block, true, texel);
      } else if (format == 25) {
        Bc7Block(block, texel);
      } else {
        uint8_t alpha[16];
        AlphaBlock(block, alpha);
        ColorBlock(block + 8, false, texel);
        for (int i = 0; i < 16; ++i) texel[i][3] = alpha[i];
      }
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
          const int px = bx * 4 + x, py = by * 4 + y;
          if (px < width && py < height) std::memcpy(rgba + (size_t(py) * size_t(width) + size_t(px)) * 4, texel[y * 4 + x], 4);
        }
    }
  return true;
}

bool ToRgba32(TypeTree::Value& tex, TextureIO& io, std::string* error) {
  TypeTree::Value* format = tex.Field("m_TextureFormat");
  if (!format || BlockBytes(int(format->Int())) == 0) return false;
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  TypeTree::Value* name = tex.Field("m_Name");
  TypeTree::Value* width = tex.Field("m_Width");
  TypeTree::Value* height = tex.Field("m_Height");
  TypeTree::Value* mips = tex.Field("m_MipCount");
  TypeTree::Value* images = tex.Field("m_ImageCount");
  TypeTree::Value* complete = tex.Field("m_CompleteImageSize");
  TypeTree::Value* data = tex.Field("image data");
  TypeTree::Value* stream = tex.Field("m_StreamData");
  if (!name || !width || !height || !mips || !images || !complete || !data || !stream) return fail("fields missing");
  const int f = int(format->Int()), w = int(width->Int()), h = int(height->Int()), m = int(mips->Int());
  if (w <= 0 || h <= 0 || m <= 0 || m > 16 || images->Int() != 1) return fail(name->bytes + ": not a plain 2D texture");
  size_t blockTotal = 0, rgbaTotal = 0;
  for (int level = 0; level < m; ++level) {
    const int lw = std::max(1, w >> level), lh = std::max(1, h >> level);
    blockTotal += BlockSize(f, lw, lh);
    rgbaTotal += size_t(lw) * size_t(lh) * 4;
  }
  // The compressed pixels: in the object, or streamed from a resource file.
  std::vector<uint8_t> blocks;
  TypeTree::Value* offset = stream->Field("offset");
  TypeTree::Value* size = stream->Field("size");
  TypeTree::Value* path = stream->Field("path");
  if (!offset || !size || !path) return fail("m_StreamData fields missing");
  const bool streamed = !path->bytes.empty() && size->Int() > 0;
  if (streamed) {
    auto read = io.read ? io.read(path->bytes, uint64_t(offset->Int()), uint32_t(size->Int())) : std::nullopt;
    if (!read) return fail(name->bytes + ": cannot read " + path->bytes);
    blocks = std::move(*read);
  } else {
    blocks.assign(data->bytes.begin(), data->bytes.end());
  }
  if (blocks.size() < blockTotal) return fail(name->bytes + ": " + std::to_string(blocks.size()) + " bytes, " +
                                              std::to_string(blockTotal) + " expected");
  std::vector<uint8_t> rgba(rgbaTotal);
  size_t in = 0, out = 0;
  for (int level = 0; level < m; ++level) {
    const int lw = std::max(1, w >> level), lh = std::max(1, h >> level);
    if (!DecodeBlocks(f, blocks.data() + in, blocks.size() - in, lw, lh, rgba.data() + out))
      return fail(name->bytes + ": mip " + std::to_string(level) + " does not decode");
    in += BlockSize(f, lw, lh);
    out += size_t(lw) * size_t(lh) * 4;
  }
  if (streamed) {
    static const uint8_t zeros[16] = {};
    const size_t pad = size_t((16 - io.written % 16) % 16);  // Unity aligns resource data to 16
    if (!io.write || (pad && !io.write(zeros, pad))) return fail(name->bytes + ": cannot write " + io.path);
    io.written += pad;
    if (!io.write(rgba.data(), rgba.size())) return fail(name->bytes + ": cannot write " + io.path);
    offset->SetInt(int64_t(io.written));
    size->SetInt(int64_t(rgba.size()));
    path->bytes = io.path;
    io.written += rgba.size();
  } else {
    data->bytes.assign(reinterpret_cast<const char*>(rgba.data()), rgba.size());
  }
  format->SetInt(4);  // RGBA32
  complete->SetInt(int64_t(rgbaTotal));
  return true;
}

} // namespace UnityBridge::UnityTextures
