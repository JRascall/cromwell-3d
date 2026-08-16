"""mercs_gameplay.py - decode the chunks the mission scripts actually refer to.

WHY THESE FIVE TOGETHER

The 198 Lua files are the game's logic in source form, and they spend most of
their time naming things that live in these chunks. A mission script says

    tPath = { "path_SongHind1", ... }               -> path
    tEncounter = { Insertion, LZ, GPS1, songtower } -> enc_ and lrg_
    tLineRegion = { AceLost = "acelost" }           -> rgns

and until these are decoded you have a script that says "fly the Hind along
path_SongHind3" and no path_SongHind3 anywhere. That is the single most
load-bearing gap left in the extraction: not a missing asset, a missing
referent. atbl is the same problem one level down - it maps a logical animation
slot to one of the 1,835 clips, so without it every clip is an anonymous name.

    path  350   AI and vehicle routes: points, orientations, junctions
    lrg_   96   named point groups - the "large" landmarks scripts spawn at
    enc_  236   encounters, each a set of squads
    atbl   95   animation tables: logical slot -> clip name
    rgns   12   region volumes, box or sphere, with a transform

NONE OF THIS NEEDED REVERSE ENGINEERING

Like the audio tables and unlike the terrain, these are self-describing ucfb
trees: four-letter tags, sizes, and either null-separated ASCII key/value text
or plain float arrays. The only real question was whether the float arrays mean
what they look like, and one test settles it - see below.

    NAME  the chunk's name, null terminated
    INFO  three u16. For path the FIRST is the point count; for lrg_ it is the
          THIRD. Reported rather than trusted: the tool takes its counts from
          the chunk sizes, which cannot disagree with themselves.
    PROP  null-separated key/value text
    PNTS  float32 x, y, z per point
    ORNT  float32 quaternion per point
    JNCT  junction data, variable length and NOT per point - the byte-per-point
          ratio ranges from 0 to 41 across the 350 paths, so it is a list of
          its own and is emitted as hex
    XFRM  3x3 basis then position, 12 floats - the same layout wrld uses

WHAT MAKES THE PNTS/ORNT READING BELIEVABLE

ORNT has exactly one 16-byte entry per PNTS entry on all 350 paths, and all
3,133 of those quaternions are unit length to within 1%. That is a property a
wrong reading does not have: mis-stride the array, or read the floats as
anything else, and the magnitudes scatter. It also confirms PNTS is three
float32 rather than four, because otherwise the counts could not match.

Read-only research - see the header of mercs_dsk.py.
"""
import argparse
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mercs_fx import children, is_container, kv, read_table   # noqa: E402

KINDS = ('path', 'lrg_', 'enc_', 'atbl', 'rgns')


def name_of(d, fallback):
    return d.get('NAME', b'').split(b'\0')[0].decode('ascii', 'replace') or fallback


def floats(buf, stride):
    n = len(buf) // (stride * 4)
    return [struct.unpack_from('<%df' % stride, buf, i * stride * 4) for i in range(n)]


def props(buf):
    return dict(kv(buf))


