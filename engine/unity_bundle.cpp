// SPDX-License-Identifier: GPL-3.0-or-later
// UnityFS bundles retargeted to Android: see unity_bundle.h.
//
// Layout (big-endian): "UnityFS\0", u32 format, the player version strings ("5.x.x", "6000.3.15f1"),
// u64 file size, u32 compressed and uncompressed size of the blocks info, u32 flags (bits 0-5 the
// blocks info's compression, 0x40 info and directory together, 0x80 info at the end of the file,
// 0x200 padding before the first block). From format 7 the header is padded to 16 bytes. The blocks
// info: a 16-byte hash, the blocks (u32 uncompressed size, u32 compressed size, u16 flags with the
// compression in bits 0-5) and the directory (i64 offset, i64 size, u32 flags, the path) into the
// concatenated uncompressed blocks.
#include "engine/unity_bundle.h"

#include <cstring>

#include "engine/shader_gles3.h"  // Lz4Decompress

namespace UnityBridge::UnityBundle {
namespace {

constexpr uint32_t kCompressionMask = 0x3f, kInfoAtEnd = 0x80, kPadBeforeBlocks = 0x200;
constexpr uint32_t kAndroid = 13;

struct Reader {
  const uint8_t* p;
  size_t n, at = 0;
  bool ok = true;
  bool Need(size_t k) { if (!ok || at + k > n) ok = false; return ok; }
  uint64_t Be(int bytes) {
    if (!Need(size_t(bytes))) return 0;
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = v << 8 | p[at++];
    return v;
  }
  std::string Str() {
    const void* end = ok && at < n ? std::memchr(p + at, 0, n - at) : nullptr;
    if (!end) { ok = false; return {}; }
    std::string s(reinterpret_cast<const char*>(p + at), static_cast<const uint8_t*>(end) - (p + at));
    at += s.size() + 1;
    return s;
  }
  void Align(size_t a) { at = (at + a - 1) / a * a; if (at > n) ok = false; }
};

void PutBe(std::vector<uint8_t>& out, uint64_t v, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) out.push_back(uint8_t(v >> (8 * i)));
}

std::optional<std::vector<uint8_t>> Decompress(const uint8_t* src, size_t n, size_t size, uint32_t kind,
                                               std::string* error) {
  if (kind == 0) {
    if (n != size) { *error = "uncompressed block of the wrong size"; return std::nullopt; }
    return std::vector<uint8_t>(src, src + n);
  }
  if (kind == 2 || kind == 3) {
    auto out = ShaderGles3::Lz4Decompress(src, n, size);
    if (!out) *error = "malformed LZ4 block";
    return out;
  }
  *error = kind == 1 ? "LZMA-compressed bundle (not supported)" : "unknown compression " + std::to_string(kind);
  return std::nullopt;
}

// A serialized file's build target, rewritten in place when it names a desktop player (the same
// header walk as the APK writer's plan_platform_patch). True if it was rewritten.
bool RetargetSerialized(uint8_t* f, size_t n) {
  if (n < 48) return false;
  const uint32_t version = uint32_t(f[8]) << 24 | uint32_t(f[9]) << 16 | uint32_t(f[10]) << 8 | f[11];
  if (version < 9 || version > 30) return false;
  const size_t header = version >= 22 ? 48 : 20;
  const bool big = f[16] != 0;
  size_t end = header;
  while (end < n && end < header + 64 && f[end] != 0) {
    const uint8_t c = f[end];
    if (!((c >= '0' && c <= '9') || c == '.' || (c >= 'a' && c <= 'z'))) return false;
    ++end;
  }
  if (end + 5 > n || f[end] != 0 || end - header < 5 || f[header] < '1' || f[header] > '9' ||
      !std::memchr(f + header, '.', end - header))
    return false;
  uint8_t* t = f + end + 1;
  const uint32_t target = big ? (uint32_t(t[0]) << 24 | uint32_t(t[1]) << 16 | uint32_t(t[2]) << 8 | t[3])
                              : (uint32_t(t[3]) << 24 | uint32_t(t[2]) << 16 | uint32_t(t[1]) << 8 | t[0]);
  if (target != 2 && target != 5 && target != 19 && target != 24) return false;
  for (int i = 0; i < 4; ++i) t[big ? 3 - i : i] = uint8_t(kAndroid >> (8 * i));
  return true;
}

} // namespace

bool IsBundle(const uint8_t* data, size_t size) { return size >= 8 && std::memcmp(data, "UnityFS", 8) == 0; }

