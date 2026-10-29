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

int BlockBytes(int format) { return format == 10 ? 8 : format == 12 ? 16 : 0; }

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
