// SPDX-License-Identifier: GPL-3.0-or-later
// ub-shader-xlat <case dir>: translates one D3D11 vertex + pixel program pair with ShaderXlat (M4b).
// The case holds vs.dxbc, ps.dxbc (no RDEF, as in a player build) and vs.refl, ps.refl, Unity's
// parameter tables as text, one entry a line:
//   cbuffer <name> <register> <size>
//   var <name> <type> <rows> <columns> <isMatrix> <arraySize> <offset>     (of the cbuffer above)
//   texture <name> <register> <sampler register|-1> <dim> <multisampled>
// Writes out.glsl and out.params (what HLSLcc reports for the GLSL side) into the case directory.
#include "engine/shader_xlat.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

using namespace UnityBridge::ShaderXlat;

static std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

static Reflection ReadReflection(const std::string& path) {
  Reflection r;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream in(line);
    std::string kind;
    in >> kind;
    if (kind == "cbuffer") {
      ConstantBuffer cb;
      in >> cb.name >> cb.bindPoint >> cb.size;
      r.constantBuffers.push_back(cb);
    } else if (kind == "var" && !r.constantBuffers.empty()) {
      Variable v;
      int type, matrix;
      in >> v.name >> type >> v.rows >> v.columns >> matrix >> v.arraySize >> v.offset;
      v.type = static_cast<ParamType>(type);
      v.isMatrix = matrix != 0;
      r.constantBuffers.back().variables.push_back(v);
    } else if (kind == "texture") {
      Texture t;
      int dim, ms;
      in >> t.name >> t.bindPoint >> t.samplerBindPoint >> dim >> ms;
      t.dim = static_cast<TextureDim>(dim);
      t.multisampled = ms != 0;
      r.textures.push_back(t);
    }
  }
  return r;
}

static std::vector<uint8_t> Prepare(const std::string& dir, const char* stage) {
  const auto dxbc = ReadFile(dir + "/" + stage + ".dxbc");
  const auto model = ShaderModel(dxbc);
  if (!model) return {};
  auto reflection = ReadReflection(dir + "/" + stage + ".refl");
  std::string error, notes;
  if (!CompleteReflection(reflection, dxbc, &error, &notes)) {
    std::fprintf(stderr, "%s %s: %s\n", dir.c_str(), stage, error.c_str());
    return {};
  }
  if (!notes.empty()) std::fprintf(stderr, "%s %s: stand-ins: %s", dir.c_str(), stage, notes.c_str());
  return WithRdef(dxbc, BuildRdef(reflection, model->first, model->second));
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <case dir> [flags]\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const uint32_t flags = argc > 2 ? static_cast<uint32_t>(std::stoul(argv[2], nullptr, 0)) : DefaultFlags();
  const auto vs = Prepare(dir, "vs"), ps = Prepare(dir, "ps");
  if (vs.empty() || ps.empty()) {
    std::fprintf(stderr, "%s: no DXBC with a shader model\n", dir.c_str());
    return 1;
  }
  std::string error;
  const auto program = Translate(vs, ps, flags, &error);
  if (!program) {
    std::fprintf(stderr, "%s: %s\n", dir.c_str(), error.c_str());
    return 1;
  }
  std::ofstream(dir + "/out.glsl") << program->source;
  std::ofstream params(dir + "/out.params");
  for (const auto& b : program->buffers) {
    params << "buffer " << b.name << " size " << b.size << " binding " << b.binding << "\n";
    for (const auto& c : b.constants)
      params << "  " << c.name << " @" << c.offset << " type " << c.type << " " << c.rows << "x" << c.columns
             << (c.isMatrix ? " matrix" : "") << " array " << c.arraySize << "\n";
  }
  for (const auto& t : program->textures)
    params << "texture " << t.name << " unit " << t.unit << " sampler " << t.sampler << " dim " << t.dim << "\n";
  for (const auto& i : program->inputs) params << "input " << i.name << " " << i.location << "\n";
  if (!program->diagnostics.empty()) std::fprintf(stderr, "%s", program->diagnostics.c_str());
  return 0;
}
