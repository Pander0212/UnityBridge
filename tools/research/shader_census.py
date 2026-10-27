# SPDX-License-Identifier: GPL-3.0-or-later
# M4 research: reads Unity shader data with UnityPy (pip install UnityPy). Data only, no game code runs.
# Usage: python3 -I <script> <files of a <Name>_Data folder: globalgamemanagers, *.assets, level*>
import sys, collections, UnityPy
from UnityPy.enums import ShaderGpuProgramType
env = UnityPy.load(*sys.argv[1:])
types = collections.Counter(); stages = collections.Counter()
shaders = 0; passes = 0; blob = 0; compute = 0; names = []
for obj in env.objects:
    t = obj.type.name
    if t == 'ComputeShader': compute += 1
    if t != 'Shader': continue
    try: sh = obj.read()
    except Exception as e: print('unreadable shader', e); continue
    shaders += 1
    blob += sum(x if isinstance(x, int) else x[0] for x in sh.decompressedLengths)
    names.append(sh.m_ParsedForm.m_Name)
    for sub in sh.m_ParsedForm.m_SubShaders:
        for p in sub.m_Passes:
            passes += 1
            for stage in ('progVertex','progFragment','progGeometry','progHull','progDomain'):
                prog = getattr(p, stage)
                subs = list(prog.m_SubPrograms or [])
                for group in (getattr(prog, 'm_PlayerSubPrograms', None) or []):  # Unity 2021.3+: per platform
                    subs.extend(group if isinstance(group, list) else [group])
                for sp in subs:
                    stages[stage] += 1
                    try: types[ShaderGpuProgramType(sp.m_GpuProgramType).name] += 1
                    except ValueError: types[sp.m_GpuProgramType] += 1
print('shaders', shaders, 'passes', passes, 'compute shaders', compute)
print('subprograms by stage', dict(stages))
print('subprograms by GPU program type', dict(types))
print('decompressed shader blobs: %.1f MB' % (blob / 1e6))
print('shader names (first 40):', '; '.join(sorted(names)[:40]))
