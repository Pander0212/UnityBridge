// SPDX-License-Identifier: GPL-3.0-or-later
// ub-shader-convert <in> <out> [--replace]: ShaderGles3::ConvertFile on a serialized file (M4c), what
// the device does at import; checked against tools/research/add_gles3.py.
// ub-shader-convert --import <in> <out>: the device's entry point (engine/shader_import.h).
// ub-shader-convert --roundtrip <in>: reads every Shader object and the file and writes them back
// unchanged, which must give the same bytes.
#include "engine/shader_gles3.h"
#include "engine/shader_import.h"
#include "engine/unity_file.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

using namespace UnityBridge;

static std::vector<uint8_t> ReadFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

static int RoundTrip(const char* path) {
  const auto data = ReadFile(path);
  std::string error;
  const auto file = UnityFile::Parse(data.data(), data.size(), &error);
  if (!file) {
    std::fprintf(stderr, "%s: %s\n", path, error.c_str());
    return 1;
  }
  int shaders = 0, bad = 0;
  for (const auto& o : file->objects) {
    if (o.classId != 48) continue;
    ++shaders;
    const uint8_t* bytes = data.data() + file->dataOffset + o.byteStart;
    const auto v = TypeTree::Read(TypeTree::Shader2021_3(), bytes, o.byteSize, &error);
    if (!v || TypeTree::Write(*v) != std::vector<uint8_t>(bytes, bytes + o.byteSize)) {
      ++bad;
      std::fprintf(stderr, "shader %lld: %s\n", static_cast<long long>(o.pathId),
                   v ? "writes back different bytes" : error.c_str());
    }
  }
  const bool same = UnityFile::Rebuild(*file, data.data(), data.size(), {}) == data;
  std::printf("%s: Unity %s, %zu objects, %d shaders, %d not round-tripped, file %s\n", path,
              file->unityVersion.c_str(), file->objects.size(), shaders, bad, same ? "identical" : "DIFFERENT");
  return bad || !same;
}

int main(int argc, char** argv) {
  if (argc == 3 && !std::strcmp(argv[1], "--roundtrip")) return RoundTrip(argv[2]);
  if (argc == 4 && !std::strcmp(argv[1], "--import")) {
    char log[600];
    const int r = ub_shader_import(argv[2], argv[3], log, sizeof(log));
    std::printf("%d: %s\n", r, log);
    return r < 0;
  }
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <in> <out> [--replace] | --roundtrip <in>\n", argv[0]);
    return 2;
  }
  const bool replace = argc > 3 && !std::strcmp(argv[3], "--replace");
  const auto data = ReadFile(argv[1]);
  ShaderGles3::Stats stats;
  std::string error;
  const auto out = ShaderGles3::ConvertFile(data.data(), data.size(), replace, &stats, &error);
  std::fputs(stats.log.c_str(), stdout);
  if (!out) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
    return 1;
  }
  std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char*>(out->data()), long(out->size()));
  std::printf("%d shaders, %d programs added, %d not translated\n", stats.shaders, stats.programs, stats.failed);
  return 0;
}
