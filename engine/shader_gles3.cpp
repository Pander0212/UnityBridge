// SPDX-License-Identifier: GPL-3.0-or-later
// D3D11 -> GLES3x platform for Shader objects: see shader_gles3.h. Record layouts in docs/STATUS.md
// (M4a); the parameter record follows USCSandbox (GPL-3.0, Processor/ShaderParams.cs and friends).
#include "engine/shader_gles3.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "engine/shader_xlat.h"
#include "engine/unity_file.h"

namespace UnityBridge::ShaderGles3 {
namespace {

using TypeTree::Value;

constexpr uint32_t kD3D11 = 4, kGles3x = 9;
constexpr int kTypeGles3 = 4;  // kShaderGpuProgramGLES3
constexpr int32_t kRecordVersion = 202012090;
constexpr int kShaderClass = 48;
const char* const kStages[] = {"progVertex", "progFragment", "progGeometry", "progHull", "progDomain",
                               "progRayTracing"};

// The platform blob a subprogram's m_BlobIndex refers to, by its GPU program type
// (UnityPy's ShaderGpuProgramType): a pass's groups list every platform's subprograms in turn.
uint32_t PlatformOf(int64_t gpuType) {
  if (gpuType >= 2 && gpuType <= 4) return kGles3x;  // GLES31AEP, GLES31, GLES3
  if (gpuType >= 13 && gpuType <= 22) return kD3D11;  // DX10Level9 and DX11
  return 0;
}

// --- records ----------------------------------------------------------------------------------

class In {
public:
  In(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  bool ok = true;
  int32_t I32() {
    int32_t v = 0;
    if (pos_ + 4 > n_) { ok = false; return 0; }
    std::memcpy(&v, p_ + pos_, 4);
    pos_ += 4;
    return v;
  }
  std::string Bytes() {
    const int32_t n = I32();
    if (n < 0 || size_t(n) > n_ - pos_) { ok = false; return {}; }
    std::string s(reinterpret_cast<const char*>(p_ + pos_), size_t(n));
    pos_ = (pos_ + size_t(n) + 3) & ~size_t {3};
    return s;
  }
  int32_t Count() {
    const int32_t n = I32();
    if (n < 0 || size_t(n) > n_) ok = false;
    return ok ? n : 0;
  }

private:
  const uint8_t* p_;
  size_t n_, pos_ = 0;
};

class Out {
public:
  std::vector<uint8_t> b;
  void I32(int32_t v) {
    const size_t at = b.size();
    b.resize(at + 4);
    std::memcpy(b.data() + at, &v, 4);
  }
  void Bytes(const std::string& s) {
    I32(static_cast<int32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
    while (b.size() % 4) b.push_back(0);
  }
};

struct CodeRecord {
  std::vector<std::string> keywords;
  std::string code;
  int32_t sourceMap = 0;
};

std::optional<CodeRecord> ReadCode(const std::string& rec) {
  In r(reinterpret_cast<const uint8_t*>(rec.data()), rec.size());
  CodeRecord c;
  r.I32();  // version
  r.I32();  // GPU program type
  for (int i = 0; i < 4; ++i) r.I32();  // stats
  for (int32_t i = 0, n = r.Count(); i < n && r.ok; ++i) c.keywords.push_back(r.Bytes());
  c.code = r.Bytes();
  c.sourceMap = r.I32();
  if (!r.ok) return std::nullopt;
  return c;
}

struct Var {
  std::string name;
  int32_t type, rows, columns, matrix, arraySize, offset;
};
struct CBuffer {
  std::string name;
  int32_t size = 0;
  std::vector<Var> vars;
};
struct Binding {
  int32_t kind = 0;  // 0 texture, 1 cbuffer, 2 buffer, 3 UAV, 4 sampler
  std::string name;
  int32_t index = 0, sampler = 0;
  uint32_t extra = 0;  // texture: dim << 1 | multisampled
};
struct Params {
  std::vector<CBuffer> cbuffers;  // [0] is the base (unnamed) one
  std::vector<Binding> bindings;
};

std::optional<Params> ReadParams(const std::string& rec, std::string* error) {
  In r(reinterpret_cast<const uint8_t*>(rec.data()), rec.size());
  Params p;
  r.I32();  // version
  for (int32_t i = 0, n = r.Count(); i < n && r.ok; ++i) {
    CBuffer cb;
    cb.name = r.Bytes();
    cb.size = r.I32();
    for (int32_t k = 0, m = r.Count(); k < m && r.ok; ++k) {
      Var v;
      v.name = r.Bytes();
      v.type = r.I32();
      v.rows = r.I32();
      v.columns = r.I32();
      v.matrix = r.I32();
      v.arraySize = r.I32();
      v.offset = r.I32();
      cb.vars.push_back(v);
    }
    if (r.I32() != 0) {
      *error = "struct parameters in cbuffer " + cb.name;
      return std::nullopt;
    }
    p.cbuffers.push_back(cb);
  }
  for (int32_t i = 0, n = r.Count(); i < n && r.ok; ++i) {
    Binding b;
    b.name = r.Bytes();
    b.kind = r.I32();
    switch (b.kind) {
    case 0: b.index = r.I32(); b.sampler = r.I32(); b.extra = static_cast<uint32_t>(r.I32()); break;
    case 1: case 2: case 3: b.index = r.I32(); r.I32(); break;
    case 4: b.index = r.I32(); r.I32(); break;
    default: *error = "binding type " + std::to_string(b.kind); return std::nullopt;
    }
    p.bindings.push_back(b);
  }
  if (!r.ok) {
    *error = "parameter record ends early";
    return std::nullopt;
  }
  return p;
}

std::string CodeRecordBytes(const std::string& source, const CodeRecord& vs) {
  Out o;
  o.I32(kRecordVersion);
  o.I32(kTypeGles3);
  for (int i = 0; i < 4; ++i) o.I32(0);  // stats
  o.I32(static_cast<int32_t>(vs.keywords.size()));
  for (const auto& k : vs.keywords) o.Bytes(k);
  o.Bytes(source);
  o.I32(vs.sourceMap);
  o.I32(0);  // no bind channels: GLSL attributes go by name
  return std::string(o.b.begin(), o.b.end());
}

struct Ubo {
  std::string name;
  int32_t size;
  std::vector<Var> vars;
  int binding;
};

std::string ParamsRecordBytes(const std::vector<Var>& globals, int32_t globalsSize, const std::vector<Ubo>& ubos,
                              const std::vector<ShaderXlat::GlslTexture>& textures) {
  Out o;
  o.I32(kRecordVersion);
  o.I32(static_cast<int32_t>(2 + ubos.size()));
  const auto cbuffer = [&](const std::string& name, int32_t size, const std::vector<Var>& vars) {
    o.Bytes(name);
    o.I32(size);
    o.I32(static_cast<int32_t>(vars.size()));
    for (const auto& v : vars) {
      o.Bytes(v.name);
      for (const int32_t x : {v.type, v.rows, v.columns, v.matrix, v.arraySize, v.offset}) o.I32(x);
    }
    o.I32(0);  // no struct parameters
  };
  cbuffer("", 0, {});
  cbuffer("$Globals", globalsSize, globals);
  for (const auto& u : ubos) cbuffer(u.name, u.size, u.vars);
  o.I32(static_cast<int32_t>(textures.size() + ubos.size()));
  for (const auto& t : textures) {
    o.Bytes(t.name);
    o.I32(0);
    o.I32(t.unit);
    o.I32(t.sampler);
    const int dim = t.dim >= 0 && t.dim <= 6 ? t.dim : t.dim == 7 ? 6 : 2;  // HLSLCC_TEX_DIMENSION -> Unity's
    o.I32(dim << 1);
  }
  for (const auto& u : ubos) {
    o.Bytes(u.name);
    o.I32(1);
    o.I32(u.binding);
    o.I32(0);
  }
  return std::string(o.b.begin(), o.b.end());
}

std::vector<uint8_t> Blob(const std::vector<std::string>& records) {
  Out o;
  o.I32(static_cast<int32_t>(records.size()));
  size_t pos = 4 + 12 * records.size();
  for (const auto& r : records) {
    o.I32(static_cast<int32_t>(pos));
    o.I32(static_cast<int32_t>(r.size()));
    o.I32(0);
    pos += r.size();
  }
  for (const auto& r : records) o.b.insert(o.b.end(), r.begin(), r.end());
  return o.b;
}

// --- the shader's tree ------------------------------------------------------------------------

Value* Path(Value& v, std::initializer_list<const char*> names) {
  Value* at = &v;
  for (const char* n : names) {
    at = at ? at->Field(n) : nullptr;
  }
  return at;
}

int PlatformIndex(const Value& shader, uint32_t platform) {
  const Value* platforms = shader.Field("platforms");
  if (!platforms) return -1;
  for (size_t i = 0; i < platforms->items.size(); ++i)
    if (platforms->items[i].bits == platform) return static_cast<int>(i);
  return -1;
}

// The decompressed blob of `platform` (one segment: Unity 2021.3 players have one).
std::optional<std::vector<uint8_t>> PlatformBlob(Value& shader, uint32_t platform) {
  const int i = PlatformIndex(shader, platform);
  Value* offsets = shader.Field("offsets");
  Value* compressed = shader.Field("compressedLengths");
  Value* decompressed = shader.Field("decompressedLengths");
  Value* blob = shader.Field("compressedBlob");
  if (i < 0 || !offsets || !compressed || !decompressed || !blob) return std::nullopt;
  const auto first = [&](Value* v) -> std::optional<uint64_t> {
    if (size_t(i) >= v->items.size() || v->items[size_t(i)].items.empty()) return std::nullopt;
    return v->items[size_t(i)].items[0].bits;
  };
  const auto off = first(offsets), cl = first(compressed), dl = first(decompressed);
  if (!off || !cl || !dl || *off + *cl > blob->bytes.size()) return std::nullopt;
  return Lz4Decompress(reinterpret_cast<const uint8_t*>(blob->bytes.data()) + *off, size_t(*cl), size_t(*dl));
}

std::optional<std::string> Record(const std::vector<uint8_t>& blob, uint64_t index) {
  int32_t count = 0;
  if (blob.size() < 4) return std::nullopt;
  std::memcpy(&count, blob.data(), 4);
  if (count < 0 || index >= uint64_t(count) || 4 + 12 * (index + 1) > blob.size()) return std::nullopt;
  int32_t off = 0, len = 0;
  std::memcpy(&off, blob.data() + 4 + 12 * index, 4);
  std::memcpy(&len, blob.data() + 8 + 12 * index, 4);
  if (off < 0 || len < 0 || size_t(off) + size_t(len) > blob.size()) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(blob.data()) + off, size_t(len));
}

void DropPlatform(Value& shader, uint32_t platform) {
  const int i = PlatformIndex(shader, platform);
  if (i < 0) return;
  for (const char* key : {"platforms", "offsets", "compressedLengths", "decompressedLengths", "stageCounts"}) {
    Value* v = shader.Field(key);
    if (v && size_t(i) < v->items.size()) v->items.erase(v->items.begin() + i);
  }
  Value* subs = Path(shader, {"m_ParsedForm", "m_SubShaders"});
  if (!subs) return;
  for (auto& sub : subs->items) {
    Value* passes = sub.Field("m_Passes");
    if (!passes) continue;
    for (auto& pass : passes->items) {
      for (const char* stage : kStages) {
        Value* groups = Path(pass, {stage, "m_PlayerSubPrograms"});
        Value* params = Path(pass, {stage, "m_ParameterBlobIndices"});
        if (!groups) continue;
        for (size_t g = 0; g < groups->items.size(); ++g) {
          auto& group = groups->items[g].items;
          std::vector<Value>* indices = params && g < params->items.size() ? &params->items[g].items : nullptr;
          for (size_t k = group.size(); k-- > 0;) {
            const Value* type = group[k].Field("m_GpuProgramType");
            if (!type || PlatformOf(type->Int()) != platform) continue;
            group.erase(group.begin() + long(k));
            if (indices && k < indices->size()) indices->erase(indices->begin() + long(k));
          }
        }
      }
    }
  }
}

std::vector<uint64_t> Keywords(const Value& sp) {
  std::vector<uint64_t> k;
  if (const Value* kw = sp.Field("m_KeywordIndices"))
    for (const auto& i : kw->items) k.push_back(i.bits);
  std::sort(k.begin(), k.end());
  return k;
}

// Unity's tables for one D3D11 program, as the Reflection BuildRdef takes.
ShaderXlat::Reflection ReflectionOf(const Params& params, const Value& program,
                                    const std::map<int64_t, std::string>& names) {
  ShaderXlat::Reflection r;
  std::map<std::string, int> registers;
  for (const auto& b : params.bindings)
    if (b.kind == 1) registers[b.name] = b.index;
  for (const auto& cb : params.cbuffers) {
    if (cb.name.empty()) continue;  // the base buffer: empty for D3D11
    ShaderXlat::ConstantBuffer c;
    c.name = cb.name;
    c.bindPoint = registers.count(cb.name) ? registers[cb.name] : 0;
    c.size = cb.size;
    for (const auto& v : cb.vars) {
      ShaderXlat::Variable x;
      x.name = v.name;
      x.type = static_cast<ShaderXlat::ParamType>(v.type);
      x.rows = v.rows;
      x.columns = v.columns;
      x.isMatrix = v.matrix > 0;
      x.arraySize = v.arraySize;
      x.offset = v.offset;
      c.variables.push_back(x);
    }
    r.constantBuffers.push_back(c);
  }
  const auto texture = [&](std::string name, int64_t reg, int64_t sampler, int64_t dim, bool ms) {
    ShaderXlat::Texture t;
    t.name = std::move(name);
    t.bindPoint = int(reg);
    t.samplerBindPoint = sampler >= 0 && sampler < 0x7fffffff ? int(sampler) : -1;
    t.dim = static_cast<ShaderXlat::TextureDim>(dim);
    t.multisampled = ms;
    r.textures.push_back(t);
  };
  for (const auto& b : params.bindings) {
    if (b.kind == 0) texture(b.name, b.index, b.sampler, b.extra >> 1, (b.extra & 1) != 0);
    if (b.kind == 4) r.samplers.push_back({b.name, b.index});
  }
  const Value* common = program.Field("m_CommonParameters");
  if (!common) return r;
  const auto nameOf = [&](const Value& v, const char* prefix) {
    const int64_t index = v.Field("m_NameIndex")->Int();
    const auto it = names.find(index);
    return it != names.end() ? it->second : prefix + std::to_string(index);
  };
  if (const Value* textures = common->Field("m_TextureParams"))
    for (const auto& t : textures->items)
      texture(nameOf(t, "tex"), t.Field("m_Index")->Int(), t.Field("m_SamplerIndex")->Int(), t.Field("m_Dim")->Int(),
              t.Field("m_MultiSampled")->bits != 0);
  if (const Value* samplers = common->Field("m_Samplers"))
    for (const auto& s : samplers->items) {
      const int64_t bind = s.Field("bindPoint")->Int();
      r.samplers.push_back({"UnityBridge_s" + std::to_string(bind), int(bind)});
    }
  // Constant buffers shared by all variants of the program, bound by name.
  const Value* buffers = common->Field("m_ConstantBuffers");
  const Value* bindings = common->Field("m_ConstantBufferBindings");
  if (buffers && bindings)
    for (const auto& b : buffers->items) {
      ShaderXlat::ConstantBuffer c;
      c.name = nameOf(b, "cb");
      bool same = false;
      for (const auto& x : r.constantBuffers) same = same || x.name == c.name;
      if (same) continue;
      bool bound = false;
      for (const auto& x : bindings->items)
        if (x.Field("m_NameIndex")->Int() == b.Field("m_NameIndex")->Int()) {
          c.bindPoint = int(x.Field("m_Index")->Int());
          bound = true;
        }
      if (!bound) continue;
      c.size = int(b.Field("m_Size")->Int());
      if (const Value* vectors = b.Field("m_VectorParams"))
        for (const auto& v : vectors->items) {
          ShaderXlat::Variable x;
          x.name = nameOf(v, "v");
          x.type = static_cast<ShaderXlat::ParamType>(v.Field("m_Type")->Int());
          x.columns = int(v.Field("m_Dim")->Int());
          x.arraySize = int(v.Field("m_ArraySize")->Int());
          x.offset = int(v.Field("m_Index")->Int());
          c.variables.push_back(x);
        }
      if (const Value* matrices = b.Field("m_MatrixParams"))
        for (const auto& v : matrices->items) {
          ShaderXlat::Variable x;
          x.name = nameOf(v, "m");
          x.type = static_cast<ShaderXlat::ParamType>(v.Field("m_Type")->Int());
          x.rows = int(v.Field("m_RowCount")->Int());
          x.columns = 4;
          x.isMatrix = true;
          x.arraySize = int(v.Field("m_ArraySize")->Int());
          x.offset = int(v.Field("m_Index")->Int());
          c.variables.push_back(x);
        }
      r.constantBuffers.push_back(c);
    }
  return r;
}

// D3D11 code minus Unity's header (6 bytes, and 32 more from header version 2 on).
std::vector<uint8_t> StripUnityHeader(const std::string& code) {
  const size_t skip = 6 + (!code.empty() && uint8_t(code[0]) >= 2 ? 0x20 : 0);
  if (code.size() <= skip) return {};
  return std::vector<uint8_t>(code.begin() + long(skip), code.end());
}

struct Pair {
  std::string code, params;  // the GLES3 records
};

std::optional<Pair> TranslatePair(const std::vector<uint8_t>& dx, uint64_t vsCode, uint64_t vsParams,
                                  uint64_t psCode, uint64_t psParams, const Value& vsProg, const Value& psProg,
                                  const std::map<int64_t, std::string>& names, std::string* error,
                                  std::string* notes) {
  std::vector<uint8_t> dxbc[2];
  ShaderXlat::Reflection tables[2];  // Unity's, before CompleteReflection's stand-ins
  std::optional<CodeRecord> vsRecord;
  const uint64_t codes[2] = {vsCode, psCode}, paramIdx[2] = {vsParams, psParams};
  const Value* progs[2] = {&vsProg, &psProg};
  for (int s = 0; s < 2; ++s) {
    const auto codeRec = Record(dx, codes[s]);
    const auto paramRec = Record(dx, paramIdx[s]);
    if (!codeRec || !paramRec) { *error = "blob index out of range"; return std::nullopt; }
    const auto code = ReadCode(*codeRec);
    if (!code) { *error = "malformed code record"; return std::nullopt; }
    if (s == 0) vsRecord = code;
    const auto params = ReadParams(*paramRec, error);
    if (!params) return std::nullopt;
    tables[s] = ReflectionOf(*params, *progs[s], names);
    const auto raw = StripUnityHeader(code->code);
    const auto model = ShaderXlat::ShaderModel(raw);
    if (!model) { *error = "no DXBC shader model"; return std::nullopt; }
    ShaderXlat::Reflection complete = tables[s];
    std::string why;
    if (!ShaderXlat::CompleteReflection(complete, raw, &why, notes)) {
      *error = std::string(s ? "pixel" : "vertex") + " program: " + why;
      return std::nullopt;
    }
    dxbc[s] = ShaderXlat::WithRdef(raw, ShaderXlat::BuildRdef(complete, model->first, model->second));
    if (dxbc[s].empty()) { *error = "not a DXBC container"; return std::nullopt; }
  }
  const auto program = ShaderXlat::Translate(dxbc[0], dxbc[1], ShaderXlat::DefaultFlags(), error);
  if (!program) return std::nullopt;

  // The GLSL side's table: members and sizes from the D3D11 tables (HLSLcc reports no offsets).
  struct Merged {
    int32_t size = 0;
    std::vector<Var> vars;
  };
  std::map<std::string, Merged> dxBuffers;
  for (const auto& t : tables)
    for (const auto& cb : t.constantBuffers) {
      auto& m = dxBuffers[cb.name];
      m.size = std::max(m.size, cb.size);
      for (const auto& v : cb.variables) {
        const Var x {v.name, static_cast<int32_t>(v.type), v.rows, v.columns, v.isMatrix ? 1 : 0, v.arraySize, v.offset};
        auto it = std::find_if(m.vars.begin(), m.vars.end(), [&](const Var& y) { return y.name == v.name; });
        if (it == m.vars.end()) m.vars.push_back(x);
        else *it = x;
      }
    }
  std::vector<Var> globals;
  int32_t globalsSize = 0;
  std::vector<Ubo> ubos;
  for (const auto& b : program->buffers) {
    const auto it = dxBuffers.find(b.name);
    const Merged src = it != dxBuffers.end() ? it->second : Merged {};
    if (b.binding < 0) {
      globals = src.vars;
      globalsSize = src.size;
    } else {
      ubos.push_back({b.name, src.size, src.vars, b.binding});
    }
  }
  return Pair {CodeRecordBytes(program->source, *vsRecord), ParamsRecordBytes(globals, globalsSize, ubos,
                                                                               program->textures)};
}

} // namespace

std::optional<std::vector<uint8_t>> Lz4Decompress(const uint8_t* src, size_t n, size_t size) {
  std::vector<uint8_t> out;
  out.reserve(size);
  size_t i = 0;
  const auto length = [&](size_t base) -> std::optional<size_t> {
    size_t len = base;
    if (base != 15) return len;
    while (true) {
      if (i >= n) return std::nullopt;
      const uint8_t b = src[i++];
      len += b;
      if (b != 255) return len;
    }
  };
  while (i < n) {
    const uint8_t token = src[i++];
    const auto lit = length(token >> 4);
    if (!lit || *lit > n - i || out.size() + *lit > size) return std::nullopt;
    out.insert(out.end(), src + i, src + i + *lit);
    i += *lit;
    if (i == n) break;  // the last sequence has literals only
    if (i + 2 > n) return std::nullopt;
    const size_t offset = size_t(src[i]) | size_t(src[i + 1]) << 8;
    i += 2;
    const auto match = length(token & 15);
    if (!match || offset == 0 || offset > out.size() || out.size() + *match + 4 > size) return std::nullopt;
    const size_t from = out.size() - offset;
    for (size_t k = 0; k < *match + 4; ++k) out.push_back(out[from + k]);  // may overlap itself
  }
  if (out.size() != size) return std::nullopt;
  return out;
}

std::vector<uint8_t> Lz4Literals(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out;
  const size_t n = data.size();
  out.push_back(static_cast<uint8_t>(std::min<size_t>(n, 15) << 4));
  if (n >= 15) {
    size_t rest = n - 15;
    for (; rest >= 255; rest -= 255) out.push_back(255);
    out.push_back(static_cast<uint8_t>(rest));
  }
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

bool AddGles3(Value& shader, bool replace, Stats* stats) {
  if (PlatformIndex(shader, kD3D11) < 0) return false;
  if (PlatformIndex(shader, kGles3x) >= 0) {
    if (!replace) return false;
    DropPlatform(shader, kGles3x);
  }
  const Value* nameValue = Path(shader, {"m_ParsedForm", "m_Name"});
  const std::string name = nameValue ? nameValue->bytes : "?";
  const auto dx = PlatformBlob(shader, kD3D11);
  if (!dx) {
    stats->log += name + ": D3D11 blob unreadable\n";
    return false;
  }
  Value* subs = Path(shader, {"m_ParsedForm", "m_SubShaders"});
  if (!subs) return false;

  if (stats->progress) stats->progress(name + ": translating");
  std::vector<std::string> records;
  int done = 0, failed = 0;
  for (auto& sub : subs->items) {
    Value* passes = sub.Field("m_Passes");
    if (!passes) continue;
    for (auto& pass : passes->items) {
      std::map<int64_t, std::string> names;
      if (const Value* map = pass.Field("m_NameIndices"))
        for (const auto& pair : map->items) names[pair.items[1].Int()] = pair.items[0].bytes;
      Value* vsp = pass.Field("progVertex");
      Value* psp = pass.Field("progFragment");
      Value* vsGroups = vsp ? vsp->Field("m_PlayerSubPrograms") : nullptr;
      Value* vsParams = vsp ? vsp->Field("m_ParameterBlobIndices") : nullptr;
      Value* psGroups = psp ? psp->Field("m_PlayerSubPrograms") : nullptr;
      Value* psParams = psp ? psp->Field("m_ParameterBlobIndices") : nullptr;
      if (!vsGroups || !vsParams || !psGroups || !psParams) continue;
      for (size_t g = 0; g < vsGroups->items.size(); ++g) {
        if (g >= psGroups->items.size() || g >= vsParams->items.size() || g >= psParams->items.size()) break;
        std::map<std::vector<uint64_t>, std::pair<uint64_t, uint64_t>> psByKeywords;  // code, params
        const auto& psGroup = psGroups->items[g].items;
        for (size_t k = 0; k < psGroup.size() && k < psParams->items[g].items.size(); ++k)
          if (PlatformOf(psGroup[k].Field("m_GpuProgramType")->Int()) == kD3D11)
            psByKeywords[Keywords(psGroup[k])] = {psGroup[k].Field("m_BlobIndex")->bits,
                                                  psParams->items[g].items[k].bits};
        auto& group = vsGroups->items[g].items;
        auto& groupParams = vsParams->items[g].items;
        std::vector<Value> added, addedParams;
        for (size_t k = 0; k < group.size() && k < groupParams.size(); ++k) {
          if (PlatformOf(group[k].Field("m_GpuProgramType")->Int()) != kD3D11) continue;
          const auto ps = psByKeywords.find(Keywords(group[k]));
          if (ps == psByKeywords.end()) continue;
          std::string error, notes;
          const auto pair = TranslatePair(*dx, group[k].Field("m_BlobIndex")->bits, groupParams[k].bits,
                                          ps->second.first, ps->second.second, *vsp, *psp, names, &error, &notes);
          for (auto& c : notes) if (c == '\n') c = ' ';
          if (!notes.empty()) {
            ++stats->standIns;
            if (stats->progress && stats->standIns <= 5) stats->progress(name + ": stand-ins: " + notes);
          }
          if (!pair) {
            ++failed;
            stats->log += name + ": not translated: " + error + "\n";
            if (stats->progress && failed <= 3) stats->progress(name + ": not translated: " + error);
            continue;
          }
          records.push_back(pair->params);
          Value param = groupParams[k];
          param.bits = records.size() - 1;
          records.push_back(pair->code);
          Value entry = group[k];  // same keywords and requirements
          entry.Field("m_BlobIndex")->bits = records.size() - 1;
          entry.Field("m_GpuProgramType")->SetInt(kTypeGles3);
          added.push_back(std::move(entry));
          addedParams.push_back(std::move(param));
          ++done;
        }
        group.insert(group.end(), added.begin(), added.end());
        groupParams.insert(groupParams.end(), addedParams.begin(), addedParams.end());
      }
    }
  }

  const auto data = Blob(records);
  const auto packed = Lz4Literals(data);
  Value* blob = shader.Field("compressedBlob");
  const int d3d = PlatformIndex(shader, kD3D11);
  const auto append = [&](const char* key, uint64_t v) {  // a copy of D3D11's entry with `v`
    Value* list = shader.Field(key);
    Value item = list->items[size_t(d3d)];
    if (item.kind == Value::Kind::Vector) item.items.resize(1), item.items[0].bits = v;
    else item.bits = v;
    list->items.push_back(std::move(item));
  };
  append("offsets", blob->bytes.size());
  append("compressedLengths", packed.size());
  append("decompressedLengths", data.size());
  append("stageCounts", 1);
  append("platforms", kGles3x);  // last: `d3d` indexes the others
  blob->bytes.append(packed.begin(), packed.end());
  stats->log += name + ": " + std::to_string(done) + " GLES3 programs added, " + std::to_string(failed) +
                " not translated\n";
  stats->programs += done;
  stats->failed += failed;
  ++stats->shaders;
  return true;
}

std::optional<std::vector<uint8_t>> ConvertFile(const uint8_t* data, size_t size, bool replace, Stats* stats,
                                                std::string* error) {
  const auto file = UnityFile::Parse(data, size, error);
  if (!file) return std::nullopt;
  if (file->unityVersion.rfind("2021.3.", 0) != 0) {
    *error = "Unity " + file->unityVersion + ": only 2021.3 shaders are known";
    return std::nullopt;
  }
  std::vector<UnityFile::Replacement> replacements;
  for (size_t i = 0; i < file->objects.size(); ++i) {
    const auto& o = file->objects[i];
    if (o.classId != kShaderClass) continue;
    std::string why;
    if (stats->progress)
      stats->progress("object " + std::to_string(o.pathId) + ": reading " + std::to_string(o.byteSize) + " bytes");
    auto shader = TypeTree::Read(TypeTree::Shader2021_3(), data + file->dataOffset + o.byteStart, o.byteSize, &why);
    // Only a shader that writes back to its own bytes is one the built-in type tree describes.
    const uint8_t* original = data + file->dataOffset + o.byteStart;
    if (shader && TypeTree::Write(*shader) != std::vector<uint8_t>(original, original + o.byteSize))
      shader.reset(), why = "does not match the 2021.3 type tree";
    if (!shader) {
      stats->log += "shader object " + std::to_string(o.pathId) + ": " + why + "\n";
      continue;
    }
    if (AddGles3(*shader, replace, stats)) replacements.push_back({i, TypeTree::Write(*shader)});
  }
  if (replacements.empty()) {
    *error = "nothing to convert";
    return std::nullopt;
  }
  return UnityFile::Rebuild(*file, data, size, replacements);
}

} // namespace UnityBridge::ShaderGles3
