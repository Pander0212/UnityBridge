// SPDX-License-Identifier: GPL-3.0-or-later
// D3D11 shader programs -> GLSL ES 3.0 through HLSLcc: see shader_xlat.h.
//
// The RDEF layout is D3D11's shader reflection chunk as HLSLcc reads it (build/hlslcc/src/reflect.cpp):
// a header of 7 dwords, the resource bindings (8 dwords each), the constant buffers (6 dwords each),
// their variables (6 dwords, 10 from shader model 5), the type descriptors (16 bytes) and the strings.
#include "engine/shader_xlat.h"

#include "hlslcc.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace UnityBridge::ShaderXlat {
namespace {

uint32_t Fourcc(const char (&s)[5]) {
  return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
         uint32_t(uint8_t(s[3])) << 24;
}

uint32_t Load32(const std::vector<uint8_t>& b, size_t at) {
  uint32_t v = 0;
  if (at + 4 <= b.size()) std::memcpy(&v, b.data() + at, 4);
  return v;
}

// A growing dword stream with a string table at the end, offsets relative to the chunk start.
class Writer {
public:
  size_t Here() const { return words_.size() * 4; }
  size_t Put(uint32_t v) {
    words_.push_back(v);
    return words_.size() - 1;
  }
  void Set(size_t word, uint32_t v) { words_[word] = v; }
  void PutHalves(uint16_t a, uint16_t b) { Put(uint32_t(a) | uint32_t(b) << 16); }
  // Records that dword `word` must hold the offset of `text` once the strings are placed.
  void PutString(const std::string& text) { strings_.push_back({Put(0), text}); }
  std::vector<uint8_t> Finish() {
    std::vector<uint8_t> out(words_.size() * 4);
    std::memcpy(out.data(), words_.data(), out.size());
    for (const auto& [word, text] : strings_) {
      const uint32_t at = static_cast<uint32_t>(out.size());
      out.insert(out.end(), text.begin(), text.end());
      out.push_back(0);
      std::memcpy(out.data() + word * 4, &at, 4);
    }
    while (out.size() % 4) out.push_back(0xab);  // fxc pads with 0xab
    return out;
  }

private:
  std::vector<uint32_t> words_;
  std::vector<std::pair<size_t, std::string>> strings_;
};

// D3D_SHADER_VARIABLE_TYPE for a Unity parameter type. Half is float in a D3D11 constant buffer.
uint16_t VariableType(ParamType t) {
  switch (t) {
  case ParamType::Int: case ParamType::Short: return SVT_INT;
  case ParamType::UInt: return SVT_UINT;
  case ParamType::Bool: return SVT_BOOL;
  default: return SVT_FLOAT;
  }
}

// Bytes a variable occupies: D3D11 packing, 16-byte registers, arrays padded per element.
uint32_t VariableSize(const Variable& v) {
  const uint32_t regs = v.isMatrix ? uint32_t(v.rows) : 1;
  const uint32_t last = v.isMatrix ? (regs - 1) * 16 + uint32_t(v.columns) * 4 : uint32_t(v.columns) * 4;
  const uint32_t n = v.arraySize > 0 ? uint32_t(v.arraySize) : 1;
  return (n - 1) * regs * 16 + last;
}

// D3D_SRV_DIMENSION for a Unity texture dimension.
uint32_t SrvDimension(const Texture& t) {
  switch (t.dim) {
  case TextureDim::Tex3D: return 8;
  case TextureDim::Cube: return 9;
  case TextureDim::Tex2DArray: return t.multisampled ? 7 : 5;
  case TextureDim::CubeArray: return 10;
  default: return t.multisampled ? 6 : 4;
  }
}

constexpr uint32_t kSitCbuffer = 0, kSitTexture = 2, kSitSampler = 3, kReturnFloat = 5;

} // namespace

