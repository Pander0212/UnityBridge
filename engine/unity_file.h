// SPDX-License-Identifier: GPL-3.0-or-later
// Unity serialized files (format 22, Unity 2021.3 players), read and written back (M4c). A player
// file is a header, metadata (Unity version, platform, the type table, the object table, scripts,
// externals, ref types, user info) and the objects' data. Parse keeps the metadata it does not need
// as bytes; Rebuild writes the file again with some objects' data replaced, moving the objects after
// them and nothing else. Without replacements it gives back the same bytes.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace UnityBridge::UnityFile {

struct Object {
  int64_t pathId = 0;
  uint64_t byteStart = 0;  // from the start of the data
  uint32_t byteSize = 0;
  int32_t typeIndex = 0;
  int32_t classId = 0;     // of its type
};

struct File {
  uint32_t version = 0;
  std::string unityVersion;
  int32_t platform = 0;
  std::vector<int32_t> typeClassIds;
  std::vector<Object> objects;  // in table order

  // Kept as they are for Rebuild.
  std::vector<uint8_t> header;    // the 48 header bytes
  std::vector<uint8_t> metaHead;  // metadata up to and including the object count
  std::vector<uint8_t> metaTail;  // metadata after the object table
  uint64_t dataOffset = 0;
};

// Parses `data` (a whole serialized file). Nullopt and `error` if it is not a format 22 file without
// type trees, the format 2021.3 players use.
std::optional<File> Parse(const uint8_t* data, size_t size, std::string* error);

// One object's new data, by its index in File::objects.
struct Replacement {
  size_t index;
  std::vector<uint8_t> data;
};

// The file again, from its original bytes `data`, with `replacements` applied.
std::vector<uint8_t> Rebuild(const File& file, const uint8_t* data, size_t size,
                             const std::vector<Replacement>& replacements);

} // namespace UnityBridge::UnityFile
