"""xcom_parcel_render.py - assemble a parcel and render it to a PNG.

    py -3 tools/xcom2/xcom_parcel_render.py md_Forest_01

This is a REFERENCE IMPLEMENTATION, not a tool you need in the engine. It is a
plain software rasteriser whose only job is to prove the extracted data is
self-consistent, and - more usefully - to show in ~200 readable lines exactly
how to consume the three CSVs from your own renderer. Port the two functions
below (`place` and `get_tex`); ignore the rasteriser.

Inputs, all produced by the other tools in this folder:

    xcom_extracted/parcels/<Parcel>/placements.csv   one row per placed prop
    xcom_extracted/models/<Package>/<Mesh>.obj       Y-up, 1 unit = 1 tile
    xcom_extracted/models/materials.csv              mesh -> diffuse/normal/mask

--- 1. TRANSFORMS (see `place`) -------------------------------------------

Meshes are already in OUR space (Y-up, divided by 96) but placement data is
Unreal's (Z-up, unreal units). Rather than reason about the handedness flip,
each vertex goes BACK to unreal space, is transformed there exactly as the
engine would, then comes forward again:

    ours -> unreal : (x, y, z) -> (x, z, y)
    scale          : componentwise DrawScale3D
    yaw            : rotate about unreal Z, degrees = yaw_deg
    translate      : Location / 96
    unreal -> ours : (X, Y, Z) -> (X, Z, Y)

Two traps:
  * A NEGATIVE DrawScale3D component is a MIRROR, not a rotation - XCOM reuses
    one mesh for both handednesses. Keep the sign or corner pieces face the
    wrong way, and note a mirror FLIPS TRIANGLE WINDING, so those instances
    need reversed cull order.
  * Rotation in the raw T3D is UE angle units (65536 == 360 deg); the CSV has
    already converted it to degrees.

--- 2. MATERIALS (see `get_tex`) ------------------------------------------

Do NOT guess a texture from the mesh's own folder. Roughly a third of packages
carry no textures at all and paint from a shared library - DirtPileDeco uses
TextureLibrary_ClimateZones, TreeStumpCover uses Foliage_Temperate. Guessing
gets ~71% of a parcel textured; materials.csv gets ~97%.

Alpha cutout is the MSK texture's BLUE channel, and only for foliage. Applying
it to solid props punches holes in them (a jersey barrier's diffuse alpha is
packed data, not opacity).
"""
import csv, math, re, sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent.parent
LIB = REPO / "xcom_extracted" / "models"
W, H = 1500, 950
FOLIAGE = re.compile(r'bush|foliage|scatter|grass|tree|plant|vine|cattail|fern|weed', re.I)

_mesh_cache, _tex_cache = {}, {}


def load_obj(p):
    V, T, N, F = [], [], [], []
    for line in p.read_text(errors="replace").splitlines():
        t, _, r = line.partition(" ")
        if t == "v":    V.append([float(x) for x in r.split()[:3]])
        elif t == "vt": T.append([float(x) for x in r.split()[:2]])
        elif t == "vn": N.append([float(x) for x in r.split()[:3]])
        elif t == "f":
            c = [[int(b) - 1 if b else 0 for b in tok.split("/")] for tok in r.split()]
            for k in range(1, len(c) - 1):
                F.append([c[0], c[k], c[k + 1]])
    return (np.array(V, np.float64), np.array(T or [[0, 0]], np.float64),
            np.array(N or [[0, 1, 0]], np.float64), np.array(F))


def get_mesh(pkg, mesh):
    key = (pkg, mesh)
    if key in _mesh_cache: return _mesh_cache[key]
    p = LIB / pkg / f"{mesh}.obj"
    if not p.exists():
        hits = list(LIB.glob(f"*/{mesh}.obj"))
        p = hits[0] if hits else None
    _mesh_cache[key] = load_obj(p) if p else None
    return _mesh_cache[key]


# mesh -> its resolved textures, from the material pass. This is the whole
# point of xcom_materials.py: cross-package references (DirtPileDeco painting
# with TextureLibrary_ClimateZones) cannot be guessed from folder contents.
_MATS = {}
for _r in csv.DictReader((LIB / 'materials.csv').open(encoding='utf-8')):
    if _r['resolved']:
        _MATS[(_r['package'], _r['mesh'])] = _r


def get_tex(pkg, mesh):
    key = (pkg, mesh)
    if key in _tex_cache: return _tex_cache[key]
    r = _MATS.get(key)
    tex = mask = None
    if r:
        dif = LIB / r['diffuse']
        if dif.exists():
            tex = np.asarray(Image.open(dif).convert("RGB"), np.float32) / 255.0
        # Alpha cutout lives in the MSK's BLUE channel, but only foliage is
        # actually masked - using it on solid props punches holes in them.
        if r['mask'] and FOLIAGE.search(pkg):
            m = LIB / r['mask']
            if m.exists():
                mask = np.asarray(Image.open(m).convert("RGB"), np.float32)[..., 2] / 255.0
    _tex_cache[key] = (tex, mask)
    return _tex_cache[key]


def place(V, row):
    u = V[:, [0, 2, 1]].copy()                               # ours -> unreal
    u *= np.array([float(row['scale_x']), float(row['scale_z']), float(row['scale_y'])])
    a = math.radians(float(row['yaw_deg']))
    ca, sa = math.cos(a), math.sin(a)
    x, y = u[:, 0].copy(), u[:, 1].copy()
    u[:, 0] = x * ca - y * sa
    u[:, 1] = x * sa + y * ca
    u += np.array([float(row['uu_x']), float(row['uu_z']), float(row['uu_y'])]) / 96.0
    return u[:, [0, 2, 1]]                                   # unreal -> ours


