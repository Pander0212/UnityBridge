# SPDX-License-Identifier: GPL-3.0-or-later
# M4: reads a Shader's compiled programs from a Unity 2021.3 serialized file, per platform: the
# subprogram records (code) and the parameter records of its blob. Data only, no game code runs.
# Format notes in docs/STATUS.md (M4a); the parameter record layout follows USCSandbox (GPL-3.0,
# github.com/nesrak1/USCSandbox, Processor/ShaderParams.cs and friends).
# Usage: python3 -I shader_blob.py <serialized file> <shader name> [platform]   (4 = D3D11, 9 = GLES3x)
import struct, sys
import UnityPy
from UnityPy.enums import ShaderGpuProgramType
from UnityPy.helpers import CompressionHelper


class Reader:
    def __init__(self, b, pos=0):
        self.b, self.p = b, pos

    def i32(self):
        v = struct.unpack_from('<i', self.b, self.p)[0]
        self.p += 4
        return v

    def u32(self):
        v = struct.unpack_from('<I', self.b, self.p)[0]
        self.p += 4
        return v

    def align(self):
        self.p = (self.p + 3) & ~3

    def string(self):
        n = self.i32()
        s = self.b[self.p:self.p + n].decode('utf8', 'replace')
        self.p += n
        self.align()
        return s

    def bytes(self):
        n = self.i32()
        v = self.b[self.p:self.p + n]
        self.p += n
        self.align()
        return v


def platform_blob(sh, platform):
    i = list(sh.platforms).index(platform)
    off, cl, dl = sh.offsets[i][0], sh.compressedLengths[i][0], sh.decompressedLengths[i][0]
    return CompressionHelper.decompress_lz4(bytes(sh.compressedBlob)[off:off + cl], dl)


def blob_entries(b):  # (offset, length, segment) per record
    n = struct.unpack_from('<i', b, 0)[0]
    return [struct.unpack_from('<iii', b, 4 + 12 * k) for k in range(n)]


def read_code(r):  # a subprogram record (version 202012090, Unity 2021.2+)
    version, kind = r.i32(), r.i32()
    stats = [r.i32() for _ in range(4)]  # ALU, TEX, flow, temp registers
    keywords = [r.string() for _ in range(r.i32())]
    code = r.bytes()
    source_map, channels = r.i32(), r.i32()
    binds = [(r.i32(), r.i32()) for _ in range(channels)]
    return dict(version=version, type=kind, stats=stats, keywords=keywords, code=code, sourceMap=source_map, binds=binds)


def read_cbuffer(r):
    name, size, params = r.string(), r.i32(), []
    for _ in range(r.i32()):
        params.append(dict(name=r.string(), type=r.i32(), rows=r.i32(), columns=r.i32(), matrix=r.i32() > 0,
                           arraySize=r.i32(), offset=r.i32()))
    structs = r.i32()
    assert structs == 0, 'struct parameters not handled yet'
    return dict(name=name, size=size, params=params)


def read_params(r):  # a parameter record: a base cbuffer, cbuffers, then typed bindings
    version, count = r.i32(), r.i32()
    cbuffers = [read_cbuffer(r) for _ in range(count)]  # [0] is the base (unnamed) one
    bindings = []
    for _ in range(r.i32()):
        name, kind = r.string(), r.i32()
        if kind == 0:
            index, sampler, extra = r.i32(), r.i32(), r.u32()
            bindings.append(dict(kind='texture', name=name, index=index, sampler=sampler, dim=extra >> 1, msaa=extra & 1))
        elif kind in (1, 2):
            bindings.append(dict(kind=('cbuffer', 'buffer')[kind - 1], name=name, index=r.i32(), arraySize=r.i32()))
        elif kind == 3:
            bindings.append(dict(kind='uav', name=name, index=r.i32(), original=r.i32()))
        elif kind == 4:
            bindings.append(dict(kind='sampler', name=name, bind=r.i32(), sampler=r.u32()))
        else:
            raise ValueError('binding type %d' % kind)
    return dict(version=version, cbuffers=cbuffers, bindings=bindings)


# Which platform's blob a subprogram's m_BlobIndex refers to, by its GPU program type: a pass's groups
# list every platform's subprograms one after another.
def platform_of(gpu_type):
    name = ShaderGpuProgramType(gpu_type).name
    for key, platform in (('DX11', 4), ('DX10Level9', 4), ('GLES3', 9), ('GLES31', 9), ('GLES', 5), ('Metal', 14),
                          ('GLCore', 15), ('SPIRV', 18)):
        if name.startswith('kShaderGpuProgram' + key):
            return platform
    return None


def find_shader(path, name):
    for obj in UnityPy.load(path).objects:
        if obj.type.name == 'Shader':
            sh = obj.read()
            if sh.m_ParsedForm.m_Name == name:
                return sh


def main():
    sh = find_shader(sys.argv[1], sys.argv[2])
    only = int(sys.argv[3]) if len(sys.argv) > 3 else None
    plats = list(sh.platforms)
    print('platforms', plats)
    for platform in plats:
        if only is not None and platform != only:
            continue
        b = platform_blob(sh, platform)
        print('== platform', platform, 'records', len(blob_entries(b)))
        for p_i, p in enumerate(sh.m_ParsedForm.m_SubShaders[0].m_Passes):
            for stage in ('progVertex', 'progFragment'):
                prog = getattr(p, stage)
                for group, pblobs in zip(prog.m_PlayerSubPrograms, prog.m_ParameterBlobIndices):
                    for sp, pb in zip(group, pblobs):
                        if platform_of(sp.m_GpuProgramType) != platform:
                            continue  # another platform's entry in the same group
                        tname = ShaderGpuProgramType(sp.m_GpuProgramType).name
                        o, l, _ = blob_entries(b)[sp.m_BlobIndex]
                        code = read_code(Reader(b[o:o + l]))
                        po, pl, _ = blob_entries(b)[pb]
                        params = read_params(Reader(b[po:po + pl]))
                        print(f'pass{p_i} {stage} {tname} code#{sp.m_BlobIndex} params#{pb} kw={code["keywords"]} '
                              f'{len(code["code"])} bytes')
                        for cb in params['cbuffers']:
                            if cb['params']:
                                print(f'   cbuffer {cb["name"] or "(base)"} size {cb["size"]}: ' +
                                      ', '.join(f'{x["name"]}@{x["offset"]}' for x in cb['params']))
                        for bd in params['bindings']:
                            print('   ', bd)


if __name__ == '__main__':
    main()
