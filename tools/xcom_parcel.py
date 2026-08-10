"""xcom_parcel.py - turn an exported parcel's T3D actor dump into placements.

A parcel is a list of "this mesh, at this tile, turned this way". UnrealEd's
BatchExport writes each placed actor as a .T3D block that carries exactly that:

    Begin Object Class=XComLevelActor Name=XComLevelActor_0
          Archetype=XComLevelActor'Urban_RoadPlateA.Archetype_Ground.ARC_CTY_...'
       Begin Object Class=StaticMeshComponent ...
          StaticMesh=StaticMesh'GroundPlane.Meshes.GroundPlaneAx16'
       End Object
       Location=(X=576.000000,Y=480.000000,Z=48.000000)
       Rotation=(Pitch=0,Yaw=-16384,Roll=0)
       DrawScale3D=(X=-1.000000,Y=1.000000,Z=1.000000)
    End Object

This reads a directory of those and writes one CSV row per placement, in the
SAME units the converted meshes use: everything divided by 96, so a placement
coordinate and a mesh vertex are directly comparable.

Conventions worth knowing:
  * Unreal is Z-up; the converted OBJs are Y-up. Columns are named for OUR
    axes, so unreal Z becomes `y` (height) and unreal Y becomes `z`.
  * Rotation is in UE angle units: 65536 == 360 degrees. Yaw is emitted in
    degrees, and parcels are almost entirely multiples of 90.
  * A negative DrawScale3D component is a MIRROR, not a rotation - XCOM
    reuses one mesh for both handednesses. Preserve the sign or corner
    pieces will face the wrong way, and remember that mirroring flips
    triangle winding.

Usage:
    py -3 tools/xcom_parcel.py --t3d <dir> --out parcel.csv
    py -3 tools/xcom_parcel.py --t3d <dir> --summary
"""

from __future__ import annotations

import argparse
import csv
import re
from collections import Counter
from pathlib import Path

UU_PER_TILE = 96.0
UU_PER_ZCELL = 64.0
YAW_UNITS = 65536.0

OBJ_RE = re.compile(r"^\s*Begin Object Class=(\S+)\s+Name=(\S+)", re.M)
MESH_RE = re.compile(r"StaticMesh=StaticMesh'([^']+)'")
ARCH_RE = re.compile(r"^\s*Begin Object Class=\S+\s+Name=\S+\s+Archetype=\S+'([^']+)'", re.M)
LOC_RE = re.compile(r"^\s*Location=\(X=([-\d.eE]+),Y=([-\d.eE]+),Z=([-\d.eE]+)\)", re.M)
ROT_RE = re.compile(r"^\s*Rotation=\(Pitch=([-\d]+),Yaw=([-\d]+),Roll=([-\d]+)\)", re.M)
SCALE3_RE = re.compile(r"^\s*DrawScale3D=\(X=([-\d.eE]+),Y=([-\d.eE]+),Z=([-\d.eE]+)\)", re.M)
SCALE_RE = re.compile(r"^\s*DrawScale=([-\d.eE]+)", re.M)


def norm_deg(units: float) -> float:
    d = units * 360.0 / YAW_UNITS
    d %= 360.0
    return round(d, 3)


def parse_t3d(path: Path) -> dict | None:
    txt = path.read_text(errors="replace")

    mesh = MESH_RE.search(txt)
    if not mesh:
        return None  # a light, a volume, a spawn point - nothing to place
    pkg_path = mesh.group(1)          # e.g. GroundPlane.Meshes.GroundPlaneAx16
    bits = pkg_path.split(".")
    mesh_pkg, mesh_name = bits[0], bits[-1]

    cls = OBJ_RE.search(txt)
    arch = ARCH_RE.search(txt)
    loc = LOC_RE.search(txt)
    rot = ROT_RE.search(txt)
    s3 = SCALE3_RE.search(txt)
    s1 = SCALE_RE.search(txt)

    lx, ly, lz = (float(g) for g in loc.groups()) if loc else (0.0, 0.0, 0.0)
    uni = float(s1.group(1)) if s1 else 1.0
    sx, sy, sz = ((float(g) * uni for g in s3.groups()) if s3 else (uni, uni, uni))

    return {
        "actor": path.stem,
        "class": cls.group(1) if cls else "",
        "archetype": arch.group(1) if arch else "",
        "mesh_package": mesh_pkg,
        "mesh": mesh_name,
        # Our axes: unreal X -> x, unreal Z (up) -> y, unreal Y -> z.
        "x": round(lx / UU_PER_TILE, 4),
        "y": round(lz / UU_PER_TILE, 4),
        "z": round(ly / UU_PER_TILE, 4),
        # Handy secondary read: which storey/cell the actor sits on.
        "z_cell": round(lz / UU_PER_ZCELL, 3),
        "yaw_deg": norm_deg(float(rot.group(2))) if rot else 0.0,
        "pitch_deg": norm_deg(float(rot.group(1))) if rot else 0.0,
        "roll_deg": norm_deg(float(rot.group(3))) if rot else 0.0,
        "scale_x": round(sx, 4),
        "scale_y": round(sz, 4),   # our y is unreal z
        "scale_z": round(sy, 4),
        "mirrored": "yes" if sx * sy * sz < 0 else "",
        "uu_x": lx, "uu_y": lz, "uu_z": ly,
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--t3d", required=True, type=Path, help="dir of exported .T3D files")
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--summary", action="store_true")
    args = ap.parse_args(argv)

    rows = []
    skipped = 0
    for f in sorted(args.t3d.glob("*.T3D")):
        r = parse_t3d(f)
        if r:
            rows.append(r)
        else:
            skipped += 1

    if not rows:
        print(f"No placeable actors found in {args.t3d}")
        return 1

    if args.summary:
        print(f"{len(rows)} placements ({skipped} non-mesh actors skipped)")
        xs = [r["x"] for r in rows]; zs = [r["z"] for r in rows]; ys = [r["y"] for r in rows]
        print(f"  extent   x {min(xs):.1f}..{max(xs):.1f} tiles   "
              f"z {min(zs):.1f}..{max(zs):.1f} tiles   y {min(ys):.2f}..{max(ys):.2f}")
        # Props sit on tile CORNERS and tile CENTRES alike, so the meaningful
        # test is alignment to a half tile, not a whole one.
        def aligned(v, step):
            return abs(v / step - round(v / step)) < 0.02
        whole = sum(1 for r in rows if aligned(r['x'], 1) and aligned(r['z'], 1))
        half = sum(1 for r in rows if aligned(r['x'], .5) and aligned(r['z'], .5))
        print(f"  aligned to whole tile: {whole}/{len(rows)}   "
              f"to half tile: {half}/{len(rows)}")
        yaws = Counter(r["yaw_deg"] for r in rows)
        print("  yaws: " + ", ".join(f"{k:g}deg x{v}" for k, v in sorted(yaws.items())[:8]))
        print(f"  mirrored: {sum(1 for r in rows if r['mirrored'])}")
        print("  top packages:")
        for pkg, n in Counter(r["mesh_package"] for r in rows).most_common(12):
            print(f"     {n:4d}  {pkg}")
        return 0

    out = args.out or (args.t3d / "placements.csv")
    cols = list(rows[0].keys())
    with out.open("w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)
    print(f"Wrote {len(rows)} placements -> {out}  ({skipped} non-mesh actors skipped)")
    print(f"  distinct meshes: {len({r['mesh'] for r in rows})} "
          f"from {len({r['mesh_package'] for r in rows})} packages")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