std::vector<uint8_t> BuildRdef(const Reflection& r, int major, int minor) {
  Writer w;
  const size_t cbCount = w.Put(0), cbOffset = w.Put(0), bindCount = w.Put(0), bindOffset = w.Put(0);
  const uint32_t typeBits = 0xfffe;  // vertex/pixel does not matter to HLSLcc; the version does
  w.Put(typeBits << 16 | uint32_t(major) << 4 | uint32_t(minor));
  w.Put(0);                          // compile flags
  w.PutString("UnityBridge");        // creator

  // Resource bindings: constant buffers, then each texture and its sampler.
  w.Set(bindOffset, static_cast<uint32_t>(w.Here()));
  uint32_t bindings = 0;
  auto binding = [&](const std::string& name, uint32_t type, uint32_t ret, uint32_t dim, uint32_t samples,
                     int point, uint32_t flags) {
    w.PutString(name);
    w.Put(type);
    w.Put(ret);
    w.Put(dim);
    w.Put(samples);
    w.Put(static_cast<uint32_t>(point));
    w.Put(1);  // bind count
    w.Put(flags);
    if (major > 5 || (major == 5 && minor >= 1)) { w.Put(0); w.Put(0); }  // space, range id
    ++bindings;
  };
  for (const auto& cb : r.constantBuffers) binding(cb.name, kSitCbuffer, 0, 0, 0, cb.bindPoint, 0);
  for (const auto& t : r.textures) {
    binding(t.name, kSitTexture, kReturnFloat, SrvDimension(t), t.multisampled ? 0 : 0xffffffffu, t.bindPoint,
            0xc /* four components */);
    if (t.samplerBindPoint >= 0) binding("sampler" + t.name, kSitSampler, 0, 0, 0, t.samplerBindPoint, 0);
  }
  for (const auto& s : r.samplers) {
    bool taken = false;
    for (const auto& t : r.textures) taken = taken || t.samplerBindPoint == s.bindPoint;
    if (!taken) binding(s.name, kSitSampler, 0, 0, 0, s.bindPoint, 0);
  }
  w.Set(bindCount, bindings);

  // Constant buffer descriptors, their variables and each variable's type.
  w.Set(cbCount, static_cast<uint32_t>(r.constantBuffers.size()));
  w.Set(cbOffset, static_cast<uint32_t>(w.Here()));
  std::vector<size_t> varOffsetWord;
  for (const auto& cb : r.constantBuffers) {
    w.PutString(cb.name);
    w.Put(static_cast<uint32_t>(cb.variables.size()));
    varOffsetWord.push_back(w.Put(0));
    w.Put(static_cast<uint32_t>((cb.size + 15) & ~15));
    w.Put(0);  // flags
    w.Put(0);  // D3D_CT_CBUFFER
  }
  std::vector<std::vector<size_t>> typeOffsetWord(r.constantBuffers.size());
  for (size_t c = 0; c < r.constantBuffers.size(); ++c) {
    w.Set(varOffsetWord[c], static_cast<uint32_t>(w.Here()));
    for (const auto& v : r.constantBuffers[c].variables) {
      w.PutString(v.name);
      w.Put(static_cast<uint32_t>(v.offset));
      w.Put(VariableSize(v));
      w.Put(2);  // D3D_SVF_USED
      typeOffsetWord[c].push_back(w.Put(0));
      w.Put(0);  // no default value
      if (major >= 5) { w.Put(0xffffffffu); w.Put(0); w.Put(0xffffffffu); w.Put(0); }
    }
  }
  for (size_t c = 0; c < r.constantBuffers.size(); ++c) {
    for (size_t i = 0; i < r.constantBuffers[c].variables.size(); ++i) {
      const auto& v = r.constantBuffers[c].variables[i];
      w.Set(typeOffsetWord[c][i], static_cast<uint32_t>(w.Here()));
      const uint16_t cls = v.isMatrix ? SVC_MATRIX_COLUMNS : (v.columns == 1 && v.rows == 1 ? SVC_SCALAR : SVC_VECTOR);
      w.PutHalves(cls, VariableType(v.type));
      w.PutHalves(static_cast<uint16_t>(v.rows), static_cast<uint16_t>(v.columns));
      w.PutHalves(static_cast<uint16_t>(v.arraySize), 0);  // elements, members
      w.Put(0);                                             // member offset
    }
  }
  return w.Finish();
}

