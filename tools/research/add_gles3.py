# SPDX-License-Identifier: GPL-3.0-or-later
# M4c prototype: gives every Shader in a serialized file that has D3D11 programs but no GLES3x ones a
# GLES3x platform, translated by ub-shader-xlat (HLSLcc), in the layout Unity's own GLES3 programs have
# (docs/STATUS.md, M4a): one combined VS+FS source per variant on the vertex stage, a parameter record
# each, a platform-9 blob. The device does the same in C++ (M4c); this is the reference it is checked
# against. Data only; the output is the game's (or Unity's) data and stays out of the repository.
# Usage: python3 -I add_gles3.py <in file> <out file> <ub-shader-xlat> [--replace]
#   --replace: drop an existing GLES3x platform first (to check against Unity's own translation)
import os, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import UnityPy
from UnityPy.helpers import CompressionHelper
from export_xlat_cases import reflection_text, strip_unity_header
from shader_blob import Reader, platform_of, read_code, read_params

GLES3, D3D11 = 9, 4
TYPE_GLES3 = 4  # kShaderGpuProgramGLES3
VERSION = 202012090


class Out:
    def __init__(self):
        self.b = bytearray()

    def i32(self, v):
        self.b += struct.pack('<i', v)

    def u32(self, v):
        self.b += struct.pack('<I', v)

    def align(self):
        while len(self.b) % 4:
            self.b += b'\0'

    def string(self, s):
        e = s.encode('utf8')
        self.i32(len(e))
        self.b += e
        self.align()

    def bytes(self, data):
        self.i32(len(data))
        self.b += data
        self.align()


def code_record(source, keywords, source_map):
    o = Out()
    o.i32(VERSION)
    o.i32(TYPE_GLES3)
    for _ in range(4):
        o.i32(0)  # stats
    o.i32(len(keywords))
    for k in keywords:
        o.string(k)
    o.bytes(source.encode('utf8'))
    o.i32(source_map)
    o.i32(0)  # no bind channels: GLSL attributes go by name
    return bytes(o.b)


def params_record(globals_vars, globals_size, ubos, textures):
    """globals_vars: [(name,type,rows,cols,matrix,array,offset)]; ubos: [(name,size,vars,binding)]."""
    o = Out()
    o.i32(VERSION)
    o.i32(2 + len(ubos))
    for name, size, vars_ in [('', 0, [])] + [('$Globals', globals_size, globals_vars)] + \
                             [(n, s, v) for n, s, v, _ in ubos]:
        o.string(name)
        o.i32(size)
        o.i32(len(vars_))
        for vn, t, r, c, m, a, off in vars_:
            o.string(vn)
            for x in (t, r, c, int(m), a, off):
                o.i32(x)
        o.i32(0)  # no struct parameters
    o.i32(len(textures) + len(ubos))
    for name, unit, sampler, dim, ms in textures:
        o.string(name)
        o.i32(0)
        o.i32(unit)
        o.i32(sampler)
        o.u32(dim << 1 | ms)
    for name, _, _, binding in ubos:
        o.string(name)
        o.i32(1)
        o.i32(binding)
        o.i32(0)
    return bytes(o.b)


def blob(records):
    head = struct.pack('<i', len(records))
    pos = 4 + 12 * len(records)
    index, body = b'', b''
    for r in records:
        index += struct.pack('<iii', pos + len(body), len(r), 0)
        body += r
    return head + index + body


def lz4_literals(data):
    """An LZ4 block of literals only: valid for any LZ4 decoder, no compression."""
    n = len(data)
    out = bytearray([min(n, 15) << 4])
    if n >= 15:
        rest = n - 15
        while rest >= 255:
            out.append(255)
            rest -= 255
        out.append(rest)
    return bytes(out) + data