std::optional<std::vector<uint8_t>> Retarget(const uint8_t* data, size_t size, Stats* stats, std::string* error) {
  Stats local;
  Stats& st = stats ? *stats : local;
  st = Stats {};  // counts of this call only: "nothing to rewrite" is decided from them
  std::string scratch;
  std::string& err = error ? *error : scratch;
  err.clear();
  if (!IsBundle(data, size)) { err = "not a UnityFS bundle"; return std::nullopt; }
  Reader r {data, size};
  r.at = 8;
  const uint32_t format = uint32_t(r.Be(4));
  const std::string playerVersion = r.Str(), engineVersion = r.Str();
  r.Be(8);  // file size
  const uint32_t infoPacked = uint32_t(r.Be(4)), infoSize = uint32_t(r.Be(4)), flags = uint32_t(r.Be(4));
  if (!r.ok || format < 6 || format > 8) { err = "bundle format " + std::to_string(format) + " not read"; return std::nullopt; }
  if (format >= 7) r.Align(16);
  const size_t infoAt = (flags & kInfoAtEnd) ? size - infoPacked : r.at;
  if (infoAt + infoPacked > size) { err = "blocks info out of range"; return std::nullopt; }
  auto info = Decompress(data + infoAt, infoPacked, infoSize, flags & kCompressionMask, &err);
  if (!info) return std::nullopt;
  size_t blocksAt = (flags & kInfoAtEnd) ? r.at : infoAt + infoPacked;
  if (flags & kPadBeforeBlocks) blocksAt = (blocksAt + 15) / 16 * 16;

  struct Block { uint32_t size, packed; uint16_t flags; };
  struct Node { int64_t offset, size; uint32_t flags; std::string path; };
  Reader ir {info->data(), info->size()};
  ir.at = 16;  // hash
  std::vector<Block> blocks(size_t(std::min<uint64_t>(ir.Be(4), 1u << 20)));
  for (auto& b : blocks) b = {uint32_t(ir.Be(4)), uint32_t(ir.Be(4)), uint16_t(ir.Be(2))};
  std::vector<Node> nodes(size_t(std::min<uint64_t>(ir.Be(4), 1u << 20)));
  for (auto& nd : nodes) nd = {int64_t(ir.Be(8)), int64_t(ir.Be(8)), uint32_t(ir.Be(4)), ir.Str()};
  if (!ir.ok) { err = "malformed blocks info"; return std::nullopt; }

  std::vector<uint8_t> payload;
  size_t at = blocksAt;
  for (const auto& b : blocks) {
    if (at + b.packed > size) { err = "block out of range"; return std::nullopt; }
    auto plain = Decompress(data + at, b.packed, b.size, b.flags & kCompressionMask, &err);
    if (!plain) return std::nullopt;
    payload.insert(payload.end(), plain->begin(), plain->end());
    at += b.packed;
  }
  st.nodes = int(nodes.size());
  for (const auto& nd : nodes) {
    if (nd.offset < 0 || nd.size < 0 || uint64_t(nd.offset + nd.size) > payload.size()) {
      err = "directory entry out of range";
      return std::nullopt;
    }
    if (RetargetSerialized(payload.data() + nd.offset, size_t(nd.size))) ++st.retargeted;
  }
  if (!st.retargeted) return std::nullopt;

  // Written back: the same header, info right after it (uncompressed), the blocks uncompressed.
  std::vector<uint8_t> out;
  out.insert(out.end(), data, data + 8);
  PutBe(out, format, 4);
  out.insert(out.end(), playerVersion.begin(), playerVersion.end());
  out.push_back(0);
  out.insert(out.end(), engineVersion.begin(), engineVersion.end());
  out.push_back(0);
  const size_t sizeAt = out.size();
  PutBe(out, 0, 8);
  std::vector<uint8_t> newInfo(16, 0);
  PutBe(newInfo, blocks.size(), 4);
  for (const auto& b : blocks) {
    PutBe(newInfo, b.size, 4);
    PutBe(newInfo, b.size, 4);
    PutBe(newInfo, b.flags & ~kCompressionMask, 2);
  }
  PutBe(newInfo, nodes.size(), 4);
  for (const auto& nd : nodes) {
    PutBe(newInfo, uint64_t(nd.offset), 8);
    PutBe(newInfo, uint64_t(nd.size), 8);
    PutBe(newInfo, nd.flags, 4);
    newInfo.insert(newInfo.end(), nd.path.begin(), nd.path.end());
    newInfo.push_back(0);
  }
  const uint32_t newFlags = (flags & ~(kCompressionMask | kInfoAtEnd)) | kPadBeforeBlocks;
  PutBe(out, newInfo.size(), 4);
  PutBe(out, newInfo.size(), 4);
  PutBe(out, newFlags, 4);
  if (format >= 7) out.resize((out.size() + 15) / 16 * 16, 0);
  out.insert(out.end(), newInfo.begin(), newInfo.end());
  out.resize((out.size() + 15) / 16 * 16, 0);
  out.insert(out.end(), payload.begin(), payload.end());
  for (int i = 0; i < 8; ++i) out[sizeAt + i] = uint8_t(uint64_t(out.size()) >> (56 - 8 * i));
  return out;
}

} // namespace UnityBridge::UnityBundle
