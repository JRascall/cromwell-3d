"""xcom_convert.py - turn raw XCOM 2 SDK exports into engine-ready assets.

What xcom_extract.ps1 gives you is not loadable art. UnrealEd's OBJ exporter
writes:

  * unindexed triangle soup - every triangle carries its own 3 vertices, so a
    600-tri wall ships 1788 duplicated positions;
  * NO vertex normals at all (`vn` count is zero), which the PBR shader needs;
  * unreal units, where one XCOM tile is 96uu and the prototype's kTileSize is
    1.0 (see src/core/lattice/Constants.hpp);
  * whatever winding UE3's Z-up -> Y-up conversion happened to produce.

This script fixes all four, and converts the fat uncompressed TGAs to PNG.

Usage
-----
    py -3 tools/xcom_convert.py --raw workbench/xcom_raw/CinderblockWallA \
                                --out assets/models/cinderblock_wall \
                                --mesh CndrBlkWall_HiFence_x1_A=wall_hi_x1 \
                                --mesh CndrBlkWall_LoFence_x1_A=wall_lo_x1 \
                                --tex  CinderblockWallB_DIF=wall_dif \
                                --diffuse wall_dif

`--mesh`/`--tex` take `SourceName=OutputName` pairs; pass them without `=` to
keep the original name. Omit them entirely to convert everything found.

Requires: numpy, Pillow.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import numpy as np

# One XCOM tile is 96 unreal units and the prototype calls that 1.0.
UNREAL_UNITS_PER_TILE = 96.0

# Faces meeting at a sharper angle than this keep a hard edge instead of being
# smoothed together. 45 deg keeps cinderblock corners crisp while letting
# curved trim (pipes, posts) round off.
DEFAULT_SMOOTH_DEGREES = 45.0


# --------------------------------------------------------------------- OBJ io
def read_raw_obj(path: Path):
    """Parse UnrealEd's OBJ. Returns (positions[N,3], uvs[M,2], faces[F,3,2]).

    Faces are `f v/vt v/vt v/vt` - no normal indices, which is the whole
    problem this script exists to solve.
    """
    positions: list[tuple[float, float, float]] = []
    uvs: list[tuple[float, float]] = []
    faces: list[list[tuple[int, int]]] = []

    for line in path.read_text(errors="replace").splitlines():
        if not line or line[0] == "#":
            continue
        tag, _, rest = line.partition(" ")
        if tag == "v":
            x, y, z = rest.split()[:3]
            positions.append((float(x), float(y), float(z)))
        elif tag == "vt":
            u, v = rest.split()[:2]
            uvs.append((float(u), float(v)))
        elif tag == "f":
            corners = []
            for token in rest.split():
                bits = token.split("/")
                vi = int(bits[0])
                ti = int(bits[1]) if len(bits) > 1 and bits[1] else 0
                corners.append((vi, ti))
            # Fan-triangulate defensively; UE3 already emits triangles.
            for k in range(1, len(corners) - 1):
                faces.append([corners[0], corners[k], corners[k + 1]])

    if not faces:
        raise ValueError(f"{path.name}: no faces found")

    pos = np.asarray(positions, dtype=np.float64)
    uv = np.asarray(uvs, dtype=np.float64) if uvs else np.zeros((0, 2))
    # OBJ indices are 1-based and may be negative (relative); normalise.
    idx = np.asarray(faces, dtype=np.int64)
    vi = np.where(idx[:, :, 0] < 0, len(pos) + idx[:, :, 0], idx[:, :, 0] - 1)
    ti = np.where(idx[:, :, 1] < 0, len(uv) + idx[:, :, 1], idx[:, :, 1] - 1)
    return pos, uv, np.stack([vi, ti], axis=-1)


def signed_volume(pos: np.ndarray, vi: np.ndarray) -> float:
    """Divergence-theorem volume. Negative means the winding faces inward."""
    a, b, c = pos[vi[:, 0]], pos[vi[:, 1]], pos[vi[:, 2]]
    return float(np.einsum("ij,ij->i", a, np.cross(b, c)).sum() / 6.0)


def build_normals(pos: np.ndarray, vi: np.ndarray, smooth_degrees: float) -> np.ndarray:
    """Per-face-corner normals with angle-thresholded smoothing.

    Returns [F,3,3]. Face normals are area-weighted when accumulated (the cross
    product's magnitude is twice the triangle area), so a wall's large flat
    faces are not out-voted by a swarm of tiny bevel triangles.
    """
    a, b, c = pos[vi[:, 0]], pos[vi[:, 1]], pos[vi[:, 2]]
    face_area_normal = np.cross(b - a, c - a)          # unnormalised == weighted
    lengths = np.linalg.norm(face_area_normal, axis=1, keepdims=True)
    face_unit = np.divide(face_area_normal, lengths,
                          out=np.zeros_like(face_area_normal), where=lengths > 1e-20)

    # Weld by quantised position so that duplicated soup vertices at the same
    # point share one accumulator. 1e-4 tile units is well under any real
    # feature size but far above float noise.
    keys = np.round(pos * 1e4).astype(np.int64)
    _, weld = np.unique(keys, axis=0, return_inverse=True)

    accum = np.zeros((weld.max() + 1, 3), dtype=np.float64)
    for corner in range(3):
        np.add.at(accum, weld[vi[:, corner]], face_area_normal)
    acc_len = np.linalg.norm(accum, axis=1, keepdims=True)
    accum_unit = np.divide(accum, acc_len, out=np.zeros_like(accum), where=acc_len > 1e-20)

    cos_limit = math.cos(math.radians(smooth_degrees))
    out = np.empty((len(vi), 3, 3), dtype=np.float64)
    for corner in range(3):
        smoothed = accum_unit[weld[vi[:, corner]]]
        agree = np.einsum("ij,ij->i", smoothed, face_unit) >= cos_limit
        out[:, corner, :] = np.where(agree[:, None], smoothed, face_unit)

    # Degenerate triangles leave zero-length normals; fall back to +Y (up)
    # rather than emitting a normal the shader will divide by zero on.
    bad = np.linalg.norm(out, axis=2) < 1e-12
    out[bad] = (0.0, 1.0, 0.0)
    return out


def write_obj(path: Path, pos, uv, nrm, faces_vi, faces_ti, mtl_name: str | None):
    """Write an indexed OBJ, deduplicating identical (pos, uv, normal) corners."""
    corner_pos = pos[faces_vi].reshape(-1, 3)
    corner_uv = (uv[faces_ti].reshape(-1, 2) if len(uv)
                 else np.zeros((faces_vi.size, 2)))
    corner_nrm = nrm.reshape(-1, 3)

    # Quantise for dedup only; the written values stay full precision.
    key = np.concatenate([np.round(corner_pos, 6),
                          np.round(corner_uv, 6),
                          np.round(corner_nrm, 5)], axis=1)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    order = np.argsort(first)                 # keep original traversal order
    remap = np.empty(len(order), dtype=np.int64)
    remap[order] = np.arange(len(order))
    vert_index = remap[inverse].reshape(-1, 3)

    uniq_pos = corner_pos[first[order]]
    uniq_uv = corner_uv[first[order]]
    uniq_nrm = corner_nrm[first[order]]

    lines = [f"# converted from XCOM 2 SDK export by tools/xcom_convert.py",
             f"# {len(uniq_pos)} verts, {len(vert_index)} tris"]
    if mtl_name:
        lines.append(f"mtllib {mtl_name}.mtl")
    lines.append(f"o {path.stem}")
    lines += [f"v {p[0]:.6f} {p[1]:.6f} {p[2]:.6f}" for p in uniq_pos]
    lines += [f"vt {t[0]:.6f} {t[1]:.6f}" for t in uniq_uv]
    lines += [f"vn {n[0]:.6f} {n[1]:.6f} {n[2]:.6f}" for n in uniq_nrm]
    if mtl_name:
        lines.append(f"usemtl {mtl_name}")
    lines.append("s 1")
    lines += [f"f {a+1}/{a+1}/{a+1} {b+1}/{b+1}/{b+1} {c+1}/{c+1}/{c+1}"
              for a, b, c in vert_index]
    path.write_text("\n".join(lines) + "\n")
    return len(uniq_pos), len(vert_index)


def write_mtl(path: Path, name: str, diffuse: str | None, normal: str | None):
    lines = [f"# generated by tools/xcom_convert.py", f"newmtl {name}",
             "Ka 1.000 1.000 1.000", "Kd 1.000 1.000 1.000",
             "Ks 0.000 0.000 0.000", "d 1.0", "illum 2"]
    if diffuse:
        lines.append(f"map_Kd {diffuse}")
    if normal:
        # raylib's tinyobj reads both spellings; emit the common one.
        lines.append(f"map_Bump {normal}")
    path.write_text("\n".join(lines) + "\n")


# ---------------------------------------------------------------- conversions
def convert_mesh(src: Path, dst: Path, scale: float, smooth_degrees: float,
                 flip_v: bool, mtl_name: str | None, recenter: bool,
                 quiet: bool = False):
    pos, uv, faces = read_raw_obj(src)
    vi, ti = faces[:, :, 0], faces[:, :, 1]

    raw_min, raw_max = pos.min(axis=0), pos.max(axis=0)

    pos = pos / scale
    if recenter:
        # Centre on X/Z, keep Y=0 at the floor - that is how a tile-placed
        # prop wants its pivot.
        lo, hi = pos.min(axis=0), pos.max(axis=0)
        centre = (lo + hi) * 0.5
        pos = pos - np.array([centre[0], lo[1], centre[2]])

    volume = signed_volume(pos, vi)
    flipped = volume < 0.0
    if flipped:
        # Reverse winding so front faces are counter-clockwise (GL convention).
        vi = vi[:, ::-1].copy()
        ti = ti[:, ::-1].copy()

    if len(uv) and flip_v:
        uv = uv.copy()
        uv[:, 1] = 1.0 - uv[:, 1]

    nrm = build_normals(pos, vi, smooth_degrees)
    nverts, ntris = write_obj(dst, pos, uv, nrm, vi, ti, mtl_name)

    lo, hi = pos.min(axis=0), pos.max(axis=0)
    size = hi - lo
    if not quiet:
        print(f"  {src.name}")
        print(f"    -> {dst.name}: {nverts} verts, {ntris} tris"
              f"{' [winding reversed]' if flipped else ''}")
        print(f"       raw uu   {fmt3(raw_min)} .. {fmt3(raw_max)}  size {fmt3(raw_max - raw_min)}")
        print(f"       tiles    {fmt3(lo)} .. {fmt3(hi)}  size {fmt3(size)}"
              f"   (w x h x d = {size[0]:.3f} x {size[1]:.3f} x {size[2]:.3f})")
    else:
        # One line per mesh, and it doubles as the library index feed.
        print(f"MESH\t{dst.stem}\t{ntris}\t{size[0]:.3f}\t{size[1]:.3f}\t{size[2]:.3f}")
    return dst


def fmt3(v) -> str:
    return "(" + ", ".join(f"{x:.2f}" for x in v) + ")"


def convert_texture(src: Path, dst: Path, max_size: int | None, fast: bool = False):
    from PIL import Image

    with Image.open(src) as im:
        im.load()
        original = im.size
        mode = im.mode
        # A constant alpha channel carries no information, whatever its value.
        # Dropping it is not just a size win: XCOM packs masks into RGB and
        # leaves alpha at a flat 0, and resizing such an image as RGBA wipes
        # the RGB out entirely (see the channel-wise resize below).
        if mode == "RGBA":
            lo, hi = im.getchannel("A").getextrema()
            if lo == hi:
                im = im.convert("RGB")
                mode = f"RGB (alpha dropped, was constant {lo})"

        if max_size and max(im.size) > max_size:
            ratio = max_size / max(im.size)
            size = (max(1, round(im.width * ratio)), max(1, round(im.height * ratio)))
            # Resize each band on its own. Pillow's RGBA resize lets alpha bleed
            # into the colour channels, which darkens the colour behind
            # transparent texels - exactly the wrong thing for the alpha-cut
            # foliage cards this kit is full of.
            im = Image.merge(im.mode, [band.resize(size, Image.LANCZOS)
                                       for band in im.split()])
        # optimize=True costs several seconds on a 2048px image, which is fine
        # for a curated asset and ruinous across a whole-library sweep.
        im.save(dst, "PNG", optimize=not fast, compress_level=1 if fast else 6)

    kb = dst.stat().st_size / 1024
    note = "" if im.size == original else f"  (from {original[0]}x{original[1]})"
    print(f"  {src.name}\n    -> {dst.name}: {im.size[0]}x{im.size[1]} {mode}, "
          f"{kb:,.0f} KB{note}")


# ---------------------------------------------------------------------- entry
def parse_pairs(values):
    """`Src=Dst` or bare `Src` -> list of (src, dst)."""
    out = []
    for v in values or []:
        src, _, dst = v.partition("=")
        out.append((src, dst or src))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw", required=True, type=Path,
                    help="directory produced by xcom_extract.ps1 (has mesh/ and tex/)")
    ap.add_argument("--out", required=True, type=Path, help="asset output directory")
    ap.add_argument("--mesh", action="append", metavar="SRC[=DST]",
                    help="mesh to convert; repeatable. Default: all.")
    ap.add_argument("--tex", action="append", metavar="SRC[=DST]",
                    help="texture to convert; repeatable. Default: all.")
    ap.add_argument("--scale", type=float, default=UNREAL_UNITS_PER_TILE,
                    help=f"unreal units per output unit (default {UNREAL_UNITS_PER_TILE:g}"
                         " = one XCOM tile)")
    ap.add_argument("--smooth-deg", type=float, default=DEFAULT_SMOOTH_DEGREES,
                    help="normal smoothing angle threshold")
    ap.add_argument("--flip-v", action="store_true",
                    help="flip the V texture coordinate (OBJ bottom-left origin "
                         "vs GL top-left texture upload)")
    ap.add_argument("--recenter", action="store_true",
                    help="centre the pivot horizontally, keeping Y=0 at the base")
    ap.add_argument("--max-size", type=int, default=None,
                    help="downscale textures so the long edge is at most this")
    # Without these, an empty --mesh/--tex list means "convert everything found",
    # which is the right default for exploring a package but silently wrong for a
    # texture-only or mesh-only entry in a curated manifest.
    ap.add_argument("--no-meshes", action="store_true",
                    help="convert no meshes at all (not the same as omitting --mesh)")
    ap.add_argument("--no-textures", action="store_true",
                    help="convert no textures at all (not the same as omitting --tex)")
    ap.add_argument("--fast-png", action="store_true",
                    help="skip PNG optimisation - much faster, larger files")
    ap.add_argument("--quiet", action="store_true",
                    help="one tab-separated MESH line per mesh instead of the "
                         "full report; used to build the library index")
    ap.add_argument("--material", default=None,
                    help="name of the .mtl/material to emit and reference")
    ap.add_argument("--diffuse", default=None, help="PNG filename for map_Kd")
    ap.add_argument("--normal", default=None, help="PNG filename for map_Bump")
    args = ap.parse_args(argv)

    raw_mesh, raw_tex = args.raw / "mesh", args.raw / "tex"
    args.out.mkdir(parents=True, exist_ok=True)

    mesh_pairs = parse_pairs(args.mesh)
    if not mesh_pairs and not args.no_meshes and raw_mesh.is_dir():
        mesh_pairs = [(p.stem, p.stem) for p in sorted(raw_mesh.glob("*.OBJ"))
                      if not p.stem.endswith(("_UV1", "_Internal"))]
    tex_pairs = parse_pairs(args.tex)
    if not tex_pairs and not args.no_textures and raw_tex.is_dir():
        tex_pairs = [(p.stem, p.stem) for p in sorted(raw_tex.glob("*.TGA"))]
    if args.no_meshes:
        mesh_pairs = []
    if args.no_textures:
        tex_pairs = []

    if tex_pairs:
        if not args.quiet:
            print("Textures:")
        for src, dst in tex_pairs:
            candidates = [raw_tex / f"{src}.TGA", raw_tex / f"{src}.tga"]
            found = next((c for c in candidates if c.exists()), None)
            if not found:
                print(f"  !! {src}: not found in {raw_tex}", file=sys.stderr)
                continue
            # Keep going across a bad file: in a library-wide sweep one
            # unreadable texture must not take the whole package down.
            try:
                convert_texture(found, args.out / f"{dst}.png", args.max_size,
                                args.fast_png)
            except Exception as exc:
                print(f"  !! {src}: {exc}", file=sys.stderr)

    if args.material:
        write_mtl(args.out / f"{args.material}.mtl", args.material,
                  args.diffuse, args.normal)
        print(f"Material: {args.material}.mtl"
              f" (map_Kd={args.diffuse or '-'}, map_Bump={args.normal or '-'})")

    if mesh_pairs:
        if not args.quiet:
            print("Meshes:")
        for src, dst in mesh_pairs:
            candidates = [raw_mesh / f"{src}.OBJ", raw_mesh / f"{src}.obj"]
            found = next((c for c in candidates if c.exists()), None)
            if not found:
                print(f"  !! {src}: not found in {raw_mesh}", file=sys.stderr)
                continue
            try:
                convert_mesh(found, args.out / f"{dst}.obj", args.scale,
                             args.smooth_deg, args.flip_v, args.material,
                             args.recenter, args.quiet)
            except Exception as exc:
                print(f"  !! {src}: {exc}", file=sys.stderr)

    if not args.quiet:
        print(f"\nWrote to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
