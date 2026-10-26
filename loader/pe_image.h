// SPDX-License-Identifier: GPL-3.0-or-later
// Loads an x86-64 Windows DLL (PE32+) into this process's memory the way the Windows loader would:
// sections at their RVAs, base relocations, imports bound through a caller-supplied resolver.
// Nothing here executes the image; on iPhone FEXCore runs its code, in host tests the CPU does.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace UnityBridge::Pe {

struct Import {
  std::string dll;      // as named in the import directory, e.g. "KERNEL32.dll"
  std::string name;     // empty when imported by ordinal
  uint16_t ordinal {};  // valid when name is empty
  uint32_t slotRva {};  // IAT slot that receives the resolved address
  bool delayed {};      // from the delay-load directory

  // The name, or "#<ordinal>" for imports by ordinal (OLEAUT32 and WS2_32 are imported that way).
  std::string Symbol() const { return name.empty() ? "#" + std::to_string(ordinal) : name; }
};

struct Section {
  std::string name;
  uint32_t rva {};
  uint32_t virtualSize {};
  uint32_t characteristics {};
};

struct Tls {
  uintptr_t rawDataStart {}, rawDataEnd {};  // template copied into each thread's TLS block
  uintptr_t indexAddress {};                 // where the loader stores this module's TLS slot
  uint32_t zeroFill {};
  std::vector<uintptr_t> callbacks;
};

struct Image {
  uint8_t* base {};
  size_t size {};
  uintptr_t preferredBase {};
  uintptr_t entryPoint {};  // DllMain, or 0
  std::vector<Section> sections;
  std::vector<Import> imports;
  std::vector<std::pair<std::string, uint32_t>> exports;  // name -> RVA (forwarders left out)
  std::optional<Tls> tls;
  uint32_t pdataRva {}, pdataSize {};  // RUNTIME_FUNCTION table for x64 unwinding

  uintptr_t Address(uint32_t rva) const { return reinterpret_cast<uintptr_t>(base) + rva; }
  std::optional<uintptr_t> Export(std::string_view name) const;
};

// Returns the address an import is bound to, or nullopt if it is unknown.
using ImportResolver = std::function<std::optional<uintptr_t>(const Import&)>;

struct LoadResult {
  std::optional<Image> image;
  std::string error;                  // set when image is empty
  std::vector<Import> unresolved;     // bound to 0; calling them would crash
};

// Parses only: headers, sections, import/export names. No memory is mapped.
LoadResult Inspect(std::span<const uint8_t> file);

// Maps the image into fresh read-write memory (code stays readable data: FEXCore translates it,
// the host CPU does not execute it on iPhone), applies relocations and binds imports.
// Host tests pass executable=true to run x86-64 code natively.
LoadResult Load(std::span<const uint8_t> file, const ImportResolver& resolve, bool executable = false);

void Unload(Image& image);

} // namespace UnityBridge::Pe
