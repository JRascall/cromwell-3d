"""WiC mesh (.mrb) -> Wavefront OBJ.

THE FORMAT, and how it was found. There is no public documentation and no
existing reader for `.mrb`; this was reversed from the shipped files. It is not
a tagged-chunk format -- the four-letter codes in it (`STND`, `WALK`, `CRCH`,
`FALL`, `WRCK`) are animation-state and variant tags, not chunk headers. The
file is a linear serialisation:

    'MRB' + u8 version          (14 and 17 both occur)
    f32 cull/LOD distance       (1500.0 on units, ~11-18 on props)
    f32[3]                      bounds-ish
    cstring node name           null-terminated, then the constant 0x58635FA9
    f32[16]                     4x4 transform, identity at the root
    ... node/state records ...

and somewhere inside it, one or more MESH BLOCKS of exactly this shape:

    u32  vertexCount
    vertexCount * 36 bytes      pos f32[3], normal f32[3], colour u8[4], uv f32[2]
    u32  (zero)
    u32  indexCount
    indexCount * u16            triangle list

Rather than parse the node tree -- which is where the remaining unknowns are --
this SCANS for mesh blocks and validates each candidate by a property that
cannot occur by chance: **the normals must be unit length**. On the reference
file that test gives mean |n| = 1.0000 with std 0.0000 for the true block and
garbage for every other offset, which makes the detector effectively
false-positive free. UV range and index-vs-vertex-count bounds are checked too.

What this does NOT recover: material/texture assignment per submesh (the
strings are in the file but their binding to mesh blocks is in the node tree),
skinning weights, and animation. Static geometry with normals and UVs comes out
correct, which is what the models are wanted for.

    py -3 wic_mrb.py <raw-dir> <out-dir>       sweep to .obj
    py -3 wic_mrb.py --info <file.mrb>         report blocks found
"""
import os, sys, glob, struct, collections
import numpy as np

STRIDE = 36
MAGIC = b'MRB'


