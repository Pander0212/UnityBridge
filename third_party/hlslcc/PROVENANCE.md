# HLSLcc

- Source: https://github.com/Unity-Technologies/HLSLcc, commit `3ea1fcd6bd0ac445bc078de0bf32f0950188577b`
  (2020-09-23), fetched by `scripts/fetch-hlslcc.sh` into `build/hlslcc`; nothing of it is committed here.
- License: MIT (Copyright (c) 2012 James Jones, 2014-2016 Unity Technologies); the bundled bstring
  library is BSD-licensed. Both compatible with UnityBridge's GPL-3.0-or-later.
- Use (M4): translates the D3D11 shader bytecode (DXBC) of a game's shaders into GLSL ES 3.0 on the
  device, the translator Unity's own GLES3 output comes from. Built unmodified.
