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

// CRC-32 (zlib's: reflected, polynomial 0xEDB88320, inverted in and out).
struct Crc32 {
  uint32_t table[256];
  uint8_t byTop[256];  // the index whose table entry has this top byte
  Crc32() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
      byTop[c >> 24] = uint8_t(i);
    }
  }
  uint32_t Of(const uint8_t* p, size_t n, uint32_t crc = 0) const {
    uint32_t r = ~crc;
    for (size_t i = 0; i < n; ++i) r = table[(r ^ p[i]) & 0xff] ^ (r >> 8);
    return ~r;
  }
  // Four bytes that, appended to data whose CRC is `crc`, give `want`: the table indices are found
  // walking back from the wanted register, the bytes walking forward from the current one.
  void Forge(uint32_t crc, uint32_t want, uint8_t out[4]) const {
    uint8_t index[4];
    uint32_t r = ~want;
    for (int k = 3; k >= 0; --k) {
      index[k] = byTop[r >> 24];
      r = (r ^ table[index[k]]) << 8;
    }
    r = ~crc;
    for (int k = 0; k < 4; ++k) {
      out[k] = uint8_t((r ^ index[k]) & 0xff);
      r = table[index[k]] ^ (r >> 8);
    }
  }
};

const Crc32& Crc() {
  static const Crc32 crc;
  return crc;
}

} // namespace

bool IsBundle(const uint8_t* data, size_t size) { return size >= 8 && std::memcmp(data, "UnityFS", 8) == 0; }

namespace {

struct Block { uint32_t size, packed; uint16_t flags; };
struct Node { int64_t offset, size; uint32_t flags; std::string path; };
struct Unpacked {
  uint32_t format = 0, flags = 0;
  std::string playerVersion, engineVersion;
  std::vector<Block> blocks;
  std::vector<Node> nodes;
  std::vector<uint8_t> payload;  // the blocks, uncompressed and concatenated
};

std::optional<Unpacked> Unpack(const uint8_t* data, size_t size, std::string& err) {
  if (!IsBundle(data, size)) { err = "not a UnityFS bundle"; return std::nullopt; }
  Unpacked u;
  Reader r {data, size};
  r.at = 8;
  u.format = uint32_t(r.Be(4));
  u.playerVersion = r.Str();
  u.engineVersion = r.Str();
  r.Be(8);  // file size
  const uint32_t infoPacked = uint32_t(r.Be(4)), infoSize = uint32_t(r.Be(4));
  u.flags = uint32_t(r.Be(4));
  if (!r.ok || u.format < 6 || u.format > 8) { err = "bundle format " + std::to_string(u.format) + " not read"; return std::nullopt; }
  if (u.format >= 7) r.Align(16);
  const size_t infoAt = (u.flags & kInfoAtEnd) ? size - infoPacked : r.at;
  if (infoPacked > size || infoAt + infoPacked > size) { err = "blocks info out of range"; return std::nullopt; }
  auto info = Decompress(data + infoAt, infoPacked, infoSize, u.flags & kCompressionMask, &err);
  if (!info) return std::nullopt;
  size_t blocksAt = (u.flags & kInfoAtEnd) ? r.at : infoAt + infoPacked;
  if (u.flags & kPadBeforeBlocks) blocksAt = (blocksAt + 15) / 16 * 16;
  Reader ir {info->data(), info->size()};
  ir.at = 16;  // hash
  u.blocks.resize(size_t(std::min<uint64_t>(ir.Be(4), 1u << 20)));
  for (auto& b : u.blocks) b = {uint32_t(ir.Be(4)), uint32_t(ir.Be(4)), uint16_t(ir.Be(2))};
  u.nodes.resize(size_t(std::min<uint64_t>(ir.Be(4), 1u << 20)));
  for (auto& nd : u.nodes) nd = {int64_t(ir.Be(8)), int64_t(ir.Be(8)), uint32_t(ir.Be(4)), ir.Str()};
  if (!ir.ok) { err = "malformed blocks info"; return std::nullopt; }
  size_t at = blocksAt;
  for (const auto& b : u.blocks) {
    if (at + b.packed > size) { err = "block out of range"; return std::nullopt; }
    auto plain = Decompress(data + at, b.packed, b.size, b.flags & kCompressionMask, &err);
    if (!plain) return std::nullopt;
    u.payload.insert(u.payload.end(), plain->begin(), plain->end());
    at += b.packed;
  }
  for (const auto& nd : u.nodes) {
    if (nd.offset < 0 || nd.size < 0 || uint64_t(nd.offset + nd.size) > u.payload.size()) {
      err = "directory entry out of range";
      return std::nullopt;
    }
  }
  return u;
}

} // namespace

std::optional<uint32_t> BlocksCrc(const uint8_t* data, size_t size, std::string* error) {
  std::string scratch;
  std::string& err = error ? *error : scratch;
  err.clear();
  const auto u = Unpack(data, size, err);
  if (!u) return std::nullopt;
  return Crc().Of(u->payload.data(), u->payload.size());
}

