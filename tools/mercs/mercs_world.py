"""mercs_world.py - decode Mercenaries' wrld chunks to object placement lists.

A wrld chunk is a level's object layout. 824 of them in ASSETS.DSK holding
51,731 instances between them - every prop, building, vehicle spawn and light
post in the game, with a full transform each.

THE wrld LAYOUT

    NAME   the world's name, e.g. aclubs, aclubs_mission1
    TNAM   the tern chunk it sits on, e.g. Aclubs
    INFO   16 bytes; first u32 is the instance count
    TABL   the string pool every instance's properties refer to
    inst   one per placed object, repeated

    inst = INFO   u16
           XFRM   12 floats: a 3x3 rotation matrix then a 3-float position
           PROP   a flat list of u32 string hashes

TABL is  u32 count  followed by count records of

    u32 hash | u32 length | length bytes of text | one 0 byte

so it is a hash-to-text map, and PROP is nothing but hashes into it. That is
what turns an instance into something readable: hash D7034613 resolves to
"GeometryFile" and 36F9A493 to "dmz_skgardenbiglightpost", which is the model
to place - and that model has already been exported by mercs_mesh.py under
that name.

The hash function itself is never needed. Every hash an instance uses is
present in its own TABL, so the mapping is read from the file rather than
recomputed.

OUTPUT. One .csv per world - instance, model, position, rotation - which is the
form that is actually loadable, plus one .json carrying every property of every
instance for the cases where the extra fields matter.

Read-only research - see the header of mercs_dsk.py.
"""
import argparse
import csv
import json
import os
import struct
import sys


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


def read_table(buf):
    """u32 count, then {u32 hash, u32 len, text, 0} records."""
    if len(buf) < 4:
        return {}
    count = struct.unpack_from('<I', buf, 0)[0]
    out, p = {}, 4
    for _ in range(count):
        if p + 8 > len(buf):
            break
        h, n = struct.unpack_from('<II', buf, p)
        p += 8
        if p + n > len(buf):
            break
        out[h] = buf[p:p + n].decode('ascii', 'replace')
        p += n + 1                      # skip the terminator
    return out


def read_props(buf, table):
    """PROP is a flat run of u32 hashes; resolve each through the world's TABL.

    The first 8 bytes are a header rather than a pair, and the rest read as
    key/value: the run for a lamp post is GeometryFile, its model name,
    objectType, and so on.
    """
    vals = []
    for i in range(8, len(buf) - 3, 4):
        h = struct.unpack_from('<I', buf, i)[0]
        vals.append(table.get(h, '%08X' % h))
    props = {}
    for i in range(0, len(vals) - 1, 2):
        props[vals[i]] = vals[i + 1]
    return props, vals


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dsk', nargs='+')
    ap.add_argument('--out', required=True)
    ap.add_argument('--json', action='store_true',
                    help='also write every property of every instance')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    worlds = insts = 0
    for path in args.dsk:
        data = open(path, 'rb').read()
        count = struct.unpack_from('<I', data, 0)[0]
        ents = [struct.unpack_from('<III', data, 8 + i * 12) for i in range(count)]
        off = 8 + count * 12
        seen = set()
        for size, h, grp in ents:
            blob = data[off:off + size]
            off += size
            if blob[8:12] != b'wrld' or h in seen:
                continue
            seen.add(h)
            inner = blob[16:16 + struct.unpack_from('<I', blob, 12)[0]]
            kids = children(inner)
            d = dict(kids)
            name = d.get('NAME', b'').split(b'\0')[0].decode('ascii', 'replace') or '%08X' % h
            terrain = d.get('TNAM', b'').split(b'\0')[0].decode('ascii', 'replace')
            table = read_table(d.get('TABL', b''))

            rows, full = [], []
            for k, v in kids:
                if k != 'inst':
                    continue
                kd = dict(children(v))
                if 'XFRM' not in kd or len(kd['XFRM']) < 48:
                    continue
                m = struct.unpack_from('<12f', kd['XFRM'], 0)
                props, order = read_props(kd.get('PROP', b''), table)
                model = props.get('GeometryFile', '')
                rows.append({
                    'index': len(rows), 'model': model,
                    'name': props.get('name', ''),
                    'object_type': props.get('objectType', ''),
                    'x': round(m[9], 4), 'y': round(m[10], 4), 'z': round(m[11], 4),
                    'r00': round(m[0], 6), 'r01': round(m[1], 6), 'r02': round(m[2], 6),
                    'r10': round(m[3], 6), 'r11': round(m[4], 6), 'r12': round(m[5], 6),
                    'r20': round(m[6], 6), 'r21': round(m[7], 6), 'r22': round(m[8], 6),
                })
                if args.json:
                    full.append({'index': len(full), 'transform': list(m),
                                 'properties': props, 'property_order': order})
            if not rows:
                continue
            safe = ''.join(c if c.isalnum() or c in '_-' else '_' for c in name)
            with open(os.path.join(args.out, safe + '.csv'), 'w', encoding='utf-8',
                      newline='') as f:
                w = csv.DictWriter(f, fieldnames=list(rows[0]))
                w.writeheader()
                w.writerows(rows)
            if args.json:
                with open(os.path.join(args.out, safe + '.json'), 'w',
                          encoding='utf-8') as f:
                    json.dump({'world': name, 'terrain': terrain,
                               'instances': full}, f, indent=1)
            worlds += 1
            insts += len(rows)
    print('  %d worlds, %s instances -> %s'
          % (worlds, format(insts, ','), args.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
