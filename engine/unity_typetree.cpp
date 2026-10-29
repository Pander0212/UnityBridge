// SPDX-License-Identifier: GPL-3.0-or-later
// Type-tree driven object reader and writer: see unity_typetree.h.
#include "engine/unity_typetree.h"

#include <cstring>
#include <map>

namespace UnityBridge::TypeTree {
namespace {

// Byte size of a primitive type, 0 if `type` is not one.
int PrimitiveSize(const std::string& type) {
  static const std::map<std::string, int> sizes = {
      {"SInt8", 1}, {"UInt8", 1}, {"char", 1}, {"bool", 1},
      {"short", 2}, {"SInt16", 2}, {"unsigned short", 2}, {"UInt16", 2},
      {"int", 4}, {"SInt32", 4}, {"unsigned int", 4}, {"UInt32", 4}, {"Type*", 4}, {"float", 4},
      {"long long", 8}, {"SInt64", 8}, {"unsigned long long", 8}, {"UInt64", 8}, {"FileSize", 8}, {"double", 8},
  };
  const auto it = sizes.find(type);
  return it == sizes.end() ? 0 : it->second;
}

bool IsVector(const Node& n) { return !n.children.empty() && n.children[0]->type == "Array"; }

bool IsSigned(const std::string& type) {
  return type == "SInt8" || type == "short" || type == "SInt16" || type == "int" || type == "SInt32" ||
         type == "long long" || type == "SInt64";
}

class Reader {
public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  bool ok = true;
  size_t pos = 0;

  void Align() { pos = (pos + 3) & ~size_t {3}; }
  uint64_t Bits(int n) {
    if (pos + size_t(n) > size_) { ok = false; return 0; }
    uint64_t v = 0;
    std::memcpy(&v, data_ + pos, size_t(n));
    pos += size_t(n);
    return v;
  }
  int32_t Count() {
    const auto v = static_cast<int32_t>(Bits(4));
    if (v < 0 || size_t(v) > size_ - pos) ok = false;  // more elements than bytes left: corrupt
    return ok ? v : 0;
  }
  std::string Raw(size_t n) {
    if (pos + n > size_) { ok = false; return {}; }
    std::string s(reinterpret_cast<const char*>(data_ + pos), n);
    pos += n;
    return s;
  }

  Value Read(const Node& n, bool ownAlign) {
    Value v;
    v.node = &n;
    bool align = ownAlign && n.Aligned();
    if (const int size = PrimitiveSize(n.type)) {
      v.kind = Value::Kind::Primitive;
      v.bits = Bits(size);
    } else if (n.type == "string") {
      v.kind = Value::Kind::String;
      v.bytes = Raw(size_t(Count()));
      Align();  // strings are always aligned
    } else if (n.type == "TypelessData") {
      v.kind = Value::Kind::Bytes;
      v.bytes = Raw(size_t(Count()));
    } else if (n.type == "pair" && n.children.size() == 2) {
      v.kind = Value::Kind::Pair;
      v.items.push_back(Read(*n.children[0], true));
      v.items.push_back(Read(*n.children[1], true));
    } else if (IsVector(n) && n.children[0]->children.size() == 2) {
      const Node& array = *n.children[0];
      const Node& element = *array.children[1];
      if (array.Aligned()) align = true;
      const int32_t count = Count();
      if (PrimitiveSize(element.type) == 1) {
        v.kind = Value::Kind::Bytes;  // compressedBlob and friends: one block
        v.bytes = Raw(size_t(count));
      } else {
        v.kind = Value::Kind::Vector;
        v.items.reserve(size_t(count));
        for (int32_t i = 0; i < count && ok; ++i) v.items.push_back(Read(element, !element.Aligned()));
      }
      if (element.Aligned()) Align();
    } else {
      v.kind = Value::Kind::Object;
      for (const auto& c : n.children) {
        if (!ok) break;
        v.items.push_back(Read(*c, true));
      }
    }
    if (align) Align();
    return v;
  }

private:
  const uint8_t* data_;
  size_t size_;
};

class Writer {
public:
  std::vector<uint8_t> out;

  void Align() { while (out.size() % 4) out.push_back(0); }
  void Bits(uint64_t v, int n) {
    const size_t at = out.size();
    out.resize(at + size_t(n));
    std::memcpy(out.data() + at, &v, size_t(n));
  }
  void Raw(const std::string& s) {
    Bits(static_cast<uint32_t>(s.size()), 4);
    out.insert(out.end(), s.begin(), s.end());
  }

