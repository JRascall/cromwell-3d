"""
hd2_index.py - index the unpacked Helldivers 2 bundles so they can be queried.

The same idea as xcom_index.py: 3389 bundles named by hash tell you nothing, so
read each one's type table and record what it actually contains. A bundle is a
mixed bag - a unit bundle carries its meshes, materials, textures and bones
together - so the useful question is "which bundles hold units" rather than
"where is mesh X".

    py -3 tools/hd2_index.py --library hd2_extracted/data
    py -3 tools/hd2_index.py --library hd2_extracted/data --query unit
    py -3 tools/hd2_index.py --library hd2_extracted/data --query "texture>200"

Type names are recovered by hashing candidates with murmur64a (seed 0) and
matching, which is how Stingray names every resource type. Anything unmatched
is reported as a raw hash rather than guessed at.
"""

import argparse
import csv
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hd2_dsar import read_bundle_types  # noqa: E402


def murmur64a(key, seed=0):
    m = 0xC6A4A7935BD1E995
    r = 47
    mask = 0xFFFFFFFFFFFFFFFF
    h = (seed ^ (len(key) * m)) & mask
    n = len(key) - len(key) % 8
    for i in range(0, n, 8):
        k = struct.unpack_from('<Q', key, i)[0]
        k = (k * m) & mask
        k ^= k >> r
        k = (k * m) & mask
        h ^= k
        h = (h * m) & mask
    tail = key[n:]
    if tail:
        for i, b in enumerate(tail):
            h ^= b << (8 * i)
        h = (h * m) & mask
    h ^= h >> r
    h = (h * m) & mask
    h ^= h >> r
    return h


# Stingray/Bitsquid resource type names. Only the ones that actually hash to a
# type present in the data end up in the index; the rest cost nothing.
TYPE_NAMES = [
    'animation', 'animation_curves', 'animation_set', 'bik', 'bones', 'camera_shake',
    'cloth', 'composite_shader', 'config', 'data', 'entity', 'flow', 'font',
    'geometry_group', 'havok_ai_properties', 'havok_cloth', 'havok_destruction',
    'havok_navigation_mesh', 'havok_physics_properties', 'havok_shape', 'ik_skeleton',
    'level', 'light_cookie', 'lua', 'material', 'material_config', 'mesh',
    'mouse_cursor', 'nav_mesh', 'navdata', 'network_config', 'occluder', 'package',
    'particles', 'physics', 'physics_properties', 'prefab', 'ragdoll_profile',
    'render_config', 'render_target', 'runtime_font', 'scene', 'shader_library',
    'shader_library_group', 'shading_environment', 'shading_environment_mapping',
    'shading_environment_template', 'slug', 'slug_album', 'sound_environment',
    'speedtree', 'spu_job', 'state_machine', 'string_table', 'strings',
    'surface_properties', 'terrain', 'texture', 'texture_atlas', 'timpani_bank',
    'timpani_master', 'unit', 'unit_anim', 'upb', 'vector_field', 'vehicle',
    'weapon', 'world', 'wwise_bank', 'wwise_dep', 'wwise_event', 'wwise_metadata',
    'wwise_properties', 'wwise_stream',
]
TYPE_BY_HASH = {murmur64a(n.encode()): n for n in TYPE_NAMES}


def type_label(h):
    return TYPE_BY_HASH.get(h, '0x%016x' % h)


def scan(library):
    """One row per bundle: sizes plus a count per resource type."""
    rows = []
    all_types = set()
    for name in sorted(os.listdir(library)):
        if '.' in name or len(name) != 16:
            continue                                   # only the bundle files
        path = os.path.join(library, name)
        if not os.path.isfile(path):
            continue
        with open(path, 'rb') as f:
            head = f.read(80 + 32 * 512)               # header + type table
        ntypes, nfiles, types = read_bundle_types(head)
        if ntypes is None:
            continue                                   # not a raw bundle, skip
        row = {
            'bundle': name,
            'bytes': os.path.getsize(path),
            'stream_bytes': _size(library, name + '.stream'),
            'gpu_bytes': _size(library, name + '.gpu_resources'),
            'files': nfiles,
            'types': ntypes,
        }
        for thash, count in types:
            label = type_label(thash)
            row[label] = row.get(label, 0) + count
            all_types.add(label)
        rows.append(row)
    return rows, sorted(all_types)


def _size(library, name):
    p = os.path.join(library, name)
    return os.path.getsize(p) if os.path.exists(p) else 0


def write_csv(rows, types, path):
    cols = ['bundle', 'bytes', 'stream_bytes', 'gpu_bytes', 'files', 'types'] + types
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=cols, restval=0)
        w.writeheader()
        for r in rows:
            w.writerow(r)


def query(rows, expr):
    """`unit` = has any; `texture>200` = threshold; several terms are AND-ed."""
    terms = []
    for part in expr.split():
        m = re.match(r'^([A-Za-z_0-9]+)\s*([<>]=?|=)\s*(\d+)$', part)
        terms.append((m.group(1), m.group(2), int(m.group(3))) if m else (part, '>', 0))
    out = []
    for r in rows:
        ok = True
        for key, op, val in terms:
            got = r.get(key, 0)
            if op in ('>', '>='):
                ok &= got > val if op == '>' else got >= val
            elif op in ('<', '<='):
                ok &= got < val if op == '<' else got <= val
            else:
                ok &= got == val
            if not ok:
                break
        if ok:
            out.append(r)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--library', required=True, help='directory of unpacked bundles')
    ap.add_argument('--query', help='filter expression, e.g. "unit" or "texture>200"')
    ap.add_argument('--top', type=int, default=25)
    args = ap.parse_args()

    rows, types = scan(args.library)
    if not rows:
        sys.exit('no raw bundles found in %s (did hd2_unpack.py run?)' % args.library)

    out = os.path.join(args.library, 'index.csv')
    write_csv(rows, types, out)

    total = sum(r['bytes'] + r['stream_bytes'] + r['gpu_bytes'] for r in rows)
    print('%d bundles, %d resources, %.1f GB -> %s'
          % (len(rows), sum(r['files'] for r in rows), total / 1024.0 ** 3, out))

    counts = {t: sum(r.get(t, 0) for r in rows) for t in types}
    print('\nresource types:')
    for t, c in sorted(counts.items(), key=lambda kv: -kv[1]):
        print('  %-32s %d' % (t, c))

    if args.query:
        hits = sorted(query(rows, args.query), key=lambda r: -r['files'])
        print('\n"%s" matched %d bundles:' % (args.query, len(hits)))
        for r in hits[:args.top]:
            extra = ' '.join('%s=%d' % (t, r[t]) for t in types if r.get(t))
            print('  %-18s %6d files  %s' % (r['bundle'], r['files'], extra))


if __name__ == '__main__':
    main()
