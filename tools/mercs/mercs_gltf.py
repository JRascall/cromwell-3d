"""mercs_gltf.py - mesh + skeleton + skin + animation, as one .glb.

The OBJ writer in mercs_mesh.py cannot carry a rig: OBJ has no skeleton, no
per-vertex binding and no notion of time, so the meshes ship in bind pose and
everything the modl and anim chunks know about how they MOVE stays in the
chunks. This is the format that carries all of it at once - Blender, three.js,
cgltf and tinygltf all read it, so it serves a viewer and a prototype equally.

WHAT GOES IN

    nodes       one per JONT, with its local TRS exactly as authored. The
                hierarchy is rebuilt from each joint's parent name.
    skin        the same joints in JONT order, so a segment's per-vertex BONE
                byte indexes it directly once SKEL's FSBI is added. Inverse
                bind matrices are the inverse of each joint's world transform,
                which is what makes bind-pose vertices land where they already
                are.
    mesh        one primitive per segment, with JOINTS_0 / WEIGHTS_0. Weights
                are all 1.0: the binding is rigid, one bone per vertex, and
                inventing smooth weights here would be a guess dressed as data.
    animations  every clip whose joints the skeleton has, sampled per frame.

TWO SEGMENT KINDS, AND WHY BOTH END UP IN MODEL SPACE

A segment binds to the rig one of two ways, and the difference is not cosmetic:

  * BNAM - the whole segment rides one joint and its vertices are in that
    joint's LOCAL space. mercs_mesh already moves these into model space to
    write an OBJ, and the same transform runs here.
  * BONE - one byte per vertex, and the vertices are already in model space.

Either way the vertices reaching glTF are in bind pose, so the inverse bind
matrix undoes the joint's world transform and the skin evaluates to the bind
pose at rest. Mixing the two conventions up puts limbs at the origin.

UVs. The OBJ writer emits `1 - v` because OBJ measures V up from the bottom.
glTF measures it down from the top, like the source data, so it goes out
unflipped. Getting this wrong mirrors every texture vertically, which on a
symmetric character texture is easy to miss.

TEXTURES are referenced by relative URI rather than embedded - `../textures_png`
by default. A .glb with 4,643 PNGs inlined would be enormous and would duplicate
what mercs_tex.py already wrote; a viewer opens the .glb from its own directory
and finds them.

FACING. The data is Y-up and the characters face +Z (bone_l_* sits at -X). glTF
is also Y-up and nominally -Z forward, so a model imports upright but looking
away from the conventional front. That is a convention difference, not a
handedness bug - nothing is mirrored - and it is left alone rather than
silently rotated, because the world and scene chunks place models in this same
frame.

Read-only research - see the header of mercs_dsk.py.
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mercs_mesh as MESH                          # noqa: E402
import mercs_anim as ANIM                          # noqa: E402
from mercs_lzss import decompress, STORED          # noqa: E402

FLOAT, USHORT, UINT, UBYTE = 5126, 5123, 5125, 5121
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963


def normalize(q):
    """glTF requires node rotations and rotation samplers to be UNIT
    quaternions, and neither source is quite unit: a JONT's quaternion is
    whatever the exporter wrote, and a decoded curve carries the 1/2047
    quantiser's noise. Both land within a few thousandths, which is harmless to
    look at and still a validation error, so they are normalised on the way out.

    This is the one place the glTF deliberately differs from the game's bytes.
    mercs_anim.py's .anim keeps the raw values - it is meant to be the data,
    not an interchange format.
    """
    n = (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) ** 0.5
    if n < 1e-9:
        return (0.0, 0.0, 0.0, 1.0)
    return (q[0] / n, q[1] / n, q[2] / n, q[3] / n)


def quat_to_mat(q):
    x, y, z, w = q
    return ((1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)),
            (2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)),
            (2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)))


def inverse_bind(R, t):
    """Column-major 4x4 inverse of a rotation+translation, as glTF wants it.

    The inverse of [R|t] is [R^T | -R^T t]; no scale is ever present in a JONT,
    so there is nothing else to undo.
    """
    ri = tuple(tuple(R[j][i] for j in range(3)) for i in range(3))
    ti = tuple(-sum(ri[i][k] * t[k] for k in range(3)) for i in range(3))
    return [ri[0][0], ri[1][0], ri[2][0], 0.0,
            ri[0][1], ri[1][1], ri[2][1], 0.0,
            ri[0][2], ri[1][2], ri[2][2], 0.0,
            ti[0], ti[1], ti[2], 1.0]


class Gltf:
    """Just enough glTF 2.0 to hold a rigged, animated model."""

    def __init__(self):
        self.bin = bytearray()
        self.j = {'asset': {'version': '2.0',
                            'generator': 'mercs_gltf.py - Mercenaries (PS2) study extract'},
                  'scene': 0, 'scenes': [{'nodes': []}], 'nodes': [],
                  'meshes': [], 'accessors': [], 'bufferViews': [],
                  'materials': [], 'textures': [], 'images': [], 'samplers': [],
                  'skins': [], 'animations': []}

    def view(self, raw, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        off = len(self.bin)
        self.bin += raw
        v = {'buffer': 0, 'byteOffset': off, 'byteLength': len(raw)}
        if target:
            v['target'] = target
        self.j['bufferViews'].append(v)
        return len(self.j['bufferViews']) - 1

    def accessor(self, view, comp, count, kind, mn=None, mx=None, norm=False):
        a = {'bufferView': view, 'componentType': comp, 'count': count, 'type': kind}
        if mn is not None:
            a['min'], a['max'] = mn, mx
        if norm:
            a['normalized'] = True
        self.j['accessors'].append(a)
        return len(self.j['accessors']) - 1

    def floats(self, rows, kind, target=ARRAY_BUFFER, minmax=True):
        n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[kind]
        flat = [c for r in rows for c in r] if n > 1 else list(rows)
        raw = struct.pack('<%df' % len(flat), *flat)
        v = self.view(raw, target)
        mn = mx = None
        if minmax and rows:
            if n == 1:
                mn, mx = [min(flat)], [max(flat)]
            else:
                mn = [min(r[i] for r in rows) for i in range(n)]
                mx = [max(r[i] for r in rows) for i in range(n)]
        return self.accessor(v, FLOAT, len(rows) if n > 1 else len(flat), kind, mn, mx)

    def glb(self, path):
        self.j['buffers'] = [{'byteLength': len(self.bin)}]
        for key in [k for k, v in self.j.items() if isinstance(v, list) and not v]:
            del self.j[key]
        js = json.dumps(self.j, separators=(',', ':')).encode('utf-8')
        js += b' ' * (-len(js) % 4)
        bn = bytes(self.bin) + b'\0' * (-len(self.bin) % 4)
        total = 12 + 8 + len(js) + 8 + len(bn)
        with open(path, 'wb') as f:
            f.write(b'glTF' + struct.pack('<II', 2, total))
            f.write(struct.pack('<I', len(js)) + b'JSON' + js)
            f.write(struct.pack('<I', len(bn)) + b'BIN\0' + bn)
        return total


def build(g, name, joints, fsbi, segs, tex_index, tex_rel, anims):
    """One model: joint nodes, skin, primitives, then any matching clips."""
    # ---- joint nodes, in JONT order so BONE + FSBI indexes them directly
    index_of = {}
    for i, (jn, q, t, parent) in enumerate(joints):
        index_of[jn.lower()] = i
        g.j['nodes'].append({'name': jn, 'translation': list(t),
                             'rotation': list(normalize(q))})
    roots = []
    for i, (jn, q, t, parent) in enumerate(joints):
        p = index_of.get(parent.lower())
        if p is not None and p != i:
            g.j['nodes'][p].setdefault('children', []).append(i)
        else:
            roots.append(i)

    world = {}

    def resolve(i, seen=()):
        if i in world:
            return world[i]
        jn, q, t, parent = joints[i]
        R, tt = quat_to_mat(normalize(q)), t
        p = index_of.get(parent.lower())
        if p is not None and p != i and p not in seen:
            PR, Pt = resolve(p, seen + (i,))
            R = tuple(tuple(sum(PR[r][k] * R[k][c] for k in range(3)) for c in range(3))
                      for r in range(3))
            tt = tuple(sum(PR[r][k] * t[k] for k in range(3)) + Pt[r] for r in range(3))
        world[i] = (R, tt)
        return world[i]

    ibm = [inverse_bind(*resolve(i)) for i in range(len(joints))]
    skin = None
    if joints:
        acc = g.floats(ibm, 'MAT4', target=None, minmax=False)
        g.j['skins'].append({'name': name + '_skin', 'joints': list(range(len(joints))),
                             'inverseBindMatrices': acc, 'skeleton': roots[0] if roots else 0})
        skin = 0

    # ---- materials, one per distinct texture, matching mercs_mesh's rule
    mat_of, prims = {}, []
    for s in segs:
        tex = s['texture']
        if tex and tex.lower() not in mat_of:
            png = tex_index.get(tex.lower())
            mi = len(g.j['materials'])
            m = {'name': tex, 'pbrMetallicRoughness': {'metallicFactor': 0.0,
                                                       'roughnessFactor': 1.0}}
            if png:
                g.j['images'].append({'uri': '%s/%s' % (tex_rel, png)})
                g.j['samplers'].append({'wrapS': 10497, 'wrapT': 10497})
                g.j['textures'].append({'source': len(g.j['images']) - 1,
                                        'sampler': len(g.j['samplers']) - 1})
                m['pbrMetallicRoughness']['baseColorTexture'] = {
                    'index': len(g.j['textures']) - 1}
                # Alpha lives in the diffuse for the _alpha textures; blending
                # rather than masking, because the source has soft edges on hair
                # and glasses.
                if 'alpha' in tex.lower():
                    m['alphaMode'] = 'BLEND'
                    m['doubleSided'] = True
            g.j['materials'].append(m)
            mat_of[tex.lower()] = mi

    for s in segs:
        if not s['tris']:
            continue
        pos = g.floats(s['pos'], 'VEC3')
        attrs = {'POSITION': pos}
        if s['nrm']:
            attrs['NORMAL'] = g.floats(s['nrm'], 'VEC3', minmax=False)
        if s['uv']:
            # Unflipped - see the note in the module docstring.
            attrs['TEXCOORD_0'] = g.floats(s['uv'], 'VEC2', minmax=False)
        if skin is not None:
            jj = s['joint_index']
            raw = b''.join(struct.pack('<4H', j, 0, 0, 0) for j in jj)
            attrs['JOINTS_0'] = g.accessor(g.view(raw, ARRAY_BUFFER), USHORT,
                                           len(jj), 'VEC4')
            raw = struct.pack('<%df' % (len(jj) * 4),
                              *[c for _ in jj for c in (1.0, 0.0, 0.0, 0.0)])
            attrs['WEIGHTS_0'] = g.accessor(g.view(raw, ARRAY_BUFFER), FLOAT,
                                            len(jj), 'VEC4')
        idx = [i for t in s['tris'] for i in t]
        iv = g.view(struct.pack('<%dI' % len(idx), *idx), ELEMENT_ARRAY_BUFFER)
        p = {'attributes': attrs, 'indices': g.accessor(iv, UINT, len(idx), 'SCALAR')}
        if s['texture'] and s['texture'].lower() in mat_of:
            p['material'] = mat_of[s['texture'].lower()]
        prims.append(p)

    g.j['meshes'].append({'name': name, 'primitives': prims})
    mesh_node = {'name': name, 'mesh': 0}
    if skin is not None:
        mesh_node['skin'] = skin
    g.j['nodes'].append(mesh_node)
    g.j['scenes'][0]['nodes'] = roots + [len(g.j['nodes']) - 1]

    # ---- animations
    attached, dropped = 0, 0
    for a in anims:
        times = [f / ANIM.FPS for f in range(a['frames'])]
        tin = g.floats(times, 'SCALAR', target=None)
        samplers, channels = [], []
        for j in a['joints']:
            node = index_of.get(j['name'].lower())
            if node is None:
                dropped += 1
                continue
            out = g.floats([normalize(q) for q in j['quat']], 'VEC4',
                           target=None, minmax=False)
            samplers.append({'input': tin, 'output': out, 'interpolation': 'LINEAR'})
            channels.append({'sampler': len(samplers) - 1,
                             'target': {'node': node, 'path': 'rotation'}})
            # A static joint's translation is its bind translation; the node
            # already holds it, so a channel repeating it every frame would be
            # pure weight. Only animated ones get a track.
            if j['dynamic']:
                out = g.floats(j['trans'], 'VEC3', target=None, minmax=False)
                samplers.append({'input': tin, 'output': out, 'interpolation': 'LINEAR'})
                channels.append({'sampler': len(samplers) - 1,
                                 'target': {'node': node, 'path': 'translation'}})
        if channels:
            g.j['animations'].append({'name': a['name'], 'samplers': samplers,
                                      'channels': channels})
            attached += 1
    return attached, dropped


def load_segments(blob_bytes, box, world, fsbi, njoints, index_of):
    """Segments with a per-vertex joint index attached, in model space."""
    dec, comp = struct.unpack_from('<II', blob_bytes, 16)
    body = blob_bytes[24:]
    inner = body[:dec] if comp == STORED else decompress(body[:comp], dec)
    top = MESH.children(inner)
    root = top[0][1] if top and top[0][0] == 'ucfb' else inner

    out = []
    for tag, buf in MESH.children(root):
        if tag != 'segm':
            continue
        seg = MESH.parse_segm(buf, box)
        if not seg or not seg['tris']:
            continue
        d = dict(MESH.children(buf))
        nv = len(seg['pos'])
        if 'BONE' in d and len(d['BONE']) >= nv:
            # Already model space; the byte indexes the joint list past FSBI.
            seg['joint_index'] = [min(njoints - 1, b + fsbi) for b in d['BONE'][:nv]]
        else:
            seg = MESH.place_segment(seg, world)
            ji = index_of.get(seg['bone'].lower(), 0)
            seg['joint_index'] = [ji] * nv
        seg['nrm'] = [(a / 127.0, b / 127.0, c / 127.0) for a, b, c in seg['nrm']]
        out.append(seg)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dsk', nargs='+')
    ap.add_argument('--out', required=True)
    ap.add_argument('--model', action='append', default=[],
                    help='substring of a model name; repeatable. Default: all')
    ap.add_argument('--anims', default=None,
                    help='substring selecting clips to attach, or "all". '
                         'A clip is only attached if the skeleton has every '
                         'joint it animates. Default: rig only, no clips')
    ap.add_argument('--tex-rel', default='../textures_png')
    ap.add_argument('--bind-threshold', type=float, default=0.75,
                    help='fraction of a clip\'s joints the skeleton must have '
                         'before the clip is attached (default 0.75)')
    ap.add_argument('--limit', type=int, default=0)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    models, cseg_loc, tex_index, anim_blobs = {}, {}, {}, []
    for path in args.dsk:
        data = open(path, 'rb').read()
        count = struct.unpack_from('<I', data, 0)[0]
        ents = [struct.unpack_from('<III', data, 8 + i * 12) for i in range(count)]
        off = 8 + count * 12
        for size, h, grp in ents:
            start, blob = off, data[off:off + size]
            off += size
            tag = blob[8:12]
            if tag == b'CSEG':
                cseg_loc[h] = (path, start, size)
                continue
            if tag not in (b'modl', b'tex_', b'anim'):
                continue
            inner = blob[16:16 + struct.unpack_from('<I', blob, 12)[0]]
            nm = ''
            for k, v in MESH.children(inner):
                if k == 'NAME':
                    nm = MESH.cstr(v)
                    break
            if tag == b'tex_':
                safe = ''.join(c if c.isalnum() or c in '_-' else '_' for c in nm)
                tex_index[nm.lower()] = '%s_%08X.png' % (safe[:60] or 'tex', h)
            elif tag == b'modl':
                models[h] = {'name': nm, 'box': MESH.model_box(inner),
                             'joints': MESH.parse_joints(inner),
                             'fsbi': MESH.skel_fsbi(inner),
                             'world': MESH.parse_skeleton(inner)}
            elif tag == b'anim':
                anim_blobs.append(inner)

    clips = []
    if args.anims:
        want = None if args.anims.lower() == 'all' else args.anims.lower()
        for inner in anim_blobs:
            nm = ''
            for k, v in MESH.children(inner):
                if k == 'NAME':
                    nm = MESH.cstr(v)
                    break
            if want and want not in nm.lower():
                continue
            try:
                a = ANIM.decode(inner)
            except Exception:
                a = None
            if a:
                clips.append(a)
        print('  %d clips selected' % len(clips))

    written = 0
    for h, m in sorted(models.items(), key=lambda kv: kv[1]['name']):
        if args.model and not any(s.lower() in m['name'].lower() for s in args.model):
            continue
        if h not in cseg_loc or not m['box']:
            continue
        if args.limit and written >= args.limit:
            break
        path, start, size = cseg_loc[h]
        with open(path, 'rb') as f:
            f.seek(start)
            blob = f.read(size)
        joints = m['joints']
        index_of = {j[0].lower(): i for i, j in enumerate(joints)}
        try:
            segs = load_segments(blob, m['box'], m['world'], m['fsbi'],
                                 max(1, len(joints)), index_of)
        except Exception:
            continue
        if not segs:
            continue

        # A clip binds by joint NAME, the way the engine does, so a clip that
        # names one joint this skeleton lacks still plays - `grounddummy` and
        # `bone_lower_eyelids` are in the animators' rig and not in the shipped
        # model, and demanding an exact match threw away every roll and
        # charge-disarm animation the American merc has. The threshold only
        # exists to keep a vehicle's clips off a character: an unrelated
        # skeleton overlaps by a joint or two, never by most of them.
        names = set(index_of)
        mine = []
        for a in clips:
            if not a['joints']:
                continue
            hit = sum(1 for j in a['joints'] if j['name'].lower() in names)
            if hit >= max(1, int(len(a['joints']) * args.bind_threshold)):
                mine.append(a)
        g = Gltf()
        attached, dropped = build(g, m['name'], joints, m['fsbi'], segs, tex_index,
                                  args.tex_rel, mine)
        safe = ''.join(c if c.isalnum() or c in '_-' else '_' for c in m['name'])
        stem = '%s_%08X' % (safe[:60] or 'model', h)
        # A model exported twice - once as a bare rig, once carrying clips -
        # otherwise lands on two files with the SAME name in different
        # directories, and a browser that lists by name shows them as
        # indistinguishable rows. The one with animation says so.
        if attached:
            stem += '_anim'
        total = g.glb(os.path.join(args.out, stem + '.glb'))
        written += 1
        if clips:
            print('  %-42s %2d joints  %4d of %d clips  %5d unbound channels'
                  ' %6.1f MB' % (m['name'][:40], len(joints), attached, len(clips),
                                 dropped, total / 1048576.0))
    print('  %d models -> %s' % (written, args.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
