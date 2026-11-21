# etcpak

- Source: https://github.com/wolfpld/etcpak, commit `3d716e1550023dc5ff8b602636af8b76584e0b66`
  (2026-09-13), fetched by `scripts/fetch-etcpak.sh` into `build/etcpak`; nothing of it is committed here.
- License: BSD-3-Clause (Copyright (c) 2013-2026, Bartosz Taudul). Compatible with UnityBridge's
  GPL-3.0-or-later.
- Use (M8): ETC2 encoding of desktop BC textures (DXT1 -> ETC2_RGB, DXT5 -> ETC2_RGBA8) at import. The
  iPhone's GPU has no BC support, and RGBA32 would make the textures 4-8 times larger. Only
  `ProcessRGB.cpp`, `Tables.cpp` and `Dither.cpp` are compiled, unmodified.
