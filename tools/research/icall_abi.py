#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""M3 research: how hard is it to bridge Unity's InternalCalls between the two calling conventions?

In the hybrid, Android's libunity.so (ARM64, AAPCS64) registers its engine functions with
il2cpp_add_internal_call, and GameAssembly.dll (x86-64, Microsoft x64) calls them. Every such call
crosses from one convention to the other. Integer and pointer arguments map one to one; floats,
doubles and structs passed by value do not (Microsoft x64 gives each argument a positional slot and
passes structs that are not 1/2/4/8 bytes through a pointer; AAPCS64 counts integer and vector
registers separately, passes up to four floats of one type (HFA) in vector registers and structs up
to 16 bytes in two integer registers).

Reads only data: global-metadata.dat (method, parameter, type definitions) and the type table inside
GameAssembly.dll (Il2CppMetadataRegistration, found by its layout). Nothing is executed.

Usage: icall_abi.py GameAssembly.dll global-metadata.dat
"""
import collections
import struct
import sys

# ---- PE: sections and RVA <-> file offset -----------------------------------------------------

pe = open(sys.argv[1], 'rb').read()
e_lfanew = struct.unpack_from('<I', pe, 0x3c)[0]
nsec = struct.unpack_from('<H', pe, e_lfanew + 6)[0]
optsize = struct.unpack_from('<H', pe, e_lfanew + 20)[0]
image_base = struct.unpack_from('<Q', pe, e_lfanew + 24 + 24)[0]
sections = []
for i in range(nsec):
    o = e_lfanew + 24 + optsize + 40 * i
    name = pe[o:o + 8].rstrip(b'\0').decode()
    vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', pe, o + 8)
    sections.append((name, va, max(vsize, rawsize), rawptr, rawsize))


def off_of_va(va):
    rva = va - image_base
    for _, sva, size, raw, rawsize in sections:
        if sva <= rva < sva + size:
            return raw + (rva - sva) if rva - sva < rawsize else None
    return None


def q(va):
    o = off_of_va(va)
    return struct.unpack_from('<Q', pe, o)[0] if o is not None else None

# ---- metadata v29 -----------------------------------------------------------------------------

md = open(sys.argv[2], 'rb').read()
assert struct.unpack_from('<I', md, 0)[0] == 0xFAB11BAF
version = struct.unpack_from('<i', md, 4)[0]
hdr = struct.unpack_from('<64i', md, 8)
pair = lambda i: (hdr[2 * i], hdr[2 * i + 1])
s_off, _ = pair(2)
m_off, m_size = pair(5)
p_off, p_size = pair(10)
f_off, f_size = pair(11)
t_off, t_size = pair(19)


def cstr(i):
    e = md.index(b'\0', s_off + i)
    return md[s_off + i:e].decode('utf-8', 'replace')


TD = 88
typedefs = []
for k in range(t_size // TD):
    v = struct.unpack_from('<16i8HII', md, t_off + k * TD)
    typedefs.append(dict(name=cstr(v[0]), ns=cstr(v[1]), byval=v[2], parent=v[4], element=v[5], fieldStart=v[8],
                         field_count=v[16 + 2], bits=v[24]))


def tname(ti):
    t = typedefs[ti]
    return (t['ns'] + '.' if t['ns'] else '') + t['name']


fields = [struct.unpack_from('<iiI', md, f_off + k * 12) for k in range(f_size // 12)]  # name, type, token
params = [struct.unpack_from('<iIi', md, p_off + k * 12) for k in range(p_size // 12)]  # name, token, type

# ---- Il2CppMetadataRegistration: [.., fieldOffsetsCount, fieldOffsets, typeDefinitionsSizesCount, sizes, ..]

ntd = len(typedefs)
reg = None
for name, sva, size, raw, rawsize in sections:
    if name not in ('.data', '.rdata'):
        continue
    for o in range(raw, raw + rawsize - 16 * 8, 8):
        a, _, c, _ = struct.unpack_from('<QQQQ', pe, o + 10 * 8)
        if a == ntd and c == ntd:
            cand = struct.unpack_from('<16Q', pe, o)
            if 0 < cand[6] < 10_000_000 and off_of_va(cand[7]) is not None and off_of_va(cand[13]) is not None:
                reg = cand
                break
    if reg:
        break
assert reg, 'Il2CppMetadataRegistration not found'
types_count, types_va, sizes_va = reg[6], reg[7], reg[13]

TYPE_NAMES = {1: 'void', 2: 'bool', 3: 'char', 4: 'i1', 5: 'u1', 6: 'i2', 7: 'u2', 8: 'i4', 9: 'u4', 10: 'i8',
              11: 'u8', 12: 'r4', 13: 'r8', 14: 'string', 15: 'ptr', 16: 'byref', 17: 'valuetype', 18: 'class',
              19: 'var', 20: 'array', 21: 'genericinst', 24: 'i', 25: 'u', 27: 'fnptr', 28: 'object',
              29: 'szarray', 30: 'mvar'}


def il2cpp_type(index):
    """(data, attrs, type, byref) of Il2CppType `index` (16 bytes: data pointer/index, then bitfield)."""
    ptr = q(types_va + 8 * index)
    o = off_of_va(ptr)
    data, bits = struct.unpack_from('<QI', pe, o)
    return data, bits & 0xffff, (bits >> 16) & 0xff, (bits >> 29) & 1


def instance_size(ti):
    o = off_of_va(q(sizes_va + 8 * ti))
    return struct.unpack_from('<I', pe, o)[0] - 16  # value types: minus the object header


def struct_shape(ti, depth=0):
    """List of leaf kinds ('f4', 'f8', 'int') of a value type's instance fields."""
    t = typedefs[ti]
    out = []
    for k in range(t['field_count']):
        _, ftype, _ = fields[t['fieldStart'] + k]
        data, attrs, kind, byref = il2cpp_type(ftype)
        if attrs & 0x10:  # FIELD_ATTRIBUTE_STATIC
            continue
        if kind == 12:
            out.append('f4')
        elif kind == 13:
            out.append('f8')
        elif kind == 17 and depth < 4:
            inner = int(data)
            if typedefs[inner]['bits'] & 2:  # enum
                out.append('int')
            else:
                out += struct_shape(inner, depth + 1)
        else:
            out.append('int')
    return out


