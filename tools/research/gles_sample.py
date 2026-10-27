# SPDX-License-Identifier: GPL-3.0-or-later
# M4 research: reads Unity shader data with UnityPy (pip install UnityPy). Data only, no game code runs.
# Usage: python3 -I <script> <files of a <Name>_Data folder: globalgamemanagers, *.assets, level*>
import sys, UnityPy
from UnityPy.helpers import CompressionHelper
env = UnityPy.load(sys.argv[1])
for obj in env.objects:
    if obj.type.name != 'Shader': continue
    sh = obj.read()
    get = lambda a, i: a[i][0] if isinstance(a[i], list) else a[i]
    blob = bytes(sh.compressedBlob)
    print('shader', sh.m_ParsedForm.m_Name, 'platforms', list(sh.platforms))
    for i, plat in enumerate(sh.platforms):
        raw = CompressionHelper.decompress_lz4(blob[get(sh.offsets,i):get(sh.offsets,i)+get(sh.compressedLengths,i)], get(sh.decompressedLengths,i))
        at = raw.find(b'#version 300 es')
        if at >= 0:
            end = raw.find(b'\0', at)
            print('--- platform', plat, 'GLSL ES 3.0 program,', end - at, 'bytes ---')
            print(raw[at:min(end, at + 2600)].decode('utf-8', 'replace'))
            sys.exit(0)
