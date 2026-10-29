// SPDX-License-Identifier: GPL-3.0-or-later
// M4d host checks without game data: the BuildSettings graphics-API rewrite on a synthetic object
// (written and read back through the 2021.3 type tree) and the LZ4 helpers.
#include "engine/shader_gles3.h"
#include "engine/unity_textures.h"
#include "engine/unity_typetree.h"

#include <cstdio>
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

  if (g_failures) return 1;
  std::printf("unity_assets_test: all checks passed\n");
  return 0;
}
