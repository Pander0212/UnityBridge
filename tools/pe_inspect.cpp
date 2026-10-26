// SPDX-License-Identifier: GPL-3.0-or-later
// Reports how far the mini Win32 covers a DLL: its imports grouped by DLL with implemented /
// missing, plus sections, exports, TLS and unwind data. Reads the file only; nothing is mapped
// executable and no code of the DLL runs.
//
//   ub-pe-inspect GameAssembly.dll [--missing]
#include "loader/pe_image.h"
#include "win32/hle.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

int main(int argc, char** argv) {
  using namespace UnityBridge;
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s file.dll [--missing]\n", argv[0]);
    return 2;
  }
  const bool listMissing = argc > 2 && std::strcmp(argv[2], "--missing") == 0;
  std::ifstream in(argv[1], std::ios::binary);
  const std::vector<uint8_t> file {std::istreambuf_iterator<char>(in), {}};
  const auto result = Pe::Inspect(file);
  if (!result.image) {
    std::fprintf(stderr, "error: %s\n", result.error.c_str());
    return 1;
  }
  const auto& image = *result.image;
  std::printf("size of image %zu bytes, preferred base %#llx\n", image.size, static_cast<unsigned long long>(image.preferredBase));
  for (const auto& s : image.sections) std::printf("  section %-8s rva %#010x size %u\n", s.name.c_str(), s.rva, s.virtualSize);
  std::printf("exports %zu, unwind table %u entries\n", image.exports.size(), image.pdataSize / 12);

  std::map<std::string, std::pair<int, int>> perDll;  // implemented, total
  std::vector<std::string> missing;
  for (const auto& imp : image.imports) {
    const auto dll = Win32::Lower(imp.dll);
    auto& [implemented, total] = perDll[dll];
    ++total;
    bool found = false;
    for (const auto& f : Win32::Functions()) {
      if (f.dll == dll && f.name == imp.Symbol() && f.handler) { found = true; break; }
    }
    if (found) ++implemented;
    else missing.push_back(dll + "!" + imp.Symbol());
  }
  int implementedAll = 0;
  for (const auto& [dll, counts] : perDll) {
    std::printf("  %-14s %3d of %3d implemented\n", dll.c_str(), counts.first, counts.second);
    implementedAll += counts.first;
  }
  std::printf("imports: %d of %zu implemented\n", implementedAll, image.imports.size());
  if (listMissing) for (const auto& m : missing) std::printf("  missing %s\n", m.c_str());
  return 0;
}
