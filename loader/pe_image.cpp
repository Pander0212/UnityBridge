// SPDX-License-Identifier: GPL-3.0-or-later
#include "pe_image.h"

#include <cstring>
#include <sys/mman.h>

namespace UnityBridge::Pe {
namespace {

// Directory indexes in the optional header.
enum : unsigned { kExport = 0, kImport = 1, kException = 3, kBaseReloc = 5, kTls = 9, kDelayImport = 13 };
constexpr uint16_t kMachineAmd64 = 0x8664;
constexpr uint16_t kPe32Plus = 0x20b;

// Bounds-checked little-endian reads; the file and the mapped image are untrusted input.
struct View {
  const uint8_t* data;
  size_t size;

  template <typename T>
  std::optional<T> Get(size_t offset) const {
    if (offset > size || size - offset < sizeof(T)) return std::nullopt;
    T value;
    std::memcpy(&value, data + offset, sizeof(T));
    return value;
  }
  std::optional<std::string> CString(size_t offset, size_t limit = 4096) const {
    if (offset >= size) return std::nullopt;
    const size_t max = std::min(limit, size - offset);
    const auto* start = reinterpret_cast<const char*>(data + offset);
    const auto* end = static_cast<const char*>(std::memchr(start, 0, max));
    if (end == nullptr) return std::nullopt;
    return std::string(start, end);
  }
};

struct Directory {
  uint32_t rva {}, size {};
};

struct Headers {
  uint16_t sectionCount {};
  size_t sectionTable {};  // file offset of the first section header
  uint32_t entryRva {}, sizeOfImage {}, sizeOfHeaders {};
  uint64_t imageBase {};
  Directory dirs[16] {};
};

std::optional<Headers> ReadHeaders(const View& file, std::string& error) {
  if (file.Get<uint16_t>(0) != uint16_t {0x5a4d}) { error = "not an MZ file"; return std::nullopt; }
  const auto lfanew = file.Get<uint32_t>(0x3c);
  if (!lfanew || file.Get<uint32_t>(*lfanew) != uint32_t {0x00004550}) { error = "no PE signature"; return std::nullopt; }
  const size_t coff = *lfanew + 4;
  const auto machine = file.Get<uint16_t>(coff);
  const auto sections = file.Get<uint16_t>(coff + 2);
  const auto optSize = file.Get<uint16_t>(coff + 16);
  if (!machine || !sections || !optSize) { error = "truncated COFF header"; return std::nullopt; }
  if (*machine != kMachineAmd64) { error = "not an x86-64 image"; return std::nullopt; }
  const size_t opt = coff + 20;
  if (file.Get<uint16_t>(opt) != kPe32Plus) { error = "not PE32+"; return std::nullopt; }

  Headers h;
  h.sectionCount = *sections;
  h.sectionTable = opt + *optSize;
  const auto entry = file.Get<uint32_t>(opt + 16);
  const auto base = file.Get<uint64_t>(opt + 24);
  const auto image = file.Get<uint32_t>(opt + 56);
  const auto headers = file.Get<uint32_t>(opt + 60);
  const auto dirCount = file.Get<uint32_t>(opt + 108);
  if (!entry || !base || !image || !headers || !dirCount) { error = "truncated optional header"; return std::nullopt; }
  h.entryRva = *entry;
  h.imageBase = *base;
  h.sizeOfImage = *image;
  h.sizeOfHeaders = *headers;
  if (h.sizeOfImage == 0 || h.sizeOfImage > (1u << 30) || h.sizeOfHeaders > h.sizeOfImage) {
    error = "implausible SizeOfImage";
    return std::nullopt;
  }
  for (unsigned i = 0; i < std::min<uint32_t>(*dirCount, 16); ++i) {
    const auto rva = file.Get<uint32_t>(opt + 112 + i * 8);
    const auto size = file.Get<uint32_t>(opt + 116 + i * 8);
    if (!rva || !size) { error = "truncated data directories"; return std::nullopt; }
    h.dirs[i] = {*rva, *size};
  }
  return h;
}

// Copies headers and section contents to their RVAs in `image` (zeroed, sizeOfImage bytes).
bool MapSections(const View& file, const Headers& h, uint8_t* image, std::vector<Section>& out, std::string& error) {
  std::memcpy(image, file.data, std::min<size_t>(h.sizeOfHeaders, file.size));
  for (unsigned i = 0; i < h.sectionCount; ++i) {
    const size_t at = h.sectionTable + i * 40;
    char name[9] {};
    if (at + 40 > file.size) { error = "truncated section table"; return false; }
    std::memcpy(name, file.data + at, 8);
    const auto vsize = *file.Get<uint32_t>(at + 8);
    const auto rva = *file.Get<uint32_t>(at + 12);
    const auto rawSize = *file.Get<uint32_t>(at + 16);
    const auto rawPtr = *file.Get<uint32_t>(at + 20);
    const auto flags = *file.Get<uint32_t>(at + 36);
    const size_t span = std::max(vsize, rawSize);
    if (rva >= h.sizeOfImage || span > h.sizeOfImage - rva) { error = std::string("section outside image: ") + name; return false; }
    const size_t copy = std::min<size_t>(rawSize, vsize ? vsize : rawSize);
    if (copy) {
      if (rawPtr > file.size || copy > file.size - rawPtr) { error = std::string("section data outside file: ") + name; return false; }
      std::memcpy(image + rva, file.data + rawPtr, copy);
    }
    out.push_back({name, rva, vsize, flags});
  }
  return true;
}

bool Relocate(uint8_t* image, const Headers& h, int64_t delta, std::string& error) {
  const auto dir = h.dirs[kBaseReloc];
  if (delta == 0 || dir.size == 0) return true;
  const View v {image, h.sizeOfImage};
  size_t at = dir.rva;
  const size_t end = size_t {dir.rva} + dir.size;
  if (end > h.sizeOfImage) { error = "relocations outside image"; return false; }
  while (at + 8 <= end) {
    const auto page = *v.Get<uint32_t>(at);
    const auto blockSize = *v.Get<uint32_t>(at + 4);
    if (blockSize < 8 || at + blockSize > end) { error = "bad relocation block"; return false; }
    for (size_t e = at + 8; e + 2 <= at + blockSize; e += 2) {
      const auto entry = *v.Get<uint16_t>(e);
      const unsigned type = entry >> 12;
      const size_t target = size_t {page} + (entry & 0xfff);
      if (type == 0) continue;  // IMAGE_REL_BASED_ABSOLUTE: padding
      if (type == 10) {         // IMAGE_REL_BASED_DIR64
        if (target + 8 > h.sizeOfImage) { error = "relocation target outside image"; return false; }
        uint64_t value;
        std::memcpy(&value, image + target, 8);
        value += delta;
        std::memcpy(image + target, &value, 8);
      } else if (type == 3) {   // IMAGE_REL_BASED_HIGHLOW
        if (target + 4 > h.sizeOfImage) { error = "relocation target outside image"; return false; }
        uint32_t value;
        std::memcpy(&value, image + target, 4);
        value += static_cast<uint32_t>(delta);
        std::memcpy(image + target, &value, 4);
      } else {
        error = "unsupported relocation type " + std::to_string(type);
        return false;
      }
    }
    at += blockSize;
  }
  return true;
}

// Walks one import name table; `slots` is the IAT that receives the bound addresses.
bool ReadThunks(const View& v, const std::string& dll, uint32_t names, uint32_t slots, bool delayed,
                std::vector<Import>& out, std::string& error) {
  for (unsigned i = 0;; ++i) {
    if (i > 65536) { error = "import table without end: " + dll; return false; }
    const auto thunk = v.Get<uint64_t>(size_t {names} + i * 8);
    if (!thunk) { error = "import table outside image: " + dll; return false; }
    if (*thunk == 0) return true;
    Import imp;
    imp.dll = dll;
    imp.slotRva = slots + i * 8;
    imp.delayed = delayed;
    if (*thunk & (1ull << 63)) {
      imp.ordinal = static_cast<uint16_t>(*thunk & 0xffff);
    } else {
      const auto name = v.CString(static_cast<size_t>(*thunk & 0x7fffffff) + 2);
      if (!name) { error = "bad import name in " + dll; return false; }
      imp.name = *name;
    }
    out.push_back(std::move(imp));
  }
}

bool ReadImports(const View& v, const Headers& h, std::vector<Import>& out, std::string& error) {
  const auto dir = h.dirs[kImport];
  for (size_t at = dir.rva; dir.size && at + 20 <= size_t {dir.rva} + dir.size; at += 20) {
    const auto ilt = v.Get<uint32_t>(at);
    const auto nameRva = v.Get<uint32_t>(at + 12);
    const auto iat = v.Get<uint32_t>(at + 16);
    if (!ilt || !nameRva || !iat) { error = "import directory outside image"; return false; }
    if (*nameRva == 0 && *iat == 0) break;
    const auto dll = v.CString(*nameRva);
    if (!dll) { error = "bad import DLL name"; return false; }
    if (!ReadThunks(v, *dll, *ilt ? *ilt : *iat, *iat, false, out, error)) return false;
  }
  const auto delay = h.dirs[kDelayImport];
  for (size_t at = delay.rva; delay.size && at + 32 <= size_t {delay.rva} + delay.size; at += 32) {
    const auto nameRva = v.Get<uint32_t>(at + 4);
    const auto iat = v.Get<uint32_t>(at + 12);
    const auto int_ = v.Get<uint32_t>(at + 16);
    if (!nameRva || !iat || !int_) { error = "delay-import directory outside image"; return false; }
    if (*nameRva == 0) break;
    const auto dll = v.CString(*nameRva);
    if (!dll) { error = "bad delay-import DLL name"; return false; }
    if (!ReadThunks(v, *dll, *int_, *iat, true, out, error)) return false;
  }
  return true;
}

void ReadExports(const View& v, const Headers& h, std::vector<std::pair<std::string, uint32_t>>& out) {
  const auto dir = h.dirs[kExport];
  if (dir.size < 40) return;
  const auto functions = v.Get<uint32_t>(dir.rva + 28);
  const auto names = v.Get<uint32_t>(dir.rva + 32);
  const auto ordinals = v.Get<uint32_t>(dir.rva + 36);
  const auto nameCount = v.Get<uint32_t>(dir.rva + 24);
  if (!functions || !names || !ordinals || !nameCount) return;
  for (uint32_t i = 0; i < *nameCount; ++i) {
    const auto nameRva = v.Get<uint32_t>(size_t {*names} + i * 4);
    const auto index = v.Get<uint16_t>(size_t {*ordinals} + i * 2);
    if (!nameRva || !index) return;
    const auto rva = v.Get<uint32_t>(size_t {*functions} + size_t {*index} * 4);
    const auto name = v.CString(*nameRva);
    if (!rva || !name) return;
    if (*rva >= dir.rva && *rva < dir.rva + dir.size) continue;  // forwarder string, not code
    out.emplace_back(*name, *rva);
  }
}

// Needs the image relocated: the TLS directory holds absolute addresses.
std::optional<Tls> ReadTls(const View& v, const Headers& h, uintptr_t base) {
  const auto dir = h.dirs[kTls];
  if (dir.size < 40) return std::nullopt;
  Tls tls;
  tls.rawDataStart = *v.Get<uint64_t>(dir.rva);
  tls.rawDataEnd = *v.Get<uint64_t>(dir.rva + 8);
  tls.indexAddress = *v.Get<uint64_t>(dir.rva + 16);
  const auto callbacks = *v.Get<uint64_t>(dir.rva + 24);
  tls.zeroFill = *v.Get<uint32_t>(dir.rva + 32);
  const auto inImage = [&](uint64_t va) { return va >= base && va < base + h.sizeOfImage; };
  for (uint64_t at = callbacks; callbacks && inImage(at) && inImage(at + 7); at += 8) {
    const auto cb = v.Get<uint64_t>(at - base);
    if (!cb || *cb == 0) break;
    tls.callbacks.push_back(*cb);
  }
  return tls;
}

LoadResult Fail(std::string error) {
  LoadResult r;
  r.error = std::move(error);
  return r;
}

} // namespace

std::optional<uintptr_t> Image::Export(std::string_view name) const {
  for (const auto& [exportName, rva] : exports) {
    if (exportName == name) return Address(rva);
  }
  return std::nullopt;
}

LoadResult Inspect(std::span<const uint8_t> bytes) {
  const View file {bytes.data(), bytes.size()};
  std::string error;
  const auto h = ReadHeaders(file, error);
  if (!h) return Fail(error);
  std::vector<uint8_t> scratch(h->sizeOfImage);
  Image image;
  if (!MapSections(file, *h, scratch.data(), image.sections, error)) return Fail(error);
  const View v {scratch.data(), scratch.size()};
  if (!ReadImports(v, *h, image.imports, error)) return Fail(error);
  ReadExports(v, *h, image.exports);
  image.size = h->sizeOfImage;
  image.preferredBase = h->imageBase;
  image.entryPoint = h->entryRva;  // an RVA here: nothing is mapped
  image.pdataRva = h->dirs[kException].rva;
  image.pdataSize = h->dirs[kException].size;
  LoadResult result;
  result.image = std::move(image);
  return result;
}

LoadResult Load(std::span<const uint8_t> bytes, const ImportResolver& resolve, bool executable) {
  const View file {bytes.data(), bytes.size()};
  std::string error;
  const auto h = ReadHeaders(file, error);
  if (!h) return Fail(error);

  const int prot = PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0);
  void* hint = reinterpret_cast<void*>(h->imageBase);
  void* mem = mmap(hint, h->sizeOfImage, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mem == MAP_FAILED) return Fail("mmap of the image failed");
  auto* base = static_cast<uint8_t*>(mem);