  void Write(const Value& v, bool ownAlign) {
    const Node& n = *v.node;
    bool align = ownAlign && n.Aligned();
    if (const int size = PrimitiveSize(n.type)) {
      Bits(v.bits, size);
    } else if (n.type == "string") {
      Raw(v.bytes);
      Align();
    } else if (n.type == "TypelessData") {
      Raw(v.bytes);
    } else if (n.type == "pair" && v.items.size() == 2) {
      Write(v.items[0], true);
      Write(v.items[1], true);
    } else if (IsVector(n) && n.children[0]->children.size() == 2) {
      const Node& array = *n.children[0];
      const Node& element = *array.children[1];
      if (array.Aligned()) align = true;
      if (v.kind == Value::Kind::Bytes) {
        Raw(v.bytes);
      } else {
        Bits(static_cast<uint32_t>(v.items.size()), 4);
        for (const auto& item : v.items) Write(item, !element.Aligned());
      }
      if (element.Aligned()) Align();
    } else {
      for (const auto& item : v.items) Write(item, true);
    }
    if (align) Align();
  }
};

Value Zeroed(const Node& n) {
  Value v;
  v.node = &n;
  if (PrimitiveSize(n.type)) v.kind = Value::Kind::Primitive;
  else if (n.type == "string") v.kind = Value::Kind::String;
  else if (n.type == "TypelessData") v.kind = Value::Kind::Bytes;
  else if (n.type == "pair" && n.children.size() == 2) {
    v.kind = Value::Kind::Pair;
    v.items = {Zeroed(*n.children[0]), Zeroed(*n.children[1])};
  } else if (IsVector(n) && n.children[0]->children.size() == 2) {
    v.kind = PrimitiveSize(n.children[0]->children[1]->type) == 1 ? Value::Kind::Bytes : Value::Kind::Vector;
  } else {
    v.kind = Value::Kind::Object;
    for (const auto& c : n.children) v.items.push_back(Zeroed(*c));
  }
  return v;
}

} // namespace

std::unique_ptr<Node> Build(const Row* rows, size_t count) {
  std::vector<Node*> stack;
  std::unique_ptr<Node> root;
  for (size_t i = 0; i < count; ++i) {
    auto n = std::make_unique<Node>();
    n->type = rows[i].type;
    n->name = rows[i].name;
    n->byteSize = rows[i].byteSize;
    n->flags = rows[i].flags;
    Node* raw = n.get();
    const auto depth = static_cast<size_t>(rows[i].depth);
    if (depth == 0) {
      root = std::move(n);
      stack = {raw};
      continue;
    }
    stack.resize(depth);
    stack.back()->children.push_back(std::move(n));
    stack.push_back(raw);
  }
  return root;
}

const Node& Shader2021_3() {
  static const Row rows[] = {
#include "engine/typetree_shader_2021_3.inc"
  };
  static const auto tree = Build(rows, sizeof(rows) / sizeof(rows[0]));
  return *tree;
}

const Node& BuildSettings2021_3() {
  static const Row rows[] = {
#include "engine/typetree_buildsettings_2021_3.inc"
  };
  static const auto tree = Build(rows, sizeof(rows) / sizeof(rows[0]));
  return *tree;
}

Value* Value::Field(const std::string& name) {
  if (kind != Kind::Object) return nullptr;
  for (auto& item : items) if (item.node && item.node->name == name) return &item;
  return nullptr;
}

const Value* Value::Field(const std::string& name) const { return const_cast<Value*>(this)->Field(name); }

int64_t Value::Int() const {
  const int size = node ? PrimitiveSize(node->type) : 0;
  if (size == 0 || size == 8 || !IsSigned(node->type)) return static_cast<int64_t>(bits);
  const int shift = 64 - 8 * size;
  return static_cast<int64_t>(bits << shift) >> shift;
}

void Value::SetInt(int64_t v) {
  const int size = node ? PrimitiveSize(node->type) : 8;
  bits = size >= 8 ? static_cast<uint64_t>(v) : static_cast<uint64_t>(v) & ((uint64_t {1} << (8 * size)) - 1);
}

std::optional<Value> Read(const Node& root, const uint8_t* data, size_t size, std::string* error) {
  Reader r(data, size);
  Value v = r.Read(root, true);
  if (!r.ok) {
    if (error) *error = "object data ends early or is malformed (at byte " + std::to_string(r.pos) + ")";
    return std::nullopt;
  }
  return v;
}

std::vector<uint8_t> Write(const Value& value) {
  Writer w;
  w.Write(value, true);
  return std::move(w.out);
}

Value Zero(const Node& node) { return Zeroed(node); }

Value NewElement(const Value& vector) {
  const Node& n = *vector.node;
  return Zeroed(*n.children[0]->children[1]);
}

} // namespace UnityBridge::TypeTree