def main():
    csv_path = REPO / "xcom_extracted" / "parcels" / sys.argv[1] / "placements.csv"
    rows = list(csv.DictReader(csv_path.open(encoding="utf-8")))

    tris = []            # (verts[3,3], uv[3,2], normal[3,3], pkg, mirrored)
    for r in rows:
        m = get_mesh(r['mesh_package'], r['mesh'])
        if m is None: continue
        V, T, N, F = m
        P = place(V, r)
        mir = r['mirrored'] == 'yes'
        vi, ti, ni = F[:, :, 0], F[:, :, 1], F[:, :, 2]
        tris.append((P[vi], T[ti], N[ni], r['mesh_package'], r['mesh']))
    total = sum(t[0].shape[0] for t in tris)
    print(f"assembled {len(tris)} props, {total:,} triangles")

    allv = np.concatenate([t[0].reshape(-1, 3) for t in tris])
    lo, hi = allv.min(0), allv.max(0)
    ctr = (lo + hi) / 2
    rad = np.linalg.norm(hi - lo) / 2
    print(f"extent {lo.round(1)} .. {hi.round(1)} tiles")

    eye = ctr + np.array([0.75, 0.62, 0.95]) * rad
    f = ctr - eye; f /= np.linalg.norm(f)
    s = np.cross(f, [0, 1, 0]); s /= np.linalg.norm(s)
    up = np.cross(s, f)
    view = np.eye(4); view[0, :3], view[1, :3], view[2, :3] = s, up, -f
    view[:3, 3] = -view[:3, :3] @ eye
    fs = 1 / math.tan(math.radians(42) / 2); near, far = 0.05, 4000.0
    P4 = np.zeros((4, 4)); P4[0, 0] = fs * H / W; P4[1, 1] = fs
    P4[2, 2] = (far + near) / (near - far); P4[2, 3] = 2 * far * near / (near - far); P4[3, 2] = -1

    color = np.full((H, W, 3), 0.07, np.float32)
    depth = np.full((H, W), np.inf)
    light = np.array([.42, .82, .38]); light /= np.linalg.norm(light)

    for verts, uvs, nrms, pkg, meshname in tris:
        tex, mask = get_tex(pkg, meshname)
        n = verts.shape[0]
        flat = verts.reshape(-1, 3)
        clip = (np.c_[flat, np.ones(len(flat))] @ view.T) @ P4.T
        wv = np.where(np.abs(clip[:, 3]) < 1e-9, 1e-9, clip[:, 3])
        ndc = clip[:, :3] / wv[:, None]
        sx = ((ndc[:, 0] * .5 + .5) * W).reshape(n, 3)
        sy = ((1 - (ndc[:, 1] * .5 + .5)) * H).reshape(n, 3)
        iw = (1 / wv).reshape(n, 3)
        okall = (clip[:, 3] > 1e-6).reshape(n, 3).all(1)

        for i in range(n):
            if not okall[i]: continue
            x, y = sx[i], sy[i]
            area = (x[1]-x[0])*(y[2]-y[0]) - (x[2]-x[0])*(y[1]-y[0])
            if abs(area) < 1e-9: continue
            x0, x1 = max(int(x.min()), 0), min(int(x.max()) + 2, W)
            y0, y1 = max(int(y.min()), 0), min(int(y.max()) + 2, H)
            if x0 >= x1 or y0 >= y1: continue
            px, py = np.meshgrid(np.arange(x0, x1) + .5, np.arange(y0, y1) + .5)
            w0 = ((x[1]-x[0])*(py-y[0]) - (px-x[0])*(y[1]-y[0])) / area
            w1 = ((px-x[0])*(y[2]-y[0]) - (x[2]-x[0])*(py-y[0])) / area
            w2 = 1 - w0 - w1
            m = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not m.any(): continue
            b = np.stack([w2, w1, w0], -1)
            iwp = b @ iw[i]
            z = 1 / iwp
            pb = (b * iw[i]) / iwp[..., None]
            uv = pb @ uvs[i]
            u = np.clip(uv[..., 0] % 1., 0, 1); v = np.clip(uv[..., 1] % 1., 0, 1)
            if tex is not None:
                th, tw = tex.shape[:2]
                texel = tex[((1-v)*(th-1)).astype(int), (u*(tw-1)).astype(int)]
            else:
                texel = np.full(u.shape + (3,), 0.55, np.float32)
            if mask is not None:
                mh, mw = mask.shape
                m = m & (mask[((1-v)*(mh-1)).astype(int), (u*(mw-1)).astype(int)] > 0.4)
                if not m.any(): continue
            nn = pb @ nrms[i]
            nn /= np.maximum(np.linalg.norm(nn, axis=-1, keepdims=True), 1e-9)
            lam = np.abs(nn @ light) if mask is not None else np.clip(nn @ light, 0, 1)
            sub = depth[y0:y1, x0:x1]
            better = m & (z < sub)
            if not better.any(): continue
            depth[y0:y1, x0:x1] = np.where(better, z, sub)
            color[y0:y1, x0:x1] = np.where(better[..., None],
                                           texel * (.3 + .85*lam)[..., None],
                                           color[y0:y1, x0:x1])

    img = Image.fromarray((np.clip(color, 0, 1)*255).astype(np.uint8))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W-1, 20], fill=(0, 0, 0))
    d.text((6, 5), f"{sys.argv[1]}  -  {len(tris)} props, {total:,} tris, "
                   f"{hi[0]-lo[0]:.0f} x {hi[2]-lo[2]:.0f} tiles", fill=(255, 220, 60))
    out = REPO / "workbench" / f"parcel_{sys.argv[1]}.png"
    out.parent.mkdir(parents=True, exist_ok=True); img.save(out); print("wrote", out)


if __name__ == "__main__":
    main()