def platform_blob(tree, platform):
    i = tree['platforms'].index(platform)
    off, cl, dl = tree['offsets'][i][0], tree['compressedLengths'][i][0], tree['decompressedLengths'][i][0]
    return CompressionHelper.decompress_lz4(bytes(tree['compressedBlob'][off:off + cl]), dl)


def record(b, index):
    o, l, _ = struct.unpack_from('<iii', b, 4 + 12 * index)
    return b[o:o + l]


def drop_platform(tree, platform):
    i = tree['platforms'].index(platform)
    for key in ('platforms', 'offsets', 'compressedLengths', 'decompressedLengths', 'stageCounts'):
        del tree[key][i]
    for sub in tree['m_ParsedForm']['m_SubShaders']:
        for p in sub['m_Passes']:
            for stage in ('progVertex', 'progFragment', 'progGeometry', 'progHull', 'progDomain', 'progRayTracing'):
                prog = p.get(stage)
                if not prog:
                    continue
                for g in range(len(prog['m_PlayerSubPrograms'])):
                    keep = [k for k, sp in enumerate(prog['m_PlayerSubPrograms'][g])
                            if platform_of(sp['m_GpuProgramType']) != platform]
                    prog['m_PlayerSubPrograms'][g] = [prog['m_PlayerSubPrograms'][g][k] for k in keep]
                    if prog['m_ParameterBlobIndices'][g]:
                        prog['m_ParameterBlobIndices'][g] = [prog['m_ParameterBlobIndices'][g][k] for k in keep]


def parse_params_out(path):
    buffers, textures, cur = [], [], None
    for line in open(path):
        w = line.split()
        if w[0] == 'buffer':
            cur = {'name': w[1], 'binding': int(w[5])}
            buffers.append(cur)
        elif w[0] == 'texture':
            textures.append((w[1], int(w[3]), int(w[5]), int(w[7])))
    return buffers, textures


def translate(xlat, dx, vs_code, vs_params, ps_code, ps_params, p, names):
    with tempfile.TemporaryDirectory() as d:
        dx_tables = {}
        for stage, code_i, param_i, prog in (('vs', vs_code, vs_params, p['progVertex']),
                                             ('ps', ps_code, ps_params, p['progFragment'])):
            code = read_code(Reader(record(dx, code_i)))['code']
            open(os.path.join(d, stage + '.dxbc'), 'wb').write(strip_unity_header(code))
            params = read_params(Reader(record(dx, param_i)))
            dx_tables[stage] = params
            open(os.path.join(d, stage + '.refl'), 'w').write(reflection_text(params, Common(prog), names))
        r = subprocess.run([xlat, d], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.strip())
        source = open(os.path.join(d, 'out.glsl')).read()
        buffers, gl_textures = parse_params_out(os.path.join(d, 'out.params'))
    # The GLSL side's table: members and sizes from the D3D11 tables (HLSLcc reports no offsets).
    dx_cbs = {}
    for stage in ('vs', 'ps'):
        for cb in dx_tables[stage]['cbuffers']:
            if not cb['name']:
                continue
            e = dx_cbs.setdefault(cb['name'], {'size': 0, 'vars': {}})
            e['size'] = max(e['size'], cb['size'])
            for v in cb['params']:
                e['vars'][v['name']] = (v['name'], v['type'], v['rows'], v['columns'], v['matrix'], v['arraySize'],
                                        v['offset'])
    globals_vars, globals_size, ubos = [], 0, []
    for b in buffers:
        dxname = b['name']
        src = dx_cbs.get(dxname, {'size': 0, 'vars': {}})
        if b['binding'] < 0:
            globals_vars, globals_size = list(src['vars'].values()), src['size']
        else:
            ubos.append((dxname, src['size'], list(src['vars'].values()), b['binding']))
    textures = [(n, unit, sampler, dim_unity(dim), 0) for n, unit, sampler, dim in gl_textures]
    return source, params_record(globals_vars, globals_size, ubos, textures)


