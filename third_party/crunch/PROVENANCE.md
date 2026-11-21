# crunch (Unity's fork)

- Source: https://github.com/Unity-Technologies/crunch, branch `unity`, commit
  `8708900eca8ec609d279270e72936258f81ddfb7` (2018-10-23), fetched by `scripts/fetch-crunch.sh` into
  `build/crunch`. Nothing of it is committed here.
- License: ZLIB (Copyright (c) 2010-2016 Richard Geldreich, Jr. and Binomial LLC). Compatible with
  UnityBridge's GPL-3.0-or-later.
- Use (M8): decoding DXT1Crunched/DXT5Crunched textures (Unity 2017.3+ format) to DXT1/DXT5 at import;
  they are then re-encoded to ETC2 like any other DXT texture. Only the header-only decoder
  `inc/crn_decomp.h` is compiled (in `engine/unity_crunch.cpp`), unmodified. On Apple platforms
  `compat/crunch/malloc.h` stands in for the `<malloc.h>` it includes.
