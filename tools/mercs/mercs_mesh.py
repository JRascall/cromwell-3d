"""mercs_mesh.py - turn Mercenaries' CSEG geometry into .obj files.

CSEG is one LZ-packed ucfb per model (see mercs_lzss.py for the compressor and
where it was read out of the PS2 executable). Inside are `segm` chunks, one per
material, and a `cshp` collision shape.

THE segm LAYOUT, and how the field sizes were settled rather than guessed:

    INFO  {u32 vertexCount; u32 indexCount; u32 flags}
    MTRL  material parameters
    MNAM  material name          "Material0"
    TNAM  texture name           the tex_ chunk this segment samples
    BNAM  bone name              which joint the segment rides on
    CLRB  4 bytes, a flat vertex colour
    STRP  indexCount * u16       triangle strips; 0x8000 marks a strip start
    POSI  vertexCount * 3 * s16  position
    NORM  vertexCount * 3 * s8   normal
    TEX0  vertexCount * 2 * s16  texture coordinate

    Two CSEGs on the disc are stored uncompressed, and they pin this down with
    no ambiguity: 22 vertices / 32 indices gives POSI 132, NORM 66, TEX0 88,
    STRP 64 - every buffer divides exactly, and a second sample at 17/25 gives
    102/51/68/50 on the same rule. A wrong field width does not divide twice.

TRIANGLE STRIPS. Indices carry 0x8000 on the FIRST TWO vertices of each strip,
which is how one buffer holds many strips. Winding alternates within a strip in
the usual way, and degenerate triangles (a repeated index) are dropped rather
than emitted - PS2 strips use them as joins.

POSITIONS ARE FIXED POINT. u16 with no scale of their own: they map linearly
onto the model's bounding box, which lives in the modl chunk and is in metres.
So the output is already in metres and --scale defaults to 1.0 - it exists only
for importers that want something else.

Read-only research - see the header of mercs_dsk.py.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mercs_lzss import decompress, STORED          # noqa: E402


def children(buf):
    out, p = [], 0
    while p + 8 <= len(buf):
        tag = buf[p:p + 4]
        size = struct.unpack_from('<I', buf, p + 4)[0]
        if p + 8 + size > len(buf):
            break
        out.append((tag.decode('ascii', 'replace'), buf[p + 8:p + 8 + size]))
        p = (p + 8 + size + 3) & ~3
    return out


def cstr(b):
    return b.split(b'\0')[0].decode('ascii', 'replace')


def parse_joints(modl_payload):
    """modl/SKEL as an ORDERED list of [name, quaternion xyzw, translation,
    parent name] - the hierarchy as authored, before anything is flattened.

    A JONT is: name\\0, quaternion xyzw (4 floats), translation (3 floats),
    parent name\\0. The Apache's root confirms the layout exactly - "Apache\\0"
    plus an identity quaternion plus a zero translation plus an empty parent is
    36 bytes, and hp_dock_rr is 11+16+12+7 = 46.

    Order matters to callers beyond this file: a segment's per-vertex BONE byte
    indexes THIS list (offset by SKEL's FSBI), and glTF wants the local TRS of
    each joint rather than the world transform parse_skeleton returns.
    """
    out = []
    for tag, buf in children(modl_payload):
        if tag != 'SKEL':
            continue
        for k, v in children(buf):
            if k != 'JONT':
                continue
            z = v.find(b'\0')
            if z < 0 or z + 29 > len(v):
                continue
            out.append((v[:z].decode('ascii', 'replace'),
                        struct.unpack_from('<4f', v, z + 1),
                        struct.unpack_from('<3f', v, z + 17),
                        v[z + 29:].split(b'\0')[0].decode('ascii', 'replace')))
    return out


def skel_fsbi(modl_payload):
    """SKEL/FSBI - the index the per-vertex BONE bytes are relative to.

    1 on every character checked, which is what skips DummyRoot. Read rather
    than assumed: a model whose first joint is real would need 0, and the
    difference is a whole-body offset that looks like a rigging error.
    """
    for tag, buf in children(modl_payload):
        if tag != 'SKEL':
            continue
        for k, v in children(buf):
            if k == 'FSBI' and len(v) >= 4:
                return struct.unpack_from('<I', v, 0)[0]
    return 0


def parse_skeleton(modl_payload):
    """name -> (rotation 3x3, translation) in MODEL space, from modl/SKEL.

    THIS IS NOT OPTIONAL DECORATION. Segment vertices are in BONE-LOCAL space,
    so a model whose parts sit on different joints - every vehicle: rotors,
    turrets, barrels, doors - collapses into a heap at the origin if the joint
    transforms are not applied. Single-bone models look fine without it, which
    is exactly why the omission was not obvious.
    """
    local, parent = {}, {}
    for name, q, t, p in parse_joints(modl_payload):
        local[name] = (q, t)
        parent[name] = p

    def rot(q):
        x, y, z_, w = q
        return ((1 - 2 * (y * y + z_ * z_), 2 * (x * y - z_ * w), 2 * (x * z_ + y * w)),
                (2 * (x * y + z_ * w), 1 - 2 * (x * x + z_ * z_), 2 * (y * z_ - x * w)),
                (2 * (x * z_ - y * w), 2 * (y * z_ + x * w), 1 - 2 * (x * x + y * y)))

    def mul(a, b):
        return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3))
                     for i in range(3))

    def apply(m, v):
        return tuple(sum(m[i][k] * v[k] for k in range(3)) for i in range(3))

    world, busy = {}, set()

    def resolve(name):
        if name in world:
            return world[name]
        if name not in local or name in busy:
            return (((1, 0, 0), (0, 1, 0), (0, 0, 1)), (0.0, 0.0, 0.0))
        busy.add(name)
        R, t = rot(local[name][0]), local[name][1]
        p = parent.get(name, '')
        if p and p in local:
            PR, Pt = resolve(p)
            R = mul(PR, R)
            t = tuple(apply(PR, t)[i] + Pt[i] for i in range(3))
        busy.discard(name)
        world[name] = (R, t)
        return world[name]

    for n in local:
        resolve(n)
    return world


def place_segment(seg, world):
    """Move a segment's vertices from bone space into model space."""
    xf = world.get(seg['bone'])
    if not xf:
        return seg
    R, t = xf
    # Positions are already in model units by the time this runs (parse_segm
    # applies the block-float exponent), so the joint translation goes in as it
    # is - it is in those same units.
    seg['pos'] = [(R[0][0] * x + R[0][1] * y + R[0][2] * z + t[0],
                   R[1][0] * x + R[1][1] * y + R[1][2] * z + t[1],
                   R[2][0] * x + R[2][1] * y + R[2][2] * z + t[2])
                  for x, y, z in seg['pos']]
    # Normals rotate but do not translate.
    seg['nrm'] = [(R[0][0] * a + R[0][1] * b + R[0][2] * c,
                   R[1][0] * a + R[1][1] * b + R[1][2] * c,
                   R[2][0] * a + R[2][1] * b + R[2][2] * c)
                  for a, b, c in seg['nrm']]
    return seg


def strips_to_tris(idx):
    """0x8000 on two consecutive indices starts a new strip."""
    strips, cur, i, n = [], [], 0, len(idx)
    while i < n:
        if (idx[i] & 0x8000) and i + 1 < n and (idx[i + 1] & 0x8000):
            if len(cur) >= 3:
                strips.append(cur)
            cur = [idx[i] & 0x7FFF, idx[i + 1] & 0x7FFF]
            i += 2
        else:
            cur.append(idx[i] & 0x7FFF)
            i += 1
    if len(cur) >= 3:
        strips.append(cur)

    tris = []
    for s in strips:
        for k in range(len(s) - 2):
            a, b, c = s[k], s[k + 1], s[k + 2]
            if a == b or b == c or a == c:
                continue                      # degenerate join
            tris.append((a, b, c) if k % 2 == 0 else (a, c, b))
    return tris


def model_box(modl_payload):
    """The model's vertex bounding box, from modl/INFO at offset 8.

    Six floats, min then max, and they read as real metres - the airport
    children's museum comes out 36 x 15 x 18, the hangar 49 x 17 x 61. A second
    identical box follows at offset 32 (the visibility box).
    """
    d = dict(children(modl_payload))
    info = d.get('INFO')
    if not info or len(info) < 32:
        return None
    v = struct.unpack_from('<6f', info, 8)
    return (v[0], v[1], v[2]), (v[3], v[4], v[5])


def parse_segm(payload, box=None):
    d = dict(children(payload))
    if 'INFO' not in d or 'POSI' not in d:
        return None
    nv, ni, topology = struct.unpack_from('<III', d['INFO'], 0)

    # POSITIONS ARE UNSIGNED u16, linearly mapped from [0, 65535] onto the
    # MODEL's bounding box. Not signed, and not scaled by any per-segment
    # exponent - INFO's third field is the primitive topology, not an exponent.
    #
    # Reading them as signed wraps every value above 32767 to a large negative,
    # which tears a model apart along no particular axis and is unrecoverable by
    # any amount of scaling afterwards. Without the box a segment cannot be
    # placed at all, which is why the box lives on the model and not the segment.
    raw = [struct.unpack_from('<HHH', d['POSI'], i * 6) for i in range(nv)]
    if box:
        mn, mx = box
        sx, sy, sz = (mx[0] - mn[0]) / 65535.0, (mx[1] - mn[1]) / 65535.0, \
                     (mx[2] - mn[2]) / 65535.0
        pos = [(mn[0] + x * sx, mn[1] + y * sy, mn[2] + z * sz) for x, y, z in raw]
    else:
        # No box: normalise to a unit cube so the shape is still readable.
        pos = [(x / 65535.0, y / 65535.0, z / 65535.0) for x, y, z in raw]

    # UVs are signed i16 over 2048, not 4096.
    uv = ([tuple(c / 2048.0 for c in struct.unpack_from('<hh', d['TEX0'], i * 4))
           for i in range(nv)]
          if len(d.get('TEX0', b'')) >= nv * 4 else [])
    nrm = ([struct.unpack_from('<bbb', d['NORM'], i * 3) for i in range(nv)]
           if len(d.get('NORM', b'')) >= nv * 3 else [])
    idx = list(struct.unpack_from('<%dH' % ni, d['STRP'], 0)) if 'STRP' in d else []
    return {
        'pos': pos, 'uv': uv, 'nrm': nrm, 'tris': strips_to_tris(idx),
        'material': cstr(d.get('MNAM', b'')), 'texture': cstr(d.get('TNAM', b'')),
        'bone': cstr(d.get('BNAM', b'')),
    }


def write_obj(path, segs, scale, name, tex_index=None, tex_rel='../../textures_png'):
    """Write the .obj and, beside it, the .mtl that binds each segment's texture.

    One material per DISTINCT texture rather than one per segment: a vehicle has
    dozens of segments sharing a handful of textures (the Apache is 55 segments
    over 8 textures), and the asset browser selects a texture per material index,
    so collapsing them keeps that mapping small and makes `usemtl` meaningful.

    tex_index maps a lowercased TNAM to the .png that mercs_tex.py wrote, whose
    filename carries the name hash. Without it the .mtl would name a texture that
    does not exist on disk.
    """
    tex_index = tex_index or {}
    # Distinct textures, in first-use order - that order IS the material index.
    order, seen = [], set()
    for s in segs:
        t = s['texture']
        if t and t.lower() not in seen:
            seen.add(t.lower())
            order.append(t)
    mtl_name = os.path.splitext(os.path.basename(path))[0] + '.mtl'

    if order:
        with open(os.path.join(os.path.dirname(path), mtl_name), 'w',
                  encoding='utf-8') as m:
            m.write('# %s - materials for the Mercenaries mesh beside this file\n' % name)
            for t in order:
                m.write('\nnewmtl %s\n' % t)
                m.write('Kd 1.000 1.000 1.000\n')
                png = tex_index.get(t.lower())
                if png:
                    m.write('map_Kd %s/%s\n' % (tex_rel, png))
                else:
                    m.write('# no decoded texture found for %s\n' % t)

    with open(path, 'w', encoding='utf-8') as f:
        f.write('# %s - Mercenaries: Playground of Destruction (PS2)\n' % name)
        f.write('# extracted for study by tools/mercs/mercs_mesh.py\n')
        f.write('# %d segment(s), %d material(s)\n' % (len(segs), len(order)))
        if order:
            f.write('mtllib %s\n' % mtl_name)
        f.write('\n')
        vbase = tbase = nbase = 1
        for si, s in enumerate(segs):
            f.write('g %s_%d%s\n' % (name, si,
                                     ('_' + s['bone']) if s['bone'] else ''))
            if s['texture']:
                f.write('usemtl %s\n' % s['texture'])
                f.write('# texture: %s   material: %s\n' % (s['texture'], s['material']))
            for x, y, z in s['pos']:
                f.write('v %.6f %.6f %.6f\n' % (x * scale, y * scale, z * scale))
            # Already divided by 2048 in parse_segm; only the V flip is left.
            for u, v in s['uv']:
                f.write('vt %.6f %.6f\n' % (u, 1.0 - v))
            for a, b, c in s['nrm']:
                f.write('vn %.4f %.4f %.4f\n' % (a / 127.0, b / 127.0, c / 127.0))
            # v, vt and vn are SEPARATE index spaces in .obj. Sharing one
            # counter works only while every segment has all three; the moment
            # one lacks UVs or normals, every later segment's vt/vn indices
            # point at the wrong rows.
            for a, b, c in s['tris']:
                if s['uv'] and s['nrm']:
                    f.write('f %d/%d/%d %d/%d/%d %d/%d/%d\n' % (
                        vbase + a, tbase + a, nbase + a,
                        vbase + b, tbase + b, nbase + b,
                        vbase + c, tbase + c, nbase + c))
                elif s['uv']:
                    f.write('f %d/%d %d/%d %d/%d\n' % (
                        vbase + a, tbase + a, vbase + b, tbase + b,
                        vbase + c, tbase + c))
                else:
                    f.write('f %d %d %d\n' % (vbase + a, vbase + b, vbase + c))
            vbase += len(s['pos'])
            tbase += len(s['uv'])
            nbase += len(s['nrm'])


def load_segments(h, cseg_loc, boxes, skeletons, cache):
    """Decompress and parse one model's segments, by name hash. Cached."""
    if h in cache:
        return cache[h]
    loc = cseg_loc.get(h)
    if not loc:
        cache[h] = []
        return []
    path, start, size = loc
    with open(path, 'rb') as f:
        f.seek(start)
        blob = f.read(size)
    dec, comp = struct.unpack_from('<II', blob, 16)
    body = blob[24:]
    try:
        inner = body[:dec] if comp == STORED else decompress(body[:comp], dec)
    except Exception:
        cache[h] = []
        return []
    box, world = boxes.get(h), skeletons.get(h, {})
    segs = []
    for k, v in children(inner[8:]):
        if k == 'segm':
            s = parse_segm(v, box)
            if s and s['tris']:
                segs.append(place_segment(s, world))
    cache[h] = segs
    return segs


def attach_wheels(name, world, segs, cseg_loc, boxes, skeletons, names,
                  name_to_hash, cache):
    """Instance <model>_wheel at every wheel_* joint the skeleton declares.

    Wheels are not part of a vehicle's mesh. The bus carries wheel_fl, wheel_fr,
    wheel_rl and wheel_rr joints but no wheel geometry, and a separate
    civ_veh_bus_wheel model is instanced at each - one 37 KB mesh drawn four
    times rather than four copies in the file. 40 models in the game are wheels
    on this pattern.

    So an exported vehicle is genuinely wheel-less, and looks it. This puts them
    back for viewing; the standalone wheel model is still exported on its own.
    """
    wh = name_to_hash.get((name + '_wheel').lower())
    if wh is None:
        return 0
    wheel_segs = load_segments(wh, cseg_loc, boxes, skeletons, cache)
    if not wheel_segs:
        return 0
    added = 0
    for joint, xf in sorted(world.items()):
        if not joint.lower().startswith('wheel'):
            continue
        R, t = xf
        for ws in wheel_segs:
            segs.append({
                'pos': [(R[0][0] * x + R[0][1] * y + R[0][2] * z + t[0],
                         R[1][0] * x + R[1][1] * y + R[1][2] * z + t[1],
                         R[2][0] * x + R[2][1] * y + R[2][2] * z + t[2])
                        for x, y, z in ws['pos']],
                'uv': list(ws['uv']),
                'nrm': [(R[0][0] * a + R[0][1] * b + R[0][2] * c,
                         R[1][0] * a + R[1][1] * b + R[1][2] * c,
                         R[2][0] * a + R[2][1] * b + R[2][2] * c)
                        for a, b, c in ws['nrm']],
                'tris': list(ws['tris']),
                'material': ws['material'], 'texture': ws['texture'],
                'bone': joint,
            })
            added += 1
    return added


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dsk', nargs='+')
    ap.add_argument('--out', required=True)
    # The block-float exponent already puts positions in the game's own units,
    # so this is only a viewing convenience and defaults to leaving them alone.
    ap.add_argument('--scale', type=float, default=1.0)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--tex-rel', default='../../textures_png',
                    help='where the .mtl should look for textures, relative to the .obj')
    ap.add_argument('--attach-wheels', action=argparse.BooleanOptionalAction,
                    default=True,
                    help='instance <model>_wheel at each wheel_* joint (default on)')
    args = ap.parse_args()
    catalog = []
    wheels = 0

    os.makedirs(args.out, exist_ok=True)
    names = {}
    tex_index = {}
    skeletons = {}
    boxes = {}
    cseg_loc = {}
    name_to_hash = {}
    seg_cache = {}
    # First pass: modl chunks carry the readable name, keyed by the same name
    # hash as the CSEG that holds its geometry; tex_ chunks give the mapping
    # from a segment's TNAM to the .png mercs_tex.py wrote for it, whose
    # filename carries the hash and so cannot be guessed from the name alone.
    for path in args.dsk:
        data = open(path, 'rb').read()
        count = struct.unpack_from('<I', data, 0)[0]
        ents = [struct.unpack_from('<III', data, 8 + i * 12) for i in range(count)]
        off = 8 + count * 12
        for size, h, grp in ents:
            start = off
            blob = data[off:off + size]
            off += size
            tag = blob[8:12]
            if tag == b'CSEG':
                # Remember where it is so a wheel can be fetched on demand
                # later, rather than holding every decompressed mesh in memory.
                cseg_loc[h] = (path, start, size)
                continue
            if tag not in (b'modl', b'tex_'):
                continue
            inner = blob[16:16 + struct.unpack_from('<I', blob, 12)[0]]
            if tag == b'modl':
                # The vertex bounding box lives here, keyed by the same name
                # hash as the CSEG whose segments are quantised against it.
                # Without it a segment's u16 positions cannot be placed at all.
                bx = model_box(inner)
                if bx:
                    boxes[h] = bx
                sk = parse_skeleton(inner)
                if sk:
                    skeletons[h] = sk
            for k, v in children(inner):
                if k != 'NAME':
                    continue
                n = cstr(v)
                if tag == b'modl':
                    names[h] = n
                    name_to_hash[n.lower()] = h
                else:
                    safe = ''.join(c if c.isalnum() or c in '_-' else '_' for c in n)
                    tex_index[n.lower()] = '%s_%08X.png' % (safe[:60] or 'tex', h)
                break

    written = tris_total = failed = 0
    for path in args.dsk:
        data = open(path, 'rb').read()
        count = struct.unpack_from('<I', data, 0)[0]
        ents = [struct.unpack_from('<III', data, 8 + i * 12) for i in range(count)]
        off = 8 + count * 12
        for size, h, grp in ents:
            blob = data[off:off + size]
            off += size
            if blob[8:12] != b'CSEG':
                continue
            if args.limit and written >= args.limit:
                break
            dec, comp = struct.unpack_from('<II', blob, 16)
            body = blob[24:]
            try:
                inner = body[:dec] if comp == STORED else decompress(body[:comp], dec)
            except Exception:
                failed += 1
                continue
            # Segments that ride a moving joint - rotors, turrets, barrels,
            # doors - are authored in BONE-LOCAL space, so they need their
            # joint's world transform to sit where they belong. Segments on the
            # root need nothing, and the root is identity, so applying it to
            # every segment is both correct and harmless.
            #
            # The box mapping above is what puts the numbers in the right units;
            # this is what puts the moving parts in the right PLACE. Both are
            # needed, and doing only the second is why an earlier attempt at
            # this appeared to change nothing.
            box = boxes.get(h)
            world = skeletons.get(h, {})
            segs = []
            for k, v in children(inner[8:]):
                if k == 'segm':
                    s = parse_segm(v, box)
                    if s and s['tris']:
                        segs.append(place_segment(s, world))
            if not segs:
                continue
            name0 = names.get(h) or ''
            if args.attach_wheels and name0:
                wheels += attach_wheels(name0, world, segs, cseg_loc, boxes,
                                        skeletons, names, name_to_hash, seg_cache)
            name = names.get(h) or '%08X' % h
            safe = ''.join(c if c.isalnum() or c in '_-' else '_' for c in name)
            # One directory per archive. LOCKED.DSK reuses the name hashes of
            # STREAMED.DSK for its variants, so a flat output silently loses
            # 657 models to overwrites - they are different geometry under the
            # same key, not duplicates.
            sub = os.path.join(args.out,
                               os.path.splitext(os.path.basename(path))[0].lower())
            os.makedirs(sub, exist_ok=True)
            stem = '%s_%08X' % (safe[:60], h)
            write_obj(os.path.join(sub, stem + '.obj'), segs, args.scale, safe,
                      tex_index, args.tex_rel)
            written += 1
            tris_total += sum(len(s['tris']) for s in segs)
            used = []
            for s in segs:
                t = s['texture']
                if t and t not in used:
                    used.append(t)
            catalog.append((os.path.basename(sub), stem, name,
                            ';'.join(tex_index.get(t.lower(), '') for t in used),
                            ';'.join(used)))

    # A materials.csv beside the library, in the shape the asset browser already
    # reads for XCOM: it is what lets the browser say which meshes resolve to
    # textures without opening 2,950 files.
    if catalog:
        import csv
        mats_path = os.path.join(os.path.dirname(os.path.abspath(args.out)),
                                 'materials.csv')
        with open(mats_path, 'w', encoding='utf-8', newline='') as f:
            w = csv.writer(f)
            w.writerow(['package', 'mesh', 'name', 'textures', 'texture_names'])
            w.writerows(catalog)
        print('  materials.csv -> %s' % mats_path)

    print('  %d models -> %s' % (written, args.out))
    print('  %s triangles total' % format(tris_total, ','))
    if wheels:
        print('  %s wheel instances attached at wheel_* joints' % format(wheels, ','))
    if failed:
        print('  %d failed to decompress' % failed)
    return 0


if __name__ == '__main__':
    sys.exit(main())