namespace {

// The bundle written back: the header of `u`, its blocks info right after the header (uncompressed),
// then `files` concatenated, uncompressed, in blocks of at most 1 MiB, and four bytes after the files
// (in no directory entry) that make the blocks' CRC-32 `crc`. Addressables' catalog holds each
// bundle's CRC-32 over its uncompressed blocks, and the engine refuses one that does not match ("CRC
// Mismatch", run 85).
std::vector<uint8_t> Pack(const Unpacked& u, const uint8_t* signature, const std::vector<File>& files, uint32_t crc) {
  std::vector<uint8_t> payload;
  std::vector<Node> nodes;
  for (const auto& f : files) {
    nodes.push_back({int64_t(payload.size()), int64_t(f.data.size()), f.flags, f.path});
    payload.insert(payload.end(), f.data.begin(), f.data.end());
  }
  uint8_t tail[4];
  Crc().Forge(Crc().Of(payload.data(), payload.size()), crc, tail);
  payload.insert(payload.end(), tail, tail + 4);
  constexpr size_t kBlock = size_t {1} << 20;
  std::vector<uint8_t> info(16, 0);
  const size_t blockCount = (payload.size() + kBlock - 1) / kBlock;
  PutBe(info, blockCount, 4);
  for (size_t i = 0; i < blockCount; ++i) {
    const size_t n = std::min(kBlock, payload.size() - i * kBlock);
    PutBe(info, n, 4);
    PutBe(info, n, 4);
    PutBe(info, 0, 2);  // uncompressed
  }
  PutBe(info, nodes.size(), 4);
  for (const auto& nd : nodes) {
    PutBe(info, uint64_t(nd.offset), 8);
    PutBe(info, uint64_t(nd.size), 8);
    PutBe(info, nd.flags, 4);
    info.insert(info.end(), nd.path.begin(), nd.path.end());
    info.push_back(0);
  }
  std::vector<uint8_t> out(signature, signature + 8);
  PutBe(out, u.format, 4);
  out.insert(out.end(), u.playerVersion.begin(), u.playerVersion.end());
  out.push_back(0);
  out.insert(out.end(), u.engineVersion.begin(), u.engineVersion.end());
  out.push_back(0);
  const size_t sizeAt = out.size();
  PutBe(out, 0, 8);
  PutBe(out, info.size(), 4);
  PutBe(out, info.size(), 4);
  PutBe(out, (u.flags & ~(kCompressionMask | kInfoAtEnd)) | kPadBeforeBlocks, 4);
  if (u.format >= 7) out.resize((out.size() + 15) / 16 * 16, 0);
  out.insert(out.end(), info.begin(), info.end());
  out.resize((out.size() + 15) / 16 * 16, 0);
  out.insert(out.end(), payload.begin(), payload.end());
  for (int i = 0; i < 8; ++i) out[sizeAt + i] = uint8_t(uint64_t(out.size()) >> (56 - 8 * i));
  return out;
}

} // namespace

bool RetargetFile(std::vector<uint8_t>& file) { return RetargetSerialized(file.data(), file.size()); }

bool IsSerializedFile(const uint8_t* data, size_t size) {
  if (size < 48) return false;
  const uint32_t version = uint32_t(data[8]) << 24 | uint32_t(data[9]) << 16 | uint32_t(data[10]) << 8 | data[11];
  return version >= 9 && version <= 30 && (data[16] == 0 || data[16] == 1);
}

std::optional<std::vector<uint8_t>> Transform(const uint8_t* data, size_t size, const Edit& edit, std::string* error) {
  std::string scratch;
  std::string& err = error ? *error : scratch;
  err.clear();
  auto u = Unpack(data, size, err);
  if (!u) return std::nullopt;
  const uint32_t crc = Crc().Of(u->payload.data(), u->payload.size());
  std::vector<File> files;
  for (const auto& nd : u->nodes)
    files.push_back({nd.path, nd.flags,
                     std::vector<uint8_t>(u->payload.begin() + nd.offset, u->payload.begin() + nd.offset + nd.size)});
  if (!edit(files)) return std::nullopt;
  return Pack(*u, data, files, crc);
}

std::optional<std::vector<uint8_t>> Retarget(const uint8_t* data, size_t size, Stats* stats, std::string* error) {
  Stats local;
  Stats& st = stats ? *stats : local;
  st = Stats {};  // counts of this call only: "nothing to rewrite" is decided from them
  return Transform(
      data, size,
      [&](std::vector<File>& files) {
        st.nodes = int(files.size());
        for (auto& f : files)
          if (RetargetSerialized(f.data.data(), f.data.size())) ++st.retargeted;
        return st.retargeted > 0;
      },
      error);
}

} // namespace UnityBridge::UnityBundle
