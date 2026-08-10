"""xcom_index.py - index the bulk-converted asset library.

A parcel is assembled by asking questions like "what full-cover props are one
tile square?" or "what wall segments run two tiles?". Six hundred folders
cannot answer that; a CSV can.

Reads every mesh in xcom_extracted/models/ and writes index.csv with, per mesh:
package, name, tris, tile dimensions, and the COVER CLASS and TILE FOOTPRINT
parsed out of XCOM's own naming convention - which is reliable enough to sort
by, because Firaxis' environment art names encode both:

    CndrBlkWall_HiFence_x1_A        full cover, 1 tile run
    SandBagsStr_LoCov_2x1_A         low cover, 2x1
    TemperatePlateau_LowCover_6x6   low cover, 6x6
    TWN_RdPltStr_08x08_A            8x8 tile road plate
    LadderMetalA_256                a 256uu climb

Usage:
    py -3 tools/xcom_index.py --library xcom_extracted/models
    py -3 tools/xcom_index.py --library xcom_extracted/models --query "hicover 1x1"
"""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path

# Cover class as XCOM spells it, in all the variants seen across the content.
COVER_PATTERNS = [
    (re.compile(r'hi(?:gh)?cov(?:er)?|hifence', re.I), 'high'),
    (re.compile(r'lo(?:w)?cov(?:er)?|lofence', re.I), 'low'),
    (re.compile(r'deco|scatter', re.I), 'deco'),
]
FOOTPRINT_RE = re.compile(r'(?<![a-z0-9])(\d{1,2})x(\d{1,2})(?![0-9])', re.I)
RUN_RE = re.compile(r'(?<![a-z0-9])x(\d{1,2})(?![0-9])', re.I)
# A bare 3-digit number on a ladder-ish name is a climb height in unreal units.
HEIGHT_RE = re.compile(r'_(\d{3})$')
DESTROYED_RE = re.compile(r'destro|destroyed|debris|_dest\b|wreck|burned|ruin', re.I)


def classify(name: str) -> dict:
    cover = ''
    for pat, label in COVER_PATTERNS:
        if pat.search(name):
            cover = label
            break
    fp = FOOTPRINT_RE.search(name)
    footprint = f"{int(fp.group(1))}x{int(fp.group(2))}" if fp else ''
    run = ''
    if not footprint:
        r = RUN_RE.search(name)
        if r:
            run = f"x{int(r.group(1))}"
    # A trailing 3-digit number is a climb height only when it plausibly is
    # one: XCOM ladders come in whole z-cells (64uu), so 192/256/512 qualify
    # while the _001/_002 variant suffixes that litter the content do not.
    h = HEIGHT_RE.search(name)
    climb = ''
    if h:
        val = int(h.group(1))
        if val >= 64 and val % 32 == 0:
            climb = val
    return {
        'cover': cover,
        'footprint': footprint or run,
        'climb_uu': climb,
        'destroyed': 'yes' if DESTROYED_RE.search(name) else '',
    }


def mesh_stats(path: Path):
    tris = 0
    lo = [1e9] * 3
    hi = [-1e9] * 3
    for line in path.open(errors='replace'):
        if line.startswith('v '):
            q = [float(x) for x in line.split()[1:4]]
            lo = [min(a, b) for a, b in zip(lo, q)]
            hi = [max(a, b) for a, b in zip(hi, q)]
        elif line.startswith('f '):
            tris += 1
    if tris == 0:
        return None
    return tris, [b - a for a, b in zip(lo, hi)]


def build(library: Path) -> list[dict]:
    rows = []
    for pkg_dir in sorted(p for p in library.iterdir() if p.is_dir()):
        textures = sorted(t.name for t in pkg_dir.glob('*.png'))
        for obj in sorted(pkg_dir.glob('*.obj')):
            st = mesh_stats(obj)
            if not st:
                continue
            tris, size = st
            row = {
                'package': pkg_dir.name,
                'mesh': obj.stem,
                'tris': tris,
                'w': round(size[0], 3),
                'h': round(size[1], 3),
                'd': round(size[2], 3),
                'path': str(obj.relative_to(library)).replace('\\', '/'),
                'textures': len(textures),
            }
            row.update(classify(obj.stem))
            rows.append(row)
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--library', required=True, type=Path)
    ap.add_argument('--query', default=None,
                    help='space-separated terms matched against mesh/package/'
                         'cover/footprint; prints matches instead of writing')
    args = ap.parse_args(argv)

    rows = build(args.library)
    if not rows:
        print(f"No meshes found under {args.library}")
        return 0

    if args.query:
        terms = args.query.lower().split()
        for r in rows:
            hay = f"{r['package']} {r['mesh']} {r['cover']} {r['footprint']}".lower()
            if all(t in hay for t in terms):
                print(f"  {r['package']}/{r['mesh']:44s} {r['tris']:6d} tris  "
                      f"{r['w']:.2f} x {r['h']:.2f} x {r['d']:.2f}  "
                      f"{r['cover']:5s} {r['footprint']}")
        return 0

    out = args.library / 'index.csv'
    cols = ['package', 'mesh', 'cover', 'footprint', 'climb_uu', 'destroyed',
            'tris', 'w', 'h', 'd', 'textures', 'path']
    with out.open('w', newline='', encoding='utf-8') as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)

    pkgs = len({r['package'] for r in rows})
    by_cover = {}
    for r in rows:
        by_cover[r['cover'] or '-'] = by_cover.get(r['cover'] or '-', 0) + 1
    print(f"Indexed {len(rows)} meshes from {pkgs} packages -> {out}")
    print("  by cover class: " + ", ".join(f"{k}={v}" for k, v in sorted(by_cover.items())))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
