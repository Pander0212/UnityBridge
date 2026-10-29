// SPDX-License-Identifier: GPL-3.0-or-later
// M4d host checks without game data: the BuildSettings graphics-API rewrite on a synthetic object
// (written and read back through the 2021.3 type tree) and the LZ4 helpers.
#include "engine/shader_gles3.h"
#include "engine/unity_textures.h"
#include "engine/unity_typetree.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace UnityBridge;

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

// A BuildSettings object as a desktop player has it: version string, graphics APIs `apis`.
static std::vector<uint8_t> MakeSettings(std::initializer_list<int64_t> apis) {
  auto v = TypeTree::Zero(TypeTree::BuildSettings2021_3());
  v.Field("m_Version")->bytes = "2021.3.18f1";
  auto* list = v.Field("m_GraphicsAPIs");
  for (const auto api : apis) {
    auto e = TypeTree::NewElement(*list);
    e.SetInt(api);
    list->items.push_back(e);
  }
  auto* scenes = v.Field("scenes");
  auto scene = TypeTree::NewElement(*scenes);
  scene.bytes = "Assets/Scenes/Main.unity";
  scenes->items.push_back(scene);
  return TypeTree::Write(v);
}

static std::vector<int64_t> Apis(const std::vector<uint8_t>& bytes) {
  std::string error;
  auto v = TypeTree::Read(TypeTree::BuildSettings2021_3(), bytes.data(), bytes.size(), &error);
  std::vector<int64_t> out;
  if (!v) return out;
  for (const auto& a : v->Field("m_GraphicsAPIs")->items) out.push_back(a.Int());
  return out;
}

