// SPDX-License-Identifier: GPL-3.0-or-later
// A type-tree driven reader and writer for objects in Unity serialized files (M4c). Player builds strip
// the type trees, so the trees are built in (engine/typetree_*.inc, generated from UnityPy's type
// database). Read turns an object's bytes into a tree of Values; Write turns it back into bytes, the
// same ones when nothing was changed. The rules are Unity's (as UnityPy's TypeTreeHelper reads them):
// little-endian primitives, strings as length + bytes aligned to 4, vectors as count + elements, a
// node with the align flag (0x4000) followed by alignment to 4; an array whose element type has the
// flag aligns once after the last element.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace UnityBridge::TypeTree {

struct Node {
  std::string type, name;
  int byteSize = -1;
  uint32_t flags = 0;
  std::vector<std::unique_ptr<Node>> children;
  bool Aligned() const { return (flags & 0x4000) != 0; }
};

struct Row {  // one line of a generated table
  int depth;
  const char* type;
  const char* name;
  int byteSize;
  uint32_t flags;
};

// The tree of a generated table (rows in depth-first order).
std::unique_ptr<Node> Build(const Row* rows, size_t count);
// Class 48 (Shader), Unity 2021.3.
const Node& Shader2021_3();
// Class 141 (BuildSettings, in globalgamemanagers), Unity 2021.3.
const Node& BuildSettings2021_3();

struct Value {
  enum class Kind { Primitive, Bytes, String, Vector, Object, Pair };
  const Node* node = nullptr;
  Kind kind = Kind::Object;
  uint64_t bits = 0;      // Primitive: the raw little-endian value, zero-extended
  std::string bytes;      // String, Bytes (TypelessData, or a vector of a 1-byte primitive type)
  std::vector<Value> items;  // Vector elements, Object fields (in node order), Pair (two)

  Value* Field(const std::string& name);
  const Value* Field(const std::string& name) const;
  int64_t Int() const;  // a Primitive as a signed integer of its size
  void SetInt(int64_t v);
};

// Reads an object of type `root` from `data`. Nullopt (and `error`) if the data is short or malformed.
std::optional<Value> Read(const Node& root, const uint8_t* data, size_t size, std::string* error);
// The bytes of `value`.
std::vector<uint8_t> Write(const Value& value);

// A new element for a vector of `vector` (a Value of Kind::Vector), zeroed, with its fields.
Value NewElement(const Value& vector);
// A zeroed value of type `node` (empty strings and vectors).
Value Zero(const Node& node);

} // namespace UnityBridge::TypeTree
