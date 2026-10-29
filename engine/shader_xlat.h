// SPDX-License-Identifier: GPL-3.0-or-later
// D3D11 shader programs -> GLSL ES 3.0, the way Unity's own GLES3 programs are made (M4).
//
// A game built for Windows carries its shaders as DXBC only, and Unity's player builds strip the
// reflection chunk (RDEF) from it: what HLSLcc, the translator Unity uses for GLES3, reads the names
// and layouts of constant buffers and textures from. Unity keeps that information in its own
// parameter tables instead. So: BuildRdef writes an RDEF from a Reflection (filled from those
// tables), WithRdef puts it into the DXBC container, and Translate runs HLSLcc on a vertex and a
// pixel program together and joins them into Unity's one-source layout (#ifdef VERTEX / #ifdef
// FRAGMENT), with the reflection HLSLcc reports for the GLSL side (what Unity's GLES3 parameter
// table describes).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace UnityBridge::ShaderXlat {

// Unity's ShaderParamType.
enum class ParamType : int { Float = 0, Int = 1, Bool = 2, Half = 3, Short = 4, UInt = 5 };

struct Variable {  // a member of a constant buffer, as Unity's parameter table describes it
  std::string name;
  ParamType type = ParamType::Float;
  int rows = 1, columns = 4;
  bool isMatrix = false;
  int arraySize = 0;  // 0: not an array
  int offset = 0;     // bytes from the start of the buffer
};

struct ConstantBuffer {
  std::string name;
  int bindPoint = 0;  // register b<n>
  int size = 0;       // bytes
  std::vector<Variable> variables;
};

// Unity's TextureDimension.
enum class TextureDim : int { Tex2D = 2, Tex3D = 3, Cube = 4, Tex2DArray = 5, CubeArray = 6 };

struct Texture {
  std::string name;
  int bindPoint = 0;          // register t<n>
  int samplerBindPoint = -1;  // register s<n> of its sampler, -1 if none
  TextureDim dim = TextureDim::Tex2D;
  bool multisampled = false;
};

struct Reflection {
  std::vector<ConstantBuffer> constantBuffers;
  std::vector<Texture> textures;
};

// The RDEF chunk body for `reflection`, for shader model `major`.`minor` (from the program's SHDR/SHEX).
std::vector<uint8_t> BuildRdef(const Reflection& reflection, int major, int minor);

// The shader model of a DXBC container's SHDR/SHEX chunk, or nullopt if it has none.
std::optional<std::pair<int, int>> ShaderModel(const std::vector<uint8_t>& dxbc);

// `dxbc` with `rdef` as its RDEF chunk (an existing one is replaced). Empty if `dxbc` is not a
// DXBC container.
std::vector<uint8_t> WithRdef(const std::vector<uint8_t>& dxbc, const std::vector<uint8_t>& rdef);

// What HLSLcc reports about the GLSL program, in the form Unity's GLES3 parameter table takes.
struct GlslConstant {
  std::string name;
  int offset = 0;  // HLSLcc's bind index: the byte offset within its buffer
  int type = 0;    // HLSLcc's SHADER_VARIABLE_TYPE
  int rows = 0, columns = 0;
  bool isMatrix = false;
  int arraySize = 0;
};
struct GlslBuffer {
  std::string name;  // "$Globals" for the plain uniforms
  int size = 0;
  std::vector<GlslConstant> constants;
  int binding = -1;  // the uniform block binding, -1 for $Globals
};
struct GlslTexture {
  std::string name;
  int unit = 0, sampler = 0;
  int dim = 0;  // HLSLcc's HLSLCC_TEX_DIMENSION
  bool multisampled = false;
};
struct GlslInput {  // a vertex attribute
  std::string name;
  int location = 0;
};
struct GlslProgram {
  std::string source;  // Unity's layout: #ifdef VERTEX ... #endif #ifdef FRAGMENT ... #endif
  std::vector<GlslBuffer> buffers;
  std::vector<GlslTexture> textures;
  std::vector<GlslInput> inputs;
  std::string diagnostics;  // HLSLcc's messages, if any
};

// HLSLcc's flags for Unity-style GLES3 output.
uint32_t DefaultFlags();

// Translates a vertex and a pixel program (DXBC with an RDEF each) into one GLSL ES 3.0 program.
std::optional<GlslProgram> Translate(const std::vector<uint8_t>& vertexDxbc, const std::vector<uint8_t>& pixelDxbc,
                                     uint32_t flags, std::string* error);

} // namespace UnityBridge::ShaderXlat