int main() {
  // A Windows build's list (Direct3D11) becomes [OpenGLES3]; the rest of the object stays.
  const auto windows = MakeSettings({2});
  CHECK(Apis(windows) == std::vector<int64_t>({2}));
  std::string error;
  auto v = TypeTree::Read(TypeTree::BuildSettings2021_3(), windows.data(), windows.size(), &error);
  CHECK(v.has_value());
  CHECK(TypeTree::Write(*v) == windows);  // round trip
  ShaderGles3::Stats stats;
  CHECK(ShaderGles3::SetAndroidGraphicsApis(*v, &stats));
  CHECK(stats.graphicsApis);
  const auto android = TypeTree::Write(*v);
  CHECK(Apis(android) == std::vector<int64_t>({ShaderGles3::kApiGles3}));
  CHECK(android.size() == windows.size());  // one int for one int
  auto back = TypeTree::Read(TypeTree::BuildSettings2021_3(), android.data(), android.size(), &error);
  CHECK(back && back->Field("m_Version")->bytes == "2021.3.18f1");
  CHECK(back && back->Field("scenes")->items.size() == 1 &&
        back->Field("scenes")->items[0].bytes == "Assets/Scenes/Main.unity");

  // An Android build's list is left alone; several desktop APIs become one.
  const auto own = MakeSettings({ShaderGles3::kApiGles3, ShaderGles3::kApiGles2});
  auto o = TypeTree::Read(TypeTree::BuildSettings2021_3(), own.data(), own.size(), &error);
  ShaderGles3::Stats unchanged;
  CHECK(o && !ShaderGles3::SetAndroidGraphicsApis(*o, &unchanged) && !unchanged.graphicsApis);
  const auto desktop = MakeSettings({2, 18});
  auto d = TypeTree::Read(TypeTree::BuildSettings2021_3(), desktop.data(), desktop.size(), &error);
  CHECK(d && ShaderGles3::SetAndroidGraphicsApis(*d, &stats));
  CHECK(d && Apis(TypeTree::Write(*d)) == std::vector<int64_t>({ShaderGles3::kApiGles3}));

  // LZ4: a literal-only block decodes to its input, at every length class.
  for (const size_t n : {size_t {0}, size_t {1}, size_t {14}, size_t {15}, size_t {16}, size_t {269}, size_t {270},
                         size_t {100000}}) {
    std::vector<uint8_t> data(n);
    for (size_t i = 0; i < n; ++i) data[i] = static_cast<uint8_t>(i * 31 + 7);
    const auto packed = ShaderGles3::Lz4Literals(data);
    const auto out = ShaderGles3::Lz4Decompress(packed.data(), packed.size(), n);
    CHECK(out && *out == data);
  }
  // A block with a match (offset 1, overlapping): "a" then 4 + 3 copies.
  const uint8_t block[] = {0x13, 'a', 0x01, 0x00, 0x00};  // 1 literal, match length 3 + 4, then 0 literals
  const auto m = ShaderGles3::Lz4Decompress(block, sizeof(block), 8);
  CHECK(m && *m == std::vector<uint8_t>(8, 'a'));
  CHECK(!ShaderGles3::Lz4Decompress(block, sizeof(block), 9));  // wrong size

  // DXT1 (BC1): red and blue end points, index 0 in the top row, 1 below, 2 and 3 (thirds between).
  {
    const uint8_t block[8] = {0x00, 0xf8, 0x1f, 0x00, 0x00, 0x55, 0xaa, 0xff};
    uint8_t rgba[16 * 4];
    CHECK(UnityTextures::DecodeBlocks(10, block, sizeof(block), 4, 4, rgba));
    CHECK(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);            // row 0: red
    CHECK(rgba[16] == 0 && rgba[18] == 255 && rgba[19] == 255);                          // row 1: blue
    CHECK(rgba[32] == 170 && rgba[34] == 85);                                            // row 2: 2/3 red
    CHECK(rgba[48] == 85 && rgba[50] == 170);                                            // row 3: 1/3 red
    CHECK(!UnityTextures::DecodeBlocks(10, block, 7, 4, 4, rgba));                       // too short
    // DXT5 (BC3): alpha 255..0 in eight steps, index 1 everywhere -> alpha 0; colour all red.
    const uint8_t dxt5[16] = {0xff, 0x00, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0x00, 0xf8, 0x00, 0xf8, 0, 0, 0, 0};
    CHECK(UnityTextures::DecodeBlocks(12, dxt5, sizeof(dxt5), 4, 4, rgba));
    CHECK(rgba[0] == 255 && rgba[3] == 0 && rgba[63] == 0);
  }

  // BC7: one block of each mode (random bits, the mode set), against texture2ddecoder (UnityPy's
  // decoder); a run of 100,000 random blocks matched it in all modes when the decoder was written.
  {
    struct Vector {
      uint8_t block[16];
      uint8_t rgba[64];
    };
    static const Vector vectors[] = {
      {{0x53,0xf2,0x26,0x65,0xa6,0x0c,0x12,0xd2,0x89,0x18,0x5d,0x95,0x0e,0xe8,0x81,0x36},
       {82,49,62,255,65,49,42,255,116,49,106,255,65,49,42,255,99,89,187,255,57,107,74,255,115,82,231,255,115,82,231,255,99,89,187,255,65,103,96,255,91,93,165,255,115,82,231,255,86,90,147,255,56,134,186,255,56,134,186,255,148,0,66,255}},
      {{0x0a,0x16,0x6f,0x6b,0x11,0x3d,0x17,0x8d,0x6c,0x0f,0xd3,0x90,0x1f,0xf2,0x39,0xa1},
       {90,70,54,255,138,74,72,255,203,181,190,255,154,100,100,255,178,152,140,255,106,22,14,255,171,129,133,255,219,207,219,255,112,90,75,255,106,22,14,255,171,129,133,255,122,48,43,255,112,90,75,255,203,181,190,255,154,100,100,255,187,155,161,255}},
      {{0xa4,0x95,0xf2,0x0f,0x93,0x95,0x65,0x0c,0xf9,0x38,0x0b,0x8e,0xdb,0x22,0x4a,0x6b},
       {82,128,76,255,144,73,76,255,33,206,115,255,87,141,96,255,82,90,57,255,247,148,181,255,206,145,122,255,247,148,181,255,82,128,76,255,206,145,122,255,164,143,59,255,164,143,59,255,82,128,76,255,144,73,76,255,33,206,115,255,198,8,57,255}},
      {{0x28,0x8a,0x1e,0x92,0x4e,0x8f,0xd0,0xae,0x2e,0x1a,0x94,0x92,0xa3,0x30,0x5f,0x18},
       {68,122,22,255,37,219,41,255,56,85,24,255,44,203,52,255,37,219,41,255,43,46,25,255,44,203,52,255,68,122,22,255,31,9,27,255,59,171,75,255,56,85,24,255,44,203,52,255,37,219,41,255,43,46,25,255,44,203,52,255,68,122,22,255}},
      {{0x90,0xb6,0x10,0x90,0x0f,0x9e,0x34,0x7f,0xae,0x88,0x6d,0xc6,0x50,0x77,0x95,0xec},
       {142,24,164,164,80,9,99,99,161,28,185,164,122,19,143,99,100,14,120,36,161,28,185,36,100,14,120,36,142,24,164,227,41,0,57,36,61,5,78,164,80,9,99,164,142,24,164,164,161,28,185,227,161,28,185,164,122,19,143,227,41,0,57,36}},
      {{0x60,0x5c,0x4c,0x3f,0xcb,0x2e,0xb2,0xc7,0x3e,0x14,0x93,0x4c,0x86,0x7e,0xe0,0x57},
       {217,227,191,140,217,179,139,48,236,227,191,140,196,251,217,185,196,203,165,93,177,203,165,93,177,251,217,185,217,203,165,93,236,227,191,140,236,203,165,93,196,251,217,185,177,227,191,140,177,203,165,93,217,227,191,140,217,203,165,93,217,251,217,185}},
      {{0xc0,0x72,0x49,0x9b,0xfa,0x12,0x1e,0x83,0x6b,0x2a,0xc1,0x57,0x26,0xee,0x7d,0x6b},
       {161,149,131,23,151,141,117,21,117,115,69,15,185,167,165,28,195,175,180,30,101,103,46,12,143,135,106,20,161,149,131,23,151,141,117,21,185,167,165,28,83,89,20,9,83,89,20,9,93,97,35,10,143,135,106,20,109,109,57,13,151,141,117,21}},
      {{0x80,0xf6,0xab,0x13,0xc3,0x8e,0x92,0xca,0xe0,0xd1,0x50,0x57,0xb1,0x59,0x98,0x7f},
       {125,134,85,166,183,72,83,117,170,70,42,146,155,202,164,60,140,167,124,114,155,202,164,60,170,70,42,146,170,70,42,146,158,69,4,174,155,202,164,60,140,167,124,114,183,72,83,117,195,73,121,89,195,73,121,89,170,235,203,8,140,167,124,114}},
    };
    for (const auto& v : vectors) {
      uint8_t rgba[64];
      CHECK(UnityTextures::DecodeBlocks(25, v.block, 16, 4, 4, rgba));
      CHECK(std::memcmp(rgba, v.rgba, 64) == 0);
    }
  }

  if (g_failures) return 1;
  std::printf("unity_assets_test: all checks passed\n");
  return 0;
}
