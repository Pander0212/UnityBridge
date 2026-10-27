# SPDX-License-Identifier: GPL-3.0-or-later
# M4 research: reads Unity shader data with UnityPy (pip install UnityPy). Data only, no game code runs.
# Usage: python3 -I <script> <files of a <Name>_Data folder: globalgamemanagers, *.assets, level*>
import sys, struct, collections, UnityPy
from UnityPy.helpers import CompressionHelper
env = UnityPy.load(*sys.argv[1:])
chunkSets = collections.Counter(); total = 0; rdefNames = collections.Counter()
for obj in env.objects:
    if obj.type.name != 'Shader': continue
    sh = obj.read()
    get = lambda a, i: a[i][0] if isinstance(a[i], list) else a[i]
    blob = bytes(sh.compressedBlob)
    for i in range(len(sh.platforms)):
        raw = CompressionHelper.decompress_lz4(blob[get(sh.offsets,i):get(sh.offsets,i)+get(sh.compressedLengths,i)], get(sh.decompressedLengths,i))
        at = raw.find(b'DXBC')
        while at >= 0:
            size, count = struct.unpack_from('<II', raw, at + 24)
            if count < 32 and at + 32 + 4 * count <= len(raw):
                total += 1
                offs = struct.unpack_from('<%dI' % count, raw, at + 32)
                tags = [raw[at+o:at+o+4].decode('latin1') for o in offs]
                chunkSets[' '.join(tags)] += 1
                if 'RDEF' in tags:  # resource names: constant buffers and bound resources
                    o = at + offs[tags.index('RDEF')] + 8
                    cbCount, cbOff, rbCount, rbOff = struct.unpack_from('<IIII', raw, o)
                    for r in range(min(rbCount, 64)):
                        nameOff = struct.unpack_from('<I', raw, o + rbOff + r * 32)[0]
                        end = raw.find(b'\0', o + nameOff)
                        rdefNames[raw[o + nameOff:end].decode('latin1')] += 1
            at = raw.find(b'DXBC', at + 4)
print('DXBC containers', total)
print('chunk sets:'); [print('  %5d  %s' % (n, k)) for k, n in chunkSets.most_common(8)]
print('most common bound resource names (RDEF):', rdefNames.most_common(15))
