// SPDX-License-Identifier: GPL-3.0-or-later
// Unity serialized files: see unity_file.h. Layout as UnityPy's SerializedFile reads it (format 22).
#include "engine/unity_file.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace UnityBridge::UnityFile {
namespace {

constexpr size_t kHeaderSize = 48;

uint64_t BigEndian(const uint8_t* p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; ++i) v = v << 8 | p[i];
  return v;
}

void PutBigEndian(uint8_t* p, uint64_t v, int n) {
  for (int i = n - 1; i >= 0; --i, v >>= 8) p[i] = static_cast<uint8_t>(v);
}

class Reader {
public:
  Reader(const uint8_t* data, size_t end, size_t pos) : data_(data), end_(end), pos(pos) {}
  bool ok = true;

  template <typename T> T Get() {
    T v {};
    if (pos + sizeof(T) > end_) { ok = false; return v; }
    std::memcpy(&v, data_ + pos, sizeof(T));
    pos += sizeof(T);
    return v;
  }
  void Skip(size_t n) {
    if (pos + n > end_) ok = false;
    else pos += n;
  }
  std::string CString() {
    const auto* start = data_ + pos;
    const auto* nul = static_cast<const uint8_t*>(std::memchr(start, 0, end_ - pos));
    if (!nul) { ok = false; return {}; }
    pos += size_t(nul - start) + 1;
    return std::string(reinterpret_cast<const char*>(start), size_t(nul - start));
  }
  void Align() { pos = (pos + 3) & ~size_t {3}; }

private:
  const uint8_t* data_;
  size_t end_;

public:
  size_t pos;
};

uint64_t Align8(uint64_t v) { return (v + 7) & ~uint64_t {7}; }

} // namespace

std::optional<File> Parse(const uint8_t* data, size_t size, std::string* error) {
  auto fail = [&](const std::string& why) -> std::optional<File> {
    if (error) *error = why;
    return std::nullopt;
  };
  if (size < kHeaderSize) return fail("too short for a serialized file");
  File f;
  f.version = static_cast<uint32_t>(BigEndian(data + 8, 4));
  if (f.version != 22) return fail("serialized file format " + std::to_string(f.version) + ", not 22");
  if (data[16] != 0) return fail("big-endian serialized file");
  const uint64_t metadataSize = BigEndian(data + 20, 4);
  const uint64_t fileSize = BigEndian(data + 24, 8);
  f.dataOffset = BigEndian(data + 32, 8);
  if (fileSize != size || kHeaderSize + metadataSize > f.dataOffset || f.dataOffset > size)
    return fail("header sizes do not match the file");
  f.header.assign(data, data + kHeaderSize);

  const size_t metaEnd = kHeaderSize + size_t(metadataSize);
  Reader r(data, metaEnd, kHeaderSize);
  f.unityVersion = r.CString();
  f.platform = r.Get<int32_t>();
  if (r.Get<uint8_t>() != 0) return fail("the file has type trees (not a player file)");
  const auto typeCount = r.Get<int32_t>();
  if (!r.ok || typeCount < 0 || typeCount > 100000) return fail("bad type table");
  for (int32_t i = 0; i < typeCount && r.ok; ++i) {
    const auto classId = r.Get<int32_t>();
    r.Skip(1 + 2);                  // stripped, script type index
    if (classId == 114) r.Skip(16);  // script id
    r.Skip(16);                      // old type hash
    f.typeClassIds.push_back(classId);
  }
  const auto objectCount = r.Get<int32_t>();
  if (!r.ok || objectCount < 0) return fail("bad type table");
  f.metaHead.assign(data + kHeaderSize, data + r.pos);
  for (int32_t i = 0; i < objectCount && r.ok; ++i) {
    r.Align();
    Object o;
    o.pathId = r.Get<int64_t>();
    o.byteStart = r.Get<uint64_t>();
    o.byteSize = r.Get<uint32_t>();
    o.typeIndex = r.Get<int32_t>();
    if (o.typeIndex < 0 || size_t(o.typeIndex) >= f.typeClassIds.size()) return fail("object with a bad type");
    if (f.dataOffset + o.byteStart + o.byteSize > size) return fail("object data past the end of the file");
    o.classId = f.typeClassIds[size_t(o.typeIndex)];
    f.objects.push_back(o);
  }
  if (!r.ok) return fail("object table ends early");
  f.metaTail.assign(data + r.pos, data + metaEnd);
  return f;
}

std::vector<uint8_t> Rebuild(const File& file, const uint8_t* data, size_t size,
                             const std::vector<Replacement>& replacements) {
  std::vector<const std::vector<uint8_t>*> replaced(file.objects.size(), nullptr);
  for (const auto& rep : replacements)
    if (rep.index < replaced.size()) replaced[rep.index] = &rep.data;

  // Objects keep their order in the data and the gaps between them; one that changes size moves the
  // ones after it by its change, rounded to 8 (Unity aligns object data to 8).
  std::vector<size_t> order(file.objects.size());
  std::iota(order.begin(), order.end(), size_t {0});
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return file.objects[a].byteStart < file.objects[b].byteStart; });
  std::vector<uint64_t> newStart(file.objects.size());
  std::vector<uint8_t> body;
  uint64_t copied = 0;  // original data bytes handled so far
  for (const size_t i : order) {
    const Object& o = file.objects[i];
    const uint8_t* base = data + file.dataOffset;
    if (o.byteStart > copied) body.insert(body.end(), base + copied, base + o.byteStart);  // the gap before it
    newStart[i] = body.size();
    if (replaced[i]) {
      // New data, padded to 8; the original's own padding is not copied, any gap beyond it is.
      body.insert(body.end(), replaced[i]->begin(), replaced[i]->end());
      body.resize(Align8(body.size()), 0);
      copied = std::max(copied, std::min<uint64_t>(o.byteStart + Align8(o.byteSize), size - file.dataOffset));
    } else {
      body.insert(body.end(), base + o.byteStart, base + o.byteStart + o.byteSize);
      copied = std::max(copied, o.byteStart + o.byteSize);
    }
  }
  if (file.dataOffset + copied < size) body.insert(body.end(), data + file.dataOffset + copied, data + size);

  std::vector<uint8_t> out(file.header);
  out.insert(out.end(), file.metaHead.begin(), file.metaHead.end());
  for (size_t i = 0; i < file.objects.size(); ++i) {
    while (out.size() % 4) out.push_back(0);
    const Object& o = file.objects[i];
    const uint32_t byteSize = replaced[i] ? static_cast<uint32_t>(replaced[i]->size()) : o.byteSize;
    const auto put = [&](const void* p, size_t n) {
      out.insert(out.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    };
    put(&o.pathId, 8);
    put(&newStart[i], 8);
    put(&byteSize, 4);
    put(&o.typeIndex, 4);
  }
  out.insert(out.end(), file.metaTail.begin(), file.metaTail.end());
  out.resize(file.dataOffset, 0);  // same metadata size: the data starts where it did
  out.insert(out.end(), body.begin(), body.end());
  PutBigEndian(out.data() + 24, out.size(), 8);
  return out;
}

} // namespace UnityBridge::UnityFile