namespace {

// The resources a program declares, from its SHDR/SHEX token stream.
struct Declared {
  std::map<int, int> cbuffers;  // register -> size in 16-byte registers
  std::vector<int> samplers, textures;
  bool unsupported = false;     // UAVs, raw/structured buffers, shader model 5.1 layouts
  bool ok = false;
};

Declared Declarations(const std::vector<uint8_t>& dxbc) {
  Declared d;
  if (dxbc.size() < 32 || Load32(dxbc, 0) != Fourcc("DXBC")) return d;
  const uint32_t n = Load32(dxbc, 28);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t at = Load32(dxbc, 32 + 4 * i);
    const uint32_t cc = Load32(dxbc, at);
    if (cc != Fourcc("SHDR") && cc != Fourcc("SHEX")) continue;
    const size_t start = size_t(at) + 8, chunkEnd = start + Load32(dxbc, at + 4);
    const uint32_t version = Load32(dxbc, start);
    const size_t end = std::min(chunkEnd, start + size_t(Load32(dxbc, start + 4)) * 4);
    if (end > dxbc.size()) return d;
    const bool sm51 = ((version >> 4) & 0xf) == 5 && (version & 0xf) >= 1;
    size_t pos = start + 8;
    while (pos + 4 <= end) {
      const uint32_t token = Load32(dxbc, pos);
      const uint32_t opcode = token & 0x7ff;
      size_t length = (token >> 24) & 0x7f;
      if (opcode == 0x35) length = Load32(dxbc, pos + 4);  // custom data: its length follows
      if (length == 0) return d;
      // The operand follows the opcode token and its extended tokens.
      size_t operand = pos + 4;
      for (uint32_t ext = token; (ext & 0x80000000u) && operand < end; operand += 4) ext = Load32(dxbc, operand);
      const uint32_t op = Load32(dxbc, operand);
      size_t index = operand + 4;
      if (op & 0x80000000u) index += 4;  // extended operand
      const int reg = static_cast<int>(Load32(dxbc, index));
      switch (opcode) {
      case 0x59: d.cbuffers[reg] = static_cast<int>(Load32(dxbc, index + 4)); break;  // dcl_constantbuffer
      case 0x5a: d.samplers.push_back(reg); break;                                   // dcl_sampler
      case 0x58: d.textures.push_back(reg); break;                                   // dcl_resource
      case 0x9c: case 0x9d: case 0x9e: case 0xa1: case 0xa2: d.unsupported = true; break;
      default: break;
      }
      if ((opcode == 0x58 || opcode == 0x59 || opcode == 0x5a) && sm51) d.unsupported = true;
      pos += length * 4;
    }
    d.ok = true;
    return d;
  }
  return d;
}

} // namespace

bool CompleteReflection(Reflection& r, const std::vector<uint8_t>& dxbc, std::string* error, std::string* notes) {
  const Declared d = Declarations(dxbc);
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (!d.ok) return fail("no SHDR/SHEX token stream");
  if (d.unsupported) return fail("UAVs, buffer resources or shader model 5.1 declarations");
  for (const int t : d.textures) {
    bool found = false;
    for (const auto& x : r.textures) found = found || x.bindPoint == t;
    if (!found) return fail("texture t" + std::to_string(t) + " is not in Unity's tables");
  }
  for (const int s : d.samplers) {
    bool found = false;
    for (const auto& x : r.textures) found = found || x.samplerBindPoint == s;
    for (const auto& x : r.samplers) found = found || x.bindPoint == s;
    if (!found) {
      r.samplers.push_back({"UnityBridge_s" + std::to_string(s), s});
      if (notes) *notes += "sampler s" + std::to_string(s) + " added\n";
    }
  }
  for (const auto& [reg, size] : d.cbuffers) {
    bool found = false;
    for (auto& x : r.constantBuffers) found = found || x.bindPoint == reg;
    if (found) continue;  // Unity's table lists the variables the program uses
    Variable all;  // the whole buffer as one float4 array, so every offset has a variable
    all.name = "UnityBridge_cb" + std::to_string(reg) + "_data";
    all.arraySize = size > 1 ? size : 0;
    r.constantBuffers.push_back({"UnityBridge_cb" + std::to_string(reg), reg, size * 16, {all}});
    if (notes) *notes += "constant buffer cb" + std::to_string(reg) + " added\n";
  }
  return true;
}

std::optional<std::pair<int, int>> ShaderModel(const std::vector<uint8_t>& dxbc) {
  if (dxbc.size() < 32 || Load32(dxbc, 0) != Fourcc("DXBC")) return std::nullopt;
  const uint32_t n = Load32(dxbc, 28);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t at = Load32(dxbc, 32 + 4 * i);
    const uint32_t cc = Load32(dxbc, at);
    if (cc == Fourcc("SHDR") || cc == Fourcc("SHEX")) {
      const uint32_t version = Load32(dxbc, at + 8);
      return std::make_pair(int((version >> 4) & 0xf), int(version & 0xf));
    }
  }
  return std::nullopt;
}

std::vector<uint8_t> WithRdef(const std::vector<uint8_t>& dxbc, const std::vector<uint8_t>& rdef) {
  if (dxbc.size() < 32 || Load32(dxbc, 0) != Fourcc("DXBC")) return {};
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> chunks;
  chunks.push_back({Fourcc("RDEF"), rdef});
  const uint32_t n = Load32(dxbc, 28);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t at = Load32(dxbc, 32 + 4 * i);
    const uint32_t cc = Load32(dxbc, at), size = Load32(dxbc, at + 4);
    if (cc == Fourcc("RDEF") || at + 8 + size > dxbc.size()) continue;
    chunks.push_back({cc, std::vector<uint8_t>(dxbc.begin() + at + 8, dxbc.begin() + at + 8 + size)});
  }
  std::vector<uint8_t> out(32 + 4 * chunks.size());
  std::memcpy(out.data(), dxbc.data(), 24);  // magic, checksum (not checked by HLSLcc), version
  const uint32_t count = static_cast<uint32_t>(chunks.size());
  std::memcpy(out.data() + 28, &count, 4);
  for (size_t i = 0; i < chunks.size(); ++i) {
    const uint32_t at = static_cast<uint32_t>(out.size()), size = static_cast<uint32_t>(chunks[i].second.size());
    std::memcpy(out.data() + 32 + 4 * i, &at, 4);
    out.resize(out.size() + 8);
    std::memcpy(out.data() + at, &chunks[i].first, 4);
    std::memcpy(out.data() + at + 4, &size, 4);
    out.insert(out.end(), chunks[i].second.begin(), chunks[i].second.end());
  }
  const uint32_t total = static_cast<uint32_t>(out.size());
  std::memcpy(out.data() + 24, &total, 4);
  return out;
}