def classify(type_index):
    """ABI class of one argument or return value: 'int', 'f4', 'f8', 'void', or ('struct', size, hfa)."""
    data, attrs, kind, byref = il2cpp_type(type_index)
    if byref:
        return 'int'
    if kind == 1:
        return 'void'
    if kind == 12:
        return 'f4'
    if kind == 13:
        return 'f8'
    if kind == 17:
        ti = int(data)
        if typedefs[ti]['bits'] & 2:  # enum: its underlying integer
            return 'int'
        shape = struct_shape(ti)
        size = instance_size(ti)
        hfa = len(shape) in (1, 2, 3, 4) and len(set(shape)) == 1 and shape[0] in ('f4', 'f8')
        return ('struct', size, hfa, tname(ti))
    if kind == 21:
        return 'generic'  # a generic value type or class instance: decided per instantiation
    return 'int'


def abi_differs(cls):
    """Whether one argument needs more than a register-to-register move between the conventions."""
    return cls not in ('int', 'void')


# ---- InternalCall methods ---------------------------------------------------------------------

# System.* and Mono.* InternalCalls are implemented inside GameAssembly.dll itself (the class
# library's runtime); only the engine's own cross between the two conventions.
def crosses(full):
    return not (full.startswith('System.') or full.startswith('Mono.'))


counts = collections.Counter()
engine_counts = collections.Counter()
shapes = collections.Counter()
struct_types = collections.Counter()
examples = collections.defaultdict(list)
total = 0
for k in range(m_size // 32):
    name_i, decl, ret, pstart, gc, token = struct.unpack_from('<iiiiiI', md, m_off + k * 32)
    flags, iflags, slot, pcount = struct.unpack_from('<HHHH', md, m_off + k * 32 + 24)
    if not iflags & 0x1000:
        continue
    total += 1
    args = [] if flags & 0x10 else ['int']  # METHOD_ATTRIBUTE_STATIC; instance methods get `this`
    args += [classify(params[pstart + i][2]) for i in range(pcount)]
    rcls = classify(ret)
    full = tname(decl) + '::' + cstr(name_i)
    kinds = set()
    for c in args + [rcls]:
        if isinstance(c, tuple):
            kinds.add('struct (HFA)' if c[2] else 'struct')
            struct_types[c[3]] += 1
        elif c in ('f4', 'f8'):
            kinds.add('float/double')
        elif c == 'generic':
            kinds.add('generic')
    if not kinds:
        cat = 'integer/pointer only (generic thunk)'
    else:
        cat = ' + '.join(sorted(kinds))
    if len(args) > 8:
        cat += ', more than 8 arguments'
    counts[cat] += 1
    if crosses(full):
        engine_counts[cat] += 1
    if len(examples[cat]) < 4:
        examples[cat].append(full)
    sig = ''.join('S' if isinstance(c, tuple) else {'int': 'i', 'f4': 'f', 'f8': 'd', 'generic': 'g'}[c] for c in args)
    r = rcls
    rsig = 'S' if isinstance(r, tuple) else {'int': 'i', 'f4': 'f', 'f8': 'd', 'void': 'v', 'generic': 'g'}[r]
    shapes[rsig + '(' + sig + ')'] += 1

print('metadata version', version, '-', len(typedefs), 'types; registration: types', types_count)
print('InternalCall methods:', total)
for cat, n in counts.most_common():
    print(f'  {n:5}  {cat}')
    for e in examples[cat]:
        print('           e.g.', e)
print('of them engine functions (cross to libunity):', sum(engine_counts.values()))
for cat, n in engine_counts.most_common():
    print(f'  {n:5}  {cat}')
nonint = sum(n for s, n in shapes.items() if set(s) - set('iv()'))
print('distinct signatures:', len(shapes), '- of them with float/struct/generic:',
      sum(1 for s in shapes if set(s) - set('iv()')), 'covering', nonint, 'methods')
print('most common signatures:', ', '.join(f'{s} {n}' for s, n in shapes.most_common(12)))
print('value types passed or returned by value:', ', '.join(f'{t} {n}' for t, n in struct_types.most_common(15)))