def collect_strings(tag, buf, out):
    """Every null-terminated string under a subtree, in order.

    atbl nests clip names a couple of levels down (aset -> vanm -> ANAM) and
    the intermediate tag varies between tables, so walking for strings beats
    hard-coding a path that only holds for the tables looked at first.
    """
    if is_container(buf):
        for t, b in children(buf):
            collect_strings(t, b, out)
    else:
        for s in buf.split(b'\0'):
            if len(s) >= 3 and all(32 <= c < 127 for c in s):
                out.append(s.decode('ascii'))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dsk', nargs='+')
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    points, meta, marks, squads, anims, regions = [], [], [], [], [], []
    counts = collections.Counter()
    unit_bad = unit_all = 0

    for path in args.dsk:
        data = open(path, 'rb').read()
        for size, name_hash, group_hash, off in read_table(data):
            blob = data[off:off + size]
            kind = blob[8:12].decode('ascii', 'replace')
            if blob[:4] != b'ucfb' or kind not in KINDS:
                continue
            body = blob[16:16 + struct.unpack_from('<I', blob, 12)[0]]
            d = collections.OrderedDict()
            for t, b in children(body):
                d.setdefault(t, []).append(b)
            flat = {t: v[0] for t, v in d.items()}
            nm = name_of(flat, '%08X' % name_hash)
            counts[kind] += 1

            if kind in ('path', 'lrg_'):
                pts = floats(flat.get('PNTS', b''), 3)
                qs = floats(flat.get('ORNT', b''), 4)
                for i, p in enumerate(pts):
                    row = {'name': nm, 'point': i,
                           'x': '%.4f' % p[0], 'y': '%.4f' % p[1], 'z': '%.4f' % p[2]}
                    if i < len(qs):
                        q = qs[i]
                        mag = sum(c * c for c in q) ** 0.5
                        unit_all += 1
                        unit_bad += abs(mag - 1.0) > 0.01
                        row.update({'qx': '%.5f' % q[0], 'qy': '%.5f' % q[1],
                                    'qz': '%.5f' % q[2], 'qw': '%.5f' % q[3]})
                    (points if kind == 'path' else marks).append(row)
                row = {'name': nm, 'kind': kind, 'points': len(pts),
                       'jnct_bytes': len(flat.get('JNCT', b'')),
                       'jnct': flat.get('JNCT', b'').hex()}
                row.update(props(flat.get('PROP', b'')))
                meta.append(row)

            elif kind == 'enc_':
                for grp in d.get('SQDS', []) + d.get('PSTS', []):
                    for t, b in children(grp):
                        sub = {st: sb for st, sb in children(b)} if is_container(b) else {}
                        squads.append({'encounter': nm, 'group': t,
                                       'name': name_of(sub, ''),
                                       'info': sub.get('INFO', b'').hex()})

            elif kind == 'atbl':
                got = []
                for t, b in children(body):
                    if t in ('NAME', 'SIZE'):
                        continue
                    collect_strings(t, b, got)
                for i, clip in enumerate(got):
                    anims.append({'table': nm, 'slot': i, 'clip': clip})

            elif kind == 'rgns':
                for b in d.get('regn', []):
                    sub = {st: sb for st, sb in children(b)}
                    p = props(sub.get('PROP', b'')) or props(
                        b''.join(v for k, v in children(b) if k not in ('INFO', 'XFRM')))
                    x = floats(sub.get('XFRM', b''), 3)
                    regions.append({'region': p.get('name', ''), 'type': p.get('type', ''),
                                    'x': '%.3f' % x[3][0] if len(x) > 3 else '',
                                    'y': '%.3f' % x[3][1] if len(x) > 3 else '',
                                    'z': '%.3f' % x[3][2] if len(x) > 3 else '',
                                    'basis': ' '.join('%.4f' % c for r in x[:3] for c in r)})

    def dump(fname, rows):
        if not rows:
            return
        keys = []
        for r in rows:
            for k in r:
                if k not in keys:
                    keys.append(k)
        with open(os.path.join(args.out, fname), 'w', encoding='utf-8', newline='') as f:
            w = csv.DictWriter(f, fieldnames=keys, extrasaction='ignore')
            w.writeheader()
            w.writerows(rows)
        print('  %-20s %6d rows' % (fname, len(rows)))

    dump('path_points.csv', points)
    dump('path_meta.csv', meta)
    dump('landmark_points.csv', marks)
    dump('encounter_squads.csv', squads)
    dump('anim_tables.csv', anims)
    dump('regions.csv', regions)
    print('  chunks: %s' % ', '.join('%s=%d' % kv for kv in sorted(counts.items())))
    if unit_all:
        print('  path orientations unit-length: %d of %d fail'
              % (unit_bad, unit_all))
    return 0


if __name__ == '__main__':
    sys.exit(main())
