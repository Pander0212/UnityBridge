# SPDX-License-Identifier: GPL-3.0-or-later
# M4b: exports translation cases from a serialized file whose shaders carry both D3D11 (4) and GLES3x
# (9) programs, e.g. the Windows player's `unity default resources`: per shader, pass and keyword
# variant the D3D11 vertex and pixel DXBC (Unity's header stripped), Unity's parameter tables for
# them as text (tools/shader_xlat/main.cpp's format), and Unity's own GLES3 source as expected.glsl.
# Data only, no code runs. The cases are Unity's data: keep them out of the repository.
# Usage: python3 -I export_xlat_cases.py <serialized file> <output dir>
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import UnityPy
from shader_blob import Reader, blob_entries, platform_blob, platform_of, read_code, read_params


def strip_unity_header(code):
    return code[6 + (0x20 if code[0] >= 2 else 0):]


def entries_by_keywords(prog, platform):
    out = {}
    for group, pblobs in zip(prog.m_PlayerSubPrograms, prog.m_ParameterBlobIndices):
        for sp, pb in zip(group, pblobs):
            if platform_of(sp.m_GpuProgramType) == platform:
                out[tuple(sorted(sp.m_KeywordIndices))] = (sp.m_BlobIndex, pb)
    return out


def record(blob, index):
    o, l, _ = blob_entries(blob)[index]
    return Reader(blob[o:o + l])


def reflection_text(params, common, names):
    lines = []
    registers = {b['name']: b['index'] for b in params['bindings'] if b['kind'] == 'cbuffer'}
    for cb in params['cbuffers']:
        if not cb['name']:
            continue  # the base (unnamed) buffer: empty for D3D11
        lines.append(f'cbuffer {cb["name"]} {registers.get(cb["name"], 0)} {cb["size"]}')
        for v in cb['params']:
            lines.append(f'var {v["name"]} {v["type"]} {v["rows"]} {v["columns"]} {int(v["matrix"])} '
                         f'{v["arraySize"]} {v["offset"]}')
    textures = [(b['name'], b['index'], b['sampler'], b['dim'], b['msaa']) for b in params['bindings']
                if b['kind'] == 'texture']
    for t in common.m_TextureParams:
        textures.append((names.get(t.m_NameIndex, f'tex{t.m_NameIndex}'), t.m_Index, t.m_SamplerIndex, t.m_Dim,
                         int(t.m_MultiSampled)))
    for name, reg, sampler, dim, ms in textures:
        sampler = sampler if sampler is not None and sampler < 0x7fffffff else -1
        lines.append(f'texture {name} {reg} {sampler} {dim} {ms}')
    return '\n'.join(lines) + '\n'


def main():
    path, out = sys.argv[1], sys.argv[2]
    count = 0
    for obj in UnityPy.load(path).objects:
        if obj.type.name != 'Shader':
            continue
        sh = obj.read()
        if 4 not in list(sh.platforms) or 9 not in list(sh.platforms):
            continue
        dx, gl = platform_blob(sh, 4), platform_blob(sh, 9)
        sname = sh.m_ParsedForm.m_Name.replace('/', '_').replace(' ', '_')
        for si, sub in enumerate(sh.m_ParsedForm.m_SubShaders):
            for pi, p in enumerate(sub.m_Passes):
                names = {v: k for k, v in p.m_NameIndices}
                vs, ps = entries_by_keywords(p.progVertex, 4), entries_by_keywords(p.progFragment, 4)
                gles = entries_by_keywords(p.progVertex, 9)
                for kw, (vsi, vpi) in vs.items():
                    if kw not in ps or kw not in gles:
                        continue
                    psi, ppi = ps[kw]
                    d = os.path.join(out, f'{sname}.s{si}p{pi}.kw{"_".join(map(str, kw)) or "none"}')
                    os.makedirs(d, exist_ok=True)
                    for stage, ci, pi2, prog in (('vs', vsi, vpi, p.progVertex), ('ps', psi, ppi, p.progFragment)):
                        code = read_code(record(dx, ci))['code']
                        open(os.path.join(d, stage + '.dxbc'), 'wb').write(strip_unity_header(code))
                        params = read_params(record(dx, pi2))
                        open(os.path.join(d, stage + '.refl'), 'w').write(
                            reflection_text(params, prog.m_CommonParameters, names))
                    expected = read_code(record(gl, gles[kw][0]))['code'].decode('utf8')
                    open(os.path.join(d, 'expected.glsl'), 'w').write(expected)
                    count += 1
    print(count, 'cases in', out)


if __name__ == '__main__':
    main()
