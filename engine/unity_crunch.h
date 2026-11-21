// SPDX-License-Identifier: GPL-3.0-or-later
// Crunched textures (DXT1Crunched 28, DXT5Crunched 29; Unity's crunch format since 2017.3) unpacked
// to their DXT1/DXT5 blocks (M8), with Unity's fork of crunch (build/crunch, scripts/fetch-crunch.sh).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace UnityBridge::UnityCrunch {

// The DXT blocks of `levels` mip levels, level after level, as a DXT1 (`alpha` false) or DXT5 texture
// of `width` x `height` stores them. Nullopt and `error` if the data is not a crunch file of that size
// and kind, or has fewer levels.
std::optional<std::vector<uint8_t>> Unpack(const uint8_t* data, size_t size, int width, int height, int levels,
                                           bool alpha, std::string* error);

} // namespace UnityBridge::UnityCrunch