  Image image;
  image.base = base;
  image.size = h->sizeOfImage;
  image.preferredBase = h->imageBase;
  auto fail = [&](std::string message) {
    munmap(mem, h->sizeOfImage);
    return Fail(std::move(message));
  };
  if (!MapSections(file, *h, base, image.sections, error)) return fail(error);
  const auto delta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(base) - h->imageBase);
  if (!Relocate(base, *h, delta, error)) return fail(error);

  const View v {base, h->sizeOfImage};
  if (!ReadImports(v, *h, image.imports, error)) return fail(error);
  ReadExports(v, *h, image.exports);
  image.tls = ReadTls(v, *h, reinterpret_cast<uintptr_t>(base));
  image.entryPoint = h->entryRva ? image.Address(h->entryRva) : 0;
  image.pdataRva = h->dirs[kException].rva;
  image.pdataSize = h->dirs[kException].size;

  LoadResult result;
  for (const auto& imp : image.imports) {
    const auto address = resolve(imp);
    if (!address) result.unresolved.push_back(imp);
    const uint64_t value = address.value_or(0);
    std::memcpy(base + imp.slotRva, &value, 8);
  }
  result.image = std::move(image);
  return result;
}

void Unload(Image& image) {
  if (image.base) munmap(image.base, image.size);
  image.base = nullptr;
}

} // namespace UnityBridge::Pe