uint32_t DefaultFlags() {
  return HLSLCC_FLAG_UNIFORM_BUFFER_OBJECT | HLSLCC_FLAG_GLOBAL_CONSTS_NEVER_IN_UBO | HLSLCC_FLAG_INOUT_APPEND_SEMANTIC_NAMES |
         HLSLCC_FLAG_DISABLE_GLOBALS_STRUCT | HLSLCC_FLAG_GLES31_IMAGE_QUALIFIERS | HLSLCC_FLAG_WRAP_UBO |
         HLSLCC_FLAG_TRANSLATE_MATRICES | HLSLCC_FLAG_FORCE_EXPLICIT_LOCATIONS;
}

namespace {

// Collects what HLSLcc reports for one stage.
class Collector final : public HLSLccReflection {
public:
  GlslProgram* out;
  GlslBuffer* current = nullptr;
  explicit Collector(GlslProgram* program) : out(program) {}

  void OnDiagnostics(const std::string& error, int line, bool isError) override {
    out->diagnostics += (isError ? "error: " : "warning: ") + error + " (line " + std::to_string(line) + ")\n";
  }
  void OnInputBinding(const std::string& name, int bindIndex) override {
    for (const auto& i : out->inputs) if (i.name == name) return;
    out->inputs.push_back({name, bindIndex});
  }
  bool OnConstantBuffer(const std::string& name, size_t bufferSize, size_t) override {
    current = nullptr;
    for (auto& b : out->buffers) if (b.name == name) current = &b;
    if (!current) {
      out->buffers.push_back({name, static_cast<int>(bufferSize), {}, -1});
      current = &out->buffers.back();
    }
    if (static_cast<int>(bufferSize) > current->size) current->size = static_cast<int>(bufferSize);
    return true;
  }
  bool OnConstant(const std::string& name, int bindIndex, SHADER_VARIABLE_TYPE cType, int rows, int cols,
                  bool isMatrix, int arraySize, bool) override {
    if (!current) return true;
    for (const auto& c : current->constants) if (c.name == name) return true;  // the other stage's
    current->constants.push_back({name, bindIndex, static_cast<int>(cType), rows, cols, isMatrix, arraySize});
    return true;
  }
  void OnConstantBufferBinding(const std::string& name, int bindIndex) override {
    for (auto& b : out->buffers) if (b.name == name) b.binding = bindIndex;
  }
  void OnTextureBinding(const std::string& name, int bindIndex, int samplerIndex, bool multisampled,
                        HLSLCC_TEX_DIMENSION dim, bool) override {
    for (const auto& t : out->textures) if (t.name == name) return;
    out->textures.push_back({name, bindIndex, samplerIndex, static_cast<int>(dim), multisampled});
  }
};

} // namespace

std::optional<GlslProgram> Translate(const std::vector<uint8_t>& vertexDxbc, const std::vector<uint8_t>& pixelDxbc,
                                     uint32_t flags, std::string* error) {
  GlslProgram program;
  Collector collector(&program);
  GLSLCrossDependencyData dependencies;
  HLSLccSamplerPrecisionInfo precisions;
  GlExtensions extensions {};
  GLSLShader vs, ps;
  if (!TranslateHLSLFromMem(reinterpret_cast<const char*>(vertexDxbc.data()), flags, LANG_ES_300, &extensions,
                            &dependencies, precisions, collector, &vs)) {
    if (error) *error = "HLSLcc failed on the vertex program: " + program.diagnostics;
    return std::nullopt;
  }
  if (!TranslateHLSLFromMem(reinterpret_cast<const char*>(pixelDxbc.data()), flags, LANG_ES_300, &extensions,
                            &dependencies, precisions, collector, &ps)) {
    if (error) *error = "HLSLcc failed on the pixel program: " + program.diagnostics;
    return std::nullopt;
  }
  program.source = "#ifdef VERTEX\n" + vs.sourceCode + "\n#endif\n#ifdef FRAGMENT\n" + ps.sourceCode + "\n#endif\n";
  return program;
}

} // namespace UnityBridge::ShaderXlat