def _unit_normal_mask(d, phase):
    """|v| == 1 for the float3 starting at every 4-byte step from `phase`.

    PHASE MATTERS AND IT COST HOURS. Mesh blocks are NOT 4-byte aligned in this
    format -- one unit buffer starts at 0x000cca, which is even but not a
    multiple of four. A float view taken from offset 0 can never see those
    fields, so a scan that only considers 4-aligned words concludes the file has
    no float normals at all and the geometry looks packed or encrypted. It is
    not: it is ordinary float32, two bytes out of step. All four phases get
    scanned.

    Vectorised: testing each byte offset in a Python loop is ~1.4M iterations
    for one unit mesh and made the sweep unusable.
    """
    body = d[phase:]
    with np.errstate(over='ignore', invalid='ignore'):
        a = np.frombuffer(body[:len(body) // 4 * 4], '<f4').astype(np.float64)
    if len(a) < 4:
        return np.zeros(0, bool), a
    with np.errstate(over='ignore', invalid='ignore'):
        ln = np.sqrt(a[:-2] ** 2 + a[1:-1] ** 2 + a[2:] ** 2)
    return np.isfinite(ln) & (np.abs(ln - 1.0) < 0.02), a


# There is no single vertex record. The engine builds a D3D9 vertex declaration
# per material from the pieces its shaders ask for -- the VSINPUT recovered from
# wic.exe is
#     float4 myPos : POSITION;  float4 myWeights : BLENDWEIGHT;
#     float4 myBoneIndices : BLENDINDICES;  float3 myNormal : NORMAL;
#     float4 myDiffuse : COLOR0;  float4 myTanspace : COLOR1;
#     float%d myTexcoord%d : TEXCOORD%d;
# with the trailing texcoords variable in count AND width, so strides of 32, 36,
# 40, 44, 48, 52, 56 and 60 all occur. Two confirmed by hand:
#     36: pos f32[3] normal f32[3] colour u8[4] uv f32[2]
#     60: pos f32[4] skinning 12B  normal f32[3] tanspace u8[4] uv f32[2] uv2 f32[2]
# Rather than enumerate the rest, the normal offset is DISCOVERED per block (it
# is whichever float3 is unit length) and the UV pair is found by range. The
# confirmation -- a u32 vertex count immediately before the array plus an index
# buffer entirely inside it -- is what keeps the wider search honest.
STRIDES = (32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72)


def _read_block(d, vstart, nv, stride, noff):
    """Decode one candidate vertex array, or None if it does not hold up.

    The UV offset is not supplied: it is chosen as the last float2 in the record
    that is finite and in a texture-coordinate range, which is where the
    texcoords sit in every layout seen (they are declared last in VSINPUT).
    """
    n = len(d)
    vend = vstart + nv * stride
    if vend + 8 > n:
        return None
    rec = np.frombuffer(d, np.uint8, count=nv * stride, offset=vstart).reshape(nv, stride)

    def fN(off, k):
        return rec[:, off:off + 4 * k].copy().view('<f4').astype(np.float64)

    with np.errstate(over='ignore', invalid='ignore'):
        pos, nrm = fN(0, 3), fN(noff, 3)
        if not (np.isfinite(pos).all() and np.isfinite(nrm).all()):
            return None
        ln = np.sqrt((nrm ** 2).sum(1))
    if not (abs(ln.mean() - 1.0) < 0.02 and ln.std() < 0.03):
        return None
    uv = None
    with np.errstate(over='ignore', invalid='ignore'):
        for off in range(stride - 8, noff + 8, -4):
            c = fN(off, 2)
            if np.isfinite(c).all() and np.abs(c).max() <= 64 and c.std() > 1e-4:
                uv = c
                break
    if uv is None:
        uv = np.zeros((nv, 2))
    return pos, nrm, uv, vend


def find_meshes(d, min_verts=3, max_verts=400000, probe=12):
    """Every validated mesh block in the buffer.

    Candidates come from runs of `probe` consecutive vertices whose normals are
    unit length, tried at all four byte phases and every known stride. Each is
    confirmed against the u32 vertex count that must sit immediately before the
    array, and against an index buffer whose values all fall inside it.
    """
    n = len(d)
    found = {}
    for phase in range(4):
        M, _a = _unit_normal_mask(d, phase)
        if len(M) < 64:
            continue
        for stride in STRIDES:
            s4 = stride // 4
            span = s4 * (probe - 1)
            if len(M) <= span + 4:
                continue
            run = np.ones(len(M) - span, bool)
            for k in range(probe):
                run &= M[k * s4: k * s4 + len(run)]
            if not run.any():
                continue
            starts = np.flatnonzero(run) * 4 + phase
            for noff in range(4, stride - 12, 4):
                # a hit is the NORMAL of some vertex; step back to record start
                for hit in (starts - noff).tolist():
                    if hit < 4 or hit in found:
                        continue
                    nv = int.from_bytes(d[hit - 4:hit], 'little')
                    if not (min_verts <= nv <= max_verts):
                        continue
                    got = _read_block(d, hit, nv, stride, noff)
                    if got is None:
                        continue
                    pos, nrm, uv, vend = got
                    ni = int.from_bytes(d[vend + 4:vend + 8], 'little')
                    if ni == 0 or ni % 3 or vend + 8 + ni * 2 > n or ni > 36 * nv:
                        continue
                    idx = np.frombuffer(d, np.uint8, count=ni * 2,
                                        offset=vend + 8).copy().view('<u2')
                    if idx.max() >= nv:
                        continue
                    found[hit] = dict(offset=hit - 4, verts=nv, tris=ni // 3,
                                      stride=stride, pos=pos, nrm=nrm,
                                      uv=uv, idx=idx)
    # drop blocks whose vertex array is contained in a larger one
    out = []
    for k in sorted(found):
        m = found[k]
        s, e = k, k + m['verts'] * m['stride']
        if any(o['offset'] + 4 <= s and e <= o['offset'] + 4 + o['verts'] * o['stride']
               and o is not m for o in found.values()):
            continue
        out.append(m)
    return out


def write_obj(meshes, path, name):
    base = 1
    with open(path, 'w') as fh:
        fh.write('# %s -- recovered from World in Conflict .mrb by tools/wic/wic_mrb.py\n' % name)
        fh.write('# %d mesh block(s)\n' % len(meshes))
        for k, m in enumerate(meshes):
            fh.write('\ng %s_mesh%02d\n' % (name, k))
            for p in m['pos']:
                fh.write('v %.6f %.6f %.6f\n' % (p[0], p[1], p[2]))
            for t in m['uv']:
                fh.write('vt %.6f %.6f\n' % (t[0], 1.0 - t[1]))
            for q in m['nrm']:
                fh.write('vn %.6f %.6f %.6f\n' % (q[0], q[1], q[2]))
            # int64 before the offset: indices are u16 per block, but `base`
            # accumulates across blocks and a model with several LODs passes
            # 65535 easily -- adding into the u16 dtype overflows (numpy 2
            # raises rather than wrapping, which is how these 4 files showed up).
            f = m['idx'].reshape(-1, 3).astype(np.int64) + base
            for a, b, c in f:
                fh.write('f %d/%d/%d %d/%d/%d %d/%d/%d\n' % (a, a, a, b, b, b, c, c, c))
            base += m['verts']


def convert(src, dst):
    d = open(src, 'rb').read()
    if d[:3] != MAGIC:
        return None
    meshes = find_meshes(d)
    if not meshes:
        return None
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    write_obj(meshes, dst, os.path.splitext(os.path.basename(src))[0])
    return meshes


def main(raw_dir, out_dir):
    files = glob.glob(os.path.join(raw_dir, '**', '*.mrb'), recursive=True)
    print('%d .mrb files' % len(files))
    ok = empty = bad = 0
    tv = tt = 0
    blocks = collections.Counter()
    fails = []
    for i, p in enumerate(files):
        rel = os.path.relpath(p, raw_dir)
        dst = os.path.join(out_dir, rel)[:-4] + '.obj'
        try:
            m = convert(p, dst)
        except Exception as e:
            bad += 1
            fails.append('%s: %s: %s' % (rel, type(e).__name__, e))
            continue
        if m is None:
            empty += 1
            fails.append('%s: no mesh block found' % rel)
        else:
            ok += 1
            blocks[len(m)] += 1
            tv += sum(x['verts'] for x in m)
            tt += sum(x['tris'] for x in m)
        if (i + 1) % 500 == 0:
            print('  %d/%d  (%d ok, %d empty, %d error)' % (i + 1, len(files), ok, empty, bad))
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, '_failures.txt'), 'w') as fh:
        fh.write('\n'.join(fails))
    print('\nconverted %d, no-mesh %d, error %d' % (ok, empty, bad))
    print('total %s verts, %s tris' % (f'{tv:,}', f'{tt:,}'))
    print('mesh blocks per file:', sorted(blocks.items())[:12])


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--info':
        d = open(sys.argv[2], 'rb').read()
        print('version', d[3], 'size', len(d))
        for m in find_meshes(d):
            p = m['pos']
            print('  @0x%06x  %6d verts %6d tris  bbox x[%.2f %.2f] y[%.2f %.2f] z[%.2f %.2f]'
                  % (m['offset'], m['verts'], m['tris'],
                     p[:, 0].min(), p[:, 0].max(), p[:, 1].min(), p[:, 1].max(),
                     p[:, 2].min(), p[:, 2].max()))
    elif len(sys.argv) == 3:
        main(sys.argv[1], sys.argv[2])
    else:
        sys.exit(__doc__)