def dim_unity(hlslcc_dim):  # HLSLCC_TEX_DIMENSION -> Unity's TextureDimension
    return {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 6}.get(hlslcc_dim, 2)


class Common:  # the bits of m_CommonParameters reflection_text reads, from the typetree dict
    def __init__(self, prog):
        self.m_TextureParams = [Tex(t) for t in prog['m_CommonParameters']['m_TextureParams']]


class Tex:
    def __init__(self, t):
        self.m_NameIndex, self.m_Index, self.m_SamplerIndex = t['m_NameIndex'], t['m_Index'], t['m_SamplerIndex']
        self.m_Dim, self.m_MultiSampled = t['m_Dim'], t['m_MultiSampled']


def add_gles3(tree, xlat, keyword_names):
    dx = platform_blob(tree, D3D11)
    records, done, failed = [], 0, 0
    for sub in tree['m_ParsedForm']['m_SubShaders']:
        for p in sub['m_Passes']:
            names = {v: k for k, v in p['m_NameIndices']}
            vsp, psp = p['progVertex'], p['progFragment']
            for g, group in enumerate(vsp['m_PlayerSubPrograms']):
                ps_by_kw = {tuple(sorted(sp['m_KeywordIndices'])): (sp, psp['m_ParameterBlobIndices'][g][k])
                            for k, sp in enumerate(psp['m_PlayerSubPrograms'][g])
                            if platform_of(sp['m_GpuProgramType']) == D3D11}
                added, added_params = [], []
                for k, sp in enumerate(group):
                    if platform_of(sp['m_GpuProgramType']) != D3D11:
                        continue
                    kw = tuple(sorted(sp['m_KeywordIndices']))
                    if kw not in ps_by_kw:
                        continue
                    ps_sp, ps_param = ps_by_kw[kw]
                    vs_rec = read_code(Reader(record(dx, sp['m_BlobIndex'])))
                    try:
                        source, params = translate(xlat, dx, sp['m_BlobIndex'], vsp['m_ParameterBlobIndices'][g][k],
                                                   ps_sp['m_BlobIndex'], ps_param, p, names)
                    except RuntimeError as e:
                        failed += 1
                        print('  not translated:', e)
                        continue
                    records.append(params)
                    param_index = len(records) - 1
                    records.append(code_record(source, vs_rec['keywords'], vs_rec['sourceMap']))
                    added.append({'m_BlobIndex': len(records) - 1, 'm_KeywordIndices': list(sp['m_KeywordIndices']),
                                  'm_ShaderRequirements': sp['m_ShaderRequirements'], 'm_GpuProgramType': TYPE_GLES3})
                    added_params.append(param_index)
                    done += 1
                group.extend(added)
                vsp['m_ParameterBlobIndices'][g].extend(added_params)
    data = blob(records)
    packed = lz4_literals(data)
    tree['platforms'].append(GLES3)
    tree['offsets'].append([len(tree['compressedBlob'])])
    tree['compressedLengths'].append([len(packed)])
    tree['decompressedLengths'].append([len(data)])
    tree['stageCounts'].append(1)
    tree['compressedBlob'].extend(packed)
    return done, failed


def main():
    src, dst, xlat = sys.argv[1], sys.argv[2], sys.argv[3]
    replace = '--replace' in sys.argv
    env = UnityPy.load(src)
    for obj in env.objects:
        if obj.type.name != 'Shader':
            continue
        tree = obj.read_typetree()
        name = tree['m_ParsedForm']['m_Name']
        if D3D11 not in tree['platforms']:
            continue
        if GLES3 in tree['platforms']:
            if not replace:
                continue
            drop_platform(tree, GLES3)
        done, failed = add_gles3(tree, xlat, tree['m_ParsedForm']['m_KeywordNames'])
        obj.save_typetree(tree)
        print(f'{name}: {done} GLES3 programs added, {failed} not translated')
    open(dst, 'wb').write(env.file.save())


if __name__ == '__main__':
    main()
