// SPDX-License-Identifier: GPL-3.0-or-later
// Crunched textures to DXT: see unity_crunch.h.
#include "engine/unity_crunch.h"

#include <algorithm>
#include <cstring>  // crn_decomp.h uses memcpy/memset without including it

#include <crn_decomp.h>

namespace UnityBridge::UnityCrunch {

std::optional<std::vector<uint8_t>> Unpack(const uint8_t* data, size_t size, int width, int height, int levels,
                                           bool alpha, std::string* error) {
  const auto fail = [&](const std::string& why) -> std::optional<std::vector<uint8_t>> {
    if (error) *error = why;
    return std::nullopt;
  };
  if (size > 0xffffffffu) return fail("crunch data too large");
  crnd::crn_texture_info info;
  if (!crnd::crnd_get_texture_info(data, uint32_t(size), &info)) return fail("not crunch data");
  const crn_format want = alpha ? cCRNFmtDXT5 : cCRNFmtDXT1;
  if (int(info.m_width) != width || int(info.m_height) != height || info.m_faces != 1 || info.m_format != want ||
      int(info.m_levels) < levels)
    return fail("crunch data is " + std::to_string(info.m_width) + "x" + std::to_string(info.m_height) + ", " +
                std::to_string(info.m_levels) + " levels, format " + std::to_string(int(info.m_format)));
  crnd::crnd_unpack_context ctx = crnd::crnd_unpack_begin(data, uint32_t(size));
  if (!ctx) return fail("crunch tables do not unpack");
  const uint32_t blockBytes = alpha ? 16 : 8;
  std::vector<uint8_t> out;
  for (int level = 0; level < levels; ++level) {
    const uint32_t lw = uint32_t(std::max(1, width >> level)), lh = uint32_t(std::max(1, height >> level));
    const uint32_t bw = (lw + 3) / 4, bh = (lh + 3) / 4, pitch = bw * blockBytes;
    const size_t at = out.size();
    out.resize(at + size_t(pitch) * bh);
    void* faces[1] = {out.data() + at};
    if (!crnd::crnd_unpack_level(ctx, faces, pitch * bh, pitch, uint32_t(level))) {
      crnd::crnd_unpack_end(ctx);
      return fail("crunch level " + std::to_string(level) + " does not unpack");
    }
  }
  crnd::crnd_unpack_end(ctx);
  return out;
}

} // namespace UnityBridge::UnityCrunch
