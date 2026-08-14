"""mercs_scene.py - compose one Mercenaries world into a single renderable .obj.

The pieces exist separately after mercs_terrain.py and mercs_world.py: a
heightmap per level, and a placement list naming a model and a transform for
every object. This puts them together - terrain plus every instance, each
model's geometry transformed into place - so a level can be looked at as a
level rather than as a spreadsheet.

COORDINATES. Instance positions are centred on the origin (aclubs runs
-1598..510 in x and -497..1473 in z) while the heightmap is indexed from a
corner, so the terrain mesh is emitted centred: a 512 grid at 8 m becomes
-2048..+2048. Without that shift the props sit in one quadrant of the map.

SIZE. A world is big: aclubs is 711 instances over 67 distinct models, and the
larger ones run to 8,546 instances. .obj has no instancing, so every placement
duplicates its geometry and the files get large quickly. --max-instances and
--terrain-step are there to keep a scene openable.

Read-only research - see the header of mercs_dsk.py.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mercs_lzss import decompress, STORED                       # noqa: E402
from mercs_tex import rle_decode                                # noqa: E402
from mercs_mesh import (children, cstr, parse_segm, model_box,  # noqa: E402
                        parse_skeleton, place_segment)
from mercs_terrain import detile                                # noqa: E402


def index_archives(paths):
    """name -> geometry, plus the boxes and skeletons those meshes need."""
    idx = {'cseg': {}, 'box': {}, 'skel': {}, 'name': {}, 'tern': {}, 'wrld': {}}
    for path in paths:
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
                idx['cseg'][h] = (path, start, size)
                continue
            if tag not in (b'modl', b'tern', b'wrld'):
                continue
            inner = blob[16:16 + struct.unpack_from('<I', blob, 12)[0]]
            nm = ''
            for k, v in children(inner):
                if k == 'NAME':
                    nm = cstr(v)
                    break
            if tag == b'modl':
                idx['name'][nm.lower()] = h
                bx = model_box(inner)
                if bx:
                    idx['box'][h] = bx
                sk = parse_skeleton(inner)
                if sk:
                    idx['skel'][h] = sk
            elif tag == b'tern':
                idx['tern'][nm.lower()] = inner
            else:
                idx['wrld'].setdefault(nm.lower(), inner)
    return idx


def segments_for(h, idx, cache):
    if h in cache:
        return cache[h]
    loc = idx['cseg'].get(h)
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
    box, world = idx['box'].get(h), idx['skel'].get(h, {})
    segs = []
    for k, v in children(inner[8:]):
        if k == 'segm':
            s = parse_segm(v, box)
            if s and s['tris']:
                segs.append(place_segment(s, world))
    cache[h] = segs
    return segs


def read_table(buf):
    if len(buf) < 4:
        return {}
    n = struct.unpack_from('<I', buf, 0)[0]
    out, p = {}, 4
    for _ in range(n):
        if p + 8 > len(buf):
            break
        h, ln = struct.unpack_from('<II', buf, p)
        p += 8
        if p + ln > len(buf):
            break
        out[h] = buf[p:p + ln].decode('ascii', 'replace')
        p += ln + 1
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--assets', required=True, help='ASSETS.DSK')
    ap.add_argument('--models', nargs='+', required=True, help='STREAMED.DSK LOCKED.DSK')
    ap.add_argument('--world', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--tex-rel', default='../textures_png')
    ap.add_argument('--terrain-step', type=int, default=4)
    ap.add_argument('--max-instances', type=int, default=0)
    ap.add_argument('--no-terrain', action='store_true')
    args = ap.parse_args()

    idx = index_archives([args.assets] + args.models)
    key = args.world.lower()
    if key not in idx['wrld']:
        near = [k for k in idx['wrld'] if key in k][:8]
        print('no world %r. close matches: %s' % (args.world, near or 'none'))
        return 2
    wrld = idx['wrld'][key]
    wd = dict(children(wrld))
    terrain_name = cstr(wd.get('TNAM', b'')).lower()
    table = read_table(wd.get('TABL', b''))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or '.', exist_ok=True)
    mtl_path = os.path.splitext(args.out)[0] + '.mtl'
    materials, cache = [], {}
    seen_mat = set()
    vbase = tbase = 1
    tris = 0

    with open(args.out, 'w', encoding='utf-8') as f:
        f.write('# Mercenaries world "%s" (terrain %s)\n' % (args.world, terrain_name))
        f.write('mtllib %s\n\n' % os.path.basename(mtl_path))

        # ---- terrain, centred on the origin ----
        if not args.no_terrain and terrain_name in idx['tern']:
            td = dict(children(idx['tern'][terrain_name]))
            unit, hscale, lo, hi = struct.unpack_from('<4f', td['INFO'], 0)
            grid = struct.unpack_from('<2H', td['INFO'], 16)[0]
            raw, _ = rle_decode(td['HGT8'], grid * grid)
            hm = detile(raw, grid)
            step = max(1, args.terrain_step)
            span = (hi - lo) / 255.0
            half = grid * unit / 2.0
            f.write('g terrain\n')
            cols = list(range(0, grid, step))
            for y in cols:
                for x in cols:
                    f.write('v %.2f %.2f %.2f\n'
                            % (x * unit - half, lo + hm[y * grid + x] * span,
                               y * unit - half))
            w = len(cols)
            for j in range(w - 1):
                for i in range(w - 1):
                    a = vbase + j * w + i
                    f.write('f %d %d %d\nf %d %d %d\n'
                            % (a, a + 1, a + w, a + 1, a + w + 1, a + w))
                    tris += 2
            vbase += w * w
            print('  terrain %s: %d x %d samples' % (terrain_name, w, w))

        # ---- instances ----
        placed = missing = 0
        for k, v in children(wrld):
            if k != 'inst':
                continue
            if args.max_instances and placed >= args.max_instances:
                break
            kd = dict(children(v))
            if 'XFRM' not in kd or len(kd['XFRM']) < 48:
                continue
            m = struct.unpack_from('<12f', kd['XFRM'], 0)
            R = ((m[0], m[1], m[2]), (m[3], m[4], m[5]), (m[6], m[7], m[8]))
            t = (m[9], m[10], m[11])
            prop = kd.get('PROP', b'')
            vals = [table.get(struct.unpack_from('<I', prop, i)[0], '')
                    for i in range(8, len(prop) - 3, 4)]
            model = ''
            for i in range(0, len(vals) - 1):
                if vals[i] == 'GeometryFile':
                    model = vals[i + 1]
                    break
            mh = idx['name'].get(model.lower())
            if mh is None:
                missing += 1
                continue
            segs = segments_for(mh, idx, cache)
            if not segs:
                continue
            placed += 1
            safe = ''.join(c if c.isalnum() or c in '_-' else '_' for c in model)
            f.write('g inst%d_%s\n' % (placed, safe[:40]))
            for s in segs:
                tex = s['texture']
                if tex and tex not in seen_mat:
                    seen_mat.add(tex)
                    materials.append(tex)
                if tex:
                    f.write('usemtl %s\n' % tex)
                for x, y, z in s['pos']:
                    f.write('v %.3f %.3f %.3f\n'
                            % (R[0][0] * x + R[0][1] * y + R[0][2] * z + t[0],
                               R[1][0] * x + R[1][1] * y + R[1][2] * z + t[1],
                               R[2][0] * x + R[2][1] * y + R[2][2] * z + t[2]))
                for u, vv in s['uv']:
                    f.write('vt %.5f %.5f\n' % (u, 1.0 - vv))
                for a, b, c in s['tris']:
                    if s['uv']:
                        f.write('f %d/%d %d/%d %d/%d\n'
                                % (vbase + a, tbase + a, vbase + b, tbase + b,
                                   vbase + c, tbase + c))
                    else:
                        f.write('f %d %d %d\n' % (vbase + a, vbase + b, vbase + c))
                    tris += 1
                vbase += len(s['pos'])
                tbase += len(s['uv'])

    # textures were already decoded by mercs_tex.py; name them by their stem
    tex_dir = os.path.join(os.path.dirname(os.path.abspath(args.out)),
                           args.tex_rel.replace('/', os.sep))
    have = {}
    if os.path.isdir(tex_dir):
        for fn in os.listdir(tex_dir):
            if fn.lower().endswith('.png'):
                have.setdefault(fn.rsplit('_', 1)[0].lower(), fn)
    with open(mtl_path, 'w', encoding='utf-8') as mf:
        for name in materials:
            mf.write('newmtl %s\nKd 1 1 1\n' % name)
            png = have.get(name.lower())
            if png:
                mf.write('map_Kd %s/%s\n' % (args.tex_rel, png))
            mf.write('\n')

    print('  %d instances placed, %d models not found, %s triangles'
          % (placed, missing, format(tris, ',')))
    print('  -> %s' % args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
