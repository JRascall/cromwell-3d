"""Browse the extracted asset libraries: search, preview meshes, inspect textures.

One window over every library the extraction tools have produced -- XCOM 2,
Helldivers 2, Rainbow Six: Siege, UNIGINE -- because the useful question is
usually "what does this look like" and the answer currently requires finding a
file by hand and opening it in something else.

Two things it does that a general image viewer does not:

*Renders a mesh without a GPU.* A numpy software rasteriser, same approach as
xcom_parcel_render.py. Flat, normal-shaded, UV-checker and wireframe modes; no
lighting model worth the name, which is the point -- it is for reading shape and
topology, not for looking pretty.

*Splits a texture into channels.* This matters more than the composite view.
These engines pack unrelated data per channel: XCOM's MSK carries alpha cutout in
BLUE only, and Siege's specular map "usually holds gloss, metalness and cavity".
Viewing such a texture as RGB shows a meaningless colour; viewing R, G, B, A
separately shows what is actually stored.

Materials are shown beside a mesh where the library records them. XCOM resolves
them properly (xcom_materials.py wrote materials.csv). Siege cannot: it ships no
asset names and no material links, so its meshes and textures are browsable but
not pairable -- see study/rainbow_six_formats.md.

Headless use, which is also how the rendering is tested:

    py -3 tools/asset_browser.py --list
    py -3 tools/asset_browser.py --find "clubhouse" --kind texture
    py -3 tools/asset_browser.py --render <mesh.obj> --out preview.png
    py -3 tools/asset_browser.py --sheet siege:mesh --out sheet.png -n 24

With no arguments it opens the GUI. Needs Python 3 with numpy and Pillow; tkinter
ships with Python. Nothing else to install.
"""
import argparse
import csv
import os
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parent.parent

# Where each library landed. Siege is the odd one out: 139 GB does not fit on the
# repo's drive, so it is passed with -Out and lives elsewhere. Missing libraries
# are skipped silently -- not everyone has run every extractor.
LIBRARIES = {
    'xcom':    REPO / 'xcom_extracted' / 'models',
    'hd2':     REPO / 'hd2_extracted',
    'unigine': REPO / 'unigine_extracted',
    'siege':   Path(os.environ.get('R6_LIBRARY', r'D:\r6_extracted')),
}

MESH_EXT = {'.obj', '.glb'}
TEX_EXT = {'.png', '.dds', '.tga', '.jpg', '.jpeg', '.bmp'}

# Cataloguing Siege alone means walking ~940,000 files, so the result is cached.
CACHE = REPO / 'workbench' / 'asset_catalog.tsv'


# --------------------------------------------------------------------------
# catalogue
# --------------------------------------------------------------------------

# Catalogue columns. Kept as a tuple rather than a dataclass because there are
# half a million of them and the whole thing is held in memory.
LIB, KIND, ROLE, MAT, NAME, PATH = range(6)


def _kind_of(lib, path):
    """Coarse bucket for filtering. Siege sorts itself by directory because the
    sweep writes one directory per asset kind; elsewhere the extension is all
    there is to go on."""
    ext = path.suffix.lower()
    if ext in MESH_EXT:
        # filediver writes one .glb per Helldivers *material* as well as per unit,
        # and a material export is a flat two-triangle quad carrying the material.
        # There are 5,587 of them against 7,541 real units, so leaving them in the
        # mesh bucket means 43% of "HD2 models" are planes on the floor.
        if lib == 'hd2' and 'materials' in {p.lower() for p in path.parts}:
            return 'material'
        return 'mesh'
    if ext not in TEX_EXT:
        return 'other'
    parts = {p.lower() for p in path.parts}
    if 'guitexture' in parts or 'gui' in parts:
        return 'guitexture'
    return 'texture'


def _role_of(lib, name):
    """Which map a texture is, read off the filename.

    Both libraries that survived with any naming encode the role: Siege's dumps
    carry the texture's own type enum (`_typeDiffuse`, `_typeNormal`), and XCOM's
    art uses Firaxis' `_DIF`/`_NRM`/`_MSK` suffixes. That makes "show me every
    normal map" answerable without opening 267,000 files.
    """
    n = name.lower()
    if 'typenormal' in n or '_nrm' in n or '_n.' in n or 'normal' in n:
        return 'normal'
    if 'typespecular' in n or '_spc' in n or 'specular' in n or '_rgh' in n:
        return 'specular'
    if 'typediffuse' in n or '_dif' in n or 'diffuse' in n or 'albedo' in n:
        return 'diffuse'
    if 'typemask' in n or '_msk' in n or 'mask' in n:
        return 'mask'
    if 'typemisc' in n or 'misc' in n:
        return 'misc'
    return 'other'


def _material_of(lib, kind, path, xcom_index):
    """Which maps a mesh actually has, as a filterable string.

    Three genuinely different situations, and collapsing them to a boolean would
    mislead: XCOM resolves real links, Helldivers carries its textures inside the
    .glb, and Siege has neither names nor links so nothing can ever be paired.

    For XCOM the value is the map set itself -- 'dif+nrm+msk' -- so the filter
    answers "which meshes have a spec map" directly rather than only "does this
    have textures at all".
    """
    if kind not in ('mesh', 'material'):
        return ''
    if lib == 'xcom':
        return xcom_index.get((path.parent.name, path.stem), 'none')
    if lib == 'hd2':
        return 'embedded'
    return 'none'


# XCOM 2 ships no roughness, emissive or AO maps: that data is packed into the
# MSK channels and the diffuse alpha. Measured over 40 files of each kind, so
# these are observations about the shipped art rather than a decoded shader.
CHANNEL_NOTES = {
    ('xcom', 'mask'): ('R varies (mean 69) - spec/roughness-like | G bright (mean 202) '
                       '| B alpha cutout, foliage only - flat in 60% of files '
                       '| A always varies (mean 72)'),
    ('xcom', 'diffuse'): 'A is packed data, not opacity - never flat, mean 126',
    ('siege', 'normal'): 'BC5 two-channel: X in R, Y in G, B unused - use the Z button',
    ('siege', 'specular'): 'packs gloss / metalness / cavity across channels',
}


def build_catalog(verbose=True):
    # Loaded once here rather than per file: it is the only cheap way to know
    # whether an XCOM mesh resolves to textures.
    xcom_index = {}
    lib_root = LIBRARIES['xcom']
    mats_csv = lib_root / 'materials.csv'
    if mats_csv.exists():
        with mats_csv.open(encoding='utf-8') as f:
            for r in csv.DictReader(f):
                if not r.get('resolved'):
                    continue
                have = [tag for slot, tag in (('diffuse', 'dif'), ('normal', 'nrm'),
                                              ('mask', 'msk'))
                        if r.get(slot) and (lib_root / r[slot]).exists()]
                # materials.csv has no specular column -- XCOM ships very few, and
                # they sit beside the diffuse under the same stem.
                if r.get('diffuse') and _xcom_spec_path(lib_root / r['diffuse']):
                    have.append('spc')
                xcom_index[(r['package'], r['mesh'])] = '+'.join(have) if have else 'none'

    rows = []
    for lib, root in LIBRARIES.items():
        if not root.exists():
            if verbose:
                print(f'  {lib:8s} -- not present, skipped')
            continue
        t0 = time.time()
        n = 0
        for dirpath, _dirnames, filenames in os.walk(root):
            d = Path(dirpath)
            for fn in filenames:
                ext = os.path.splitext(fn)[1].lower()
                if ext not in MESH_EXT and ext not in TEX_EXT:
                    continue
                p = d / fn
                kind = _kind_of(lib, p)
                role = _role_of(lib, fn) if kind in ('texture', 'guitexture') else ''
                rows.append((lib, kind, role, _material_of(lib, kind, p, xcom_index),
                             fn, str(p)))
                n += 1
        if verbose:
            print(f'  {lib:8s} {n:9,d} files  ({time.time() - t0:.0f}s)')
    return rows


def load_catalog(refresh=False, verbose=True):
    if CACHE.exists() and not refresh:
        with CACHE.open(encoding='utf-8', newline='') as f:
            return [tuple(r) for r in csv.reader(f, delimiter='\t')]
    if verbose:
        print('cataloguing (first run; cached afterwards)...')
    rows = build_catalog(verbose)
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    with CACHE.open('w', encoding='utf-8', newline='') as f:
        csv.writer(f, delimiter='\t').writerows(rows)
    if verbose:
        print(f'cached {len(rows):,} entries -> {CACHE}')
    return rows


def search(rows, query='', lib=None, kind=None, role=None, material=None, limit=None):
    """Filter the catalogue. Every criterion is AND-ed; None means "don't care"."""
    q = query.lower()
    out = []
    for r in rows:
        if lib and r[LIB] != lib:
            continue
        if kind and r[KIND] != kind:
            continue
        if role and r[ROLE] != role:
            continue
        if material and r[MAT] != material:
            continue
        if q and q not in r[NAME].lower() and q not in r[PATH].lower():
            continue
        out.append(r)
        if limit and len(out) >= limit:
            break
    return out


# --------------------------------------------------------------------------
# XCOM material links -- the only library that records them
# --------------------------------------------------------------------------

_XCOM_MATS = None


def _xcom_spec_path(diffuse_path):
    """A specular map sitting beside the diffuse, if one exists.

    materials.csv records diffuse/normal/mask only, because that is what the
    material graph names. XCOM's ~148 spec maps follow the art naming instead:
    same stem as the diffuse with the suffix swapped.
    """
    p = Path(diffuse_path)
    stem = p.stem
    for dif_tag in ('_DIF', '_DIFF', '_dif', '_diff'):
        if stem.endswith(dif_tag):
            base = stem[: -len(dif_tag)]
            for spec_tag in ('_SPC', '_SPEC', '_spc', '_spec'):
                cand = p.with_name(base + spec_tag + p.suffix)
                if cand.exists():
                    return str(cand)
            break
    return None


def xcom_material(mesh_path):
    """(diffuse, normal, mask) absolute paths for an XCOM mesh, or None.

    Never guess a texture from the mesh's own folder: about a third of packages
    ship no textures and paint from a shared library, which is exactly why
    materials.csv exists.
    """
    global _XCOM_MATS
    lib = LIBRARIES['xcom']
    csv_path = lib / 'materials.csv'
    if not csv_path.exists():
        return None
    if _XCOM_MATS is None:
        _XCOM_MATS = {}
        with csv_path.open(encoding='utf-8') as f:
            for r in csv.DictReader(f):
                if r.get('resolved'):
                    _XCOM_MATS[(r['package'], r['mesh'])] = r
    p = Path(mesh_path)
    row = _XCOM_MATS.get((p.parent.name, p.stem))
    if not row:
        return None
    out = {}
    for slot in ('diffuse', 'normal', 'mask'):
        rel = row.get(slot)
        out[slot] = str(lib / rel) if rel and (lib / rel).exists() else None
    out['specular'] = _xcom_spec_path(out['diffuse']) if out['diffuse'] else None
    # XCOM ships no emissive *file*. Self-illumination is the MSK's ALPHA channel:
    # where it is bright the diffuse goes neutral so the glow colour reads through
    # (measured on AbstractSculpture: RGB 138,138,138 under the mask against
    # 133,33,34 outside it). So the emissive slot points at the mask and the
    # renderer takes its alpha.
    out['emissive'] = out['mask'] if out['mask'] and xcom_has_emissive(out['mask']) else None
    return out


_EMISSIVE_CACHE = {}


def xcom_has_emissive(mask_path):
    """True when a MSK's alpha looks like a glow mask rather than flat filler.

    Cached per file: masks are shared across a package's meshes, so the same few
    hundred images would otherwise be reopened thousands of times.
    """
    key = str(mask_path)
    if key in _EMISSIVE_CACHE:
        return _EMISSIVE_CACHE[key]
    ok = False
    try:
        im = Image.open(key)
        im.load()
        if 'A' in im.getbands():
            a = np.asarray(im.getchannel('A'), np.float32)
            # A real mask is mostly dark with a bright patch; flat or nearly-flat
            # alpha is filler and would light the whole model.
            ok = bool(a.std() > 8 and (a > 200).mean() > 0.002 and (a < 50).mean() > 0.25)
    except Exception:
        ok = False
    _EMISSIVE_CACHE[key] = ok
    return ok


# --------------------------------------------------------------------------
# mesh loading and software rendering
# --------------------------------------------------------------------------

def load_obj(path):
    """Positions, UVs, normals and triangulated faces. Faces are (n,3,3) of
    vertex/uv/normal indices, matching xcom_parcel_render.py."""
    V, T, N, F = [], [], [], []
    with open(path, 'r', errors='replace') as fh:
        for line in fh:
            tok, _, rest = line.partition(' ')
            if tok == 'v':
                V.append([float(x) for x in rest.split()[:3]])
            elif tok == 'vt':
                T.append([float(x) for x in rest.split()[:2]])
            elif tok == 'vn':
                N.append([float(x) for x in rest.split()[:3]])
            elif tok == 'f':
                c = [[int(b) - 1 if b else 0 for b in t.split('/')] for t in rest.split()]
                for k in range(1, len(c) - 1):
                    F.append([c[0], c[k], c[k + 1]])
    faces = np.array(F, np.int32) if F else np.zeros((0, 3, 3), np.int32)
    return (np.array(V, np.float32),
            np.array(T or [[0, 0]], np.float32),
            np.array(N or [[0, 1, 0]], np.float32),
            faces,
            np.zeros(len(faces), np.int32))   # OBJ: one material


_GLTF_COMPONENT = {5120: np.int8, 5121: np.uint8, 5122: np.int16,
                   5123: np.uint16, 5125: np.uint32, 5126: np.float32}
_GLTF_COUNT = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}


def load_glb(path):
    """Positions/UVs/normals/faces out of a binary glTF.

    Deliberately minimal: Helldivers' filediver output is one .glb per unit with
    everything embedded, and all this viewer needs is the geometry. No scene
    graph, no node transforms, no skinning -- every primitive in the file is
    merged into one soup, which is right for "what shape is this" and wrong for
    anything else.
    """
    import json
    import struct

    raw = Path(path).read_bytes()
    if raw[:4] != b'glTF':
        raise ValueError('not a binary glTF')

    js, bin_chunk = None, b''
    off = 12
    while off + 8 <= len(raw):
        clen, ctype = struct.unpack_from('<II', raw, off)
        data = raw[off + 8: off + 8 + clen]
        if ctype == 0x4E4F534A:          # 'JSON'
            js = json.loads(data.decode('utf-8', 'replace'))
        elif ctype == 0x004E4942:        # 'BIN'
            bin_chunk = data
        off += 8 + clen + (-clen % 4)

    if not js:
        raise ValueError('no JSON chunk')

    views = js.get('bufferViews', [])
    accessors = js.get('accessors', [])

    def read(idx):
        acc = accessors[idx]
        view = views[acc['bufferView']]
        start = view.get('byteOffset', 0) + acc.get('byteOffset', 0)
        count = acc['count']
        ncomp = _GLTF_COUNT[acc['type']]
        dt = np.dtype(_GLTF_COMPONENT[acc['componentType']])
        elem = dt.itemsize * ncomp
        stride = view.get('byteStride')

        # Vertex attributes are frequently interleaved, and reading them as if
        # they were contiguous silently walks into the neighbouring attribute --
        # which looks like valid geometry with a few absurd coordinates in it,
        # not like a parse failure.
        if stride and stride != elem:
            raw_len = stride * (count - 1) + elem
            block = np.frombuffer(bin_chunk, dtype=np.uint8, count=raw_len, offset=start)
            picks = (np.arange(count)[:, None] * stride + np.arange(elem)[None, :])
            packed = block[picks].reshape(-1).tobytes()
            return np.frombuffer(packed, dtype=dt, count=count * ncomp).reshape(count, ncomp)

        return np.frombuffer(bin_chunk, dtype=dt, count=count * ncomp,
                             offset=start).reshape(count, ncomp)

    V, T, N, F, M = [], [], [], [], []
    base = 0
    for m in js.get('meshes', []):
        for prim in m.get('primitives', []):
            attrs = prim.get('attributes', {})
            if 'POSITION' not in attrs or 'indices' not in prim:
                continue
            mat_id = prim.get('material', -1)
            pos = read(attrs['POSITION']).astype(np.float32)
            idx = read(prim['indices']).astype(np.int64).ravel()
            nrm = read(attrs['NORMAL']).astype(np.float32) if 'NORMAL' in attrs else None
            uv = read(attrs['TEXCOORD_0']).astype(np.float32) if 'TEXCOORD_0' in attrs else None

            V.append(pos)
            N.append(nrm if nrm is not None else np.zeros_like(pos))
            T.append(uv if uv is not None else np.zeros((len(pos), 2), np.float32))
            tri = idx[: len(idx) // 3 * 3].reshape(-1, 3) + base
            F.append(np.stack([tri, tri, tri], axis=2))
            M.append(np.full(len(tri), mat_id, np.int32))
            base += len(pos)

    if not V:
        return (np.zeros((0, 3), np.float32), np.zeros((1, 2), np.float32),
                np.zeros((1, 3), np.float32), np.zeros((0, 3, 3), np.int32),
                np.zeros((0,), np.int32))

    return (np.concatenate(V), np.concatenate(T), np.concatenate(N),
            np.concatenate(F).astype(np.int32), np.concatenate(M))


def load_mesh(path):
    """Dispatch on extension so callers do not care which library it came from."""
    return load_glb(path) if str(path).lower().endswith('.glb') else load_obj(path)


# glTF PBR slot -> the name this tool uses, so HD2 lines up with the other
# libraries' vocabulary instead of exposing glTF's.
_GLTF_SLOTS = (
    ('baseColorTexture', 'diffuse'),
    ('normalTexture', 'normal'),
    ('metallicRoughnessTexture', 'specular'),   # G=roughness, B=metallic
    ('emissiveTexture', 'emissive'),
    ('occlusionTexture', 'occlusion'),
)


def glb_materials(path):
    """[{slot: PIL.Image}] per glTF material, decoded from inside the .glb.

    Helldivers' exports carry their textures in the binary chunk rather than
    beside the file, so a viewer that only reads geometry shows grey models --
    the textures are right there, just never opened. Images may be PNG or DDS
    (filediver writes MSFT_texture_dds); Pillow reads both from bytes.

    Returned per material because a unit can carry dozens, and the renderer picks
    per triangle.
    """
    import io
    import json
    import struct

    raw = Path(path).read_bytes()
    if raw[:4] != b'glTF':
        return []

    js, blob = None, b''
    off = 12
    while off + 8 <= len(raw):
        clen, ctype = struct.unpack_from('<II', raw, off)
        data = raw[off + 8: off + 8 + clen]
        if ctype == 0x4E4F534A:
            js = json.loads(data.decode('utf-8', 'replace'))
        elif ctype == 0x004E4942:
            blob = data
        off += 8 + clen + (-clen % 4)
    if not js:
        return []

    views = js.get('bufferViews', [])
    images = js.get('images', [])
    textures = js.get('textures', [])
    cache = {}

    def image(idx):
        """Decode an image by index, cached -- materials share textures heavily."""
        if idx in cache:
            return cache[idx]
        img = None
        try:
            src = images[idx]
            if 'bufferView' in src:
                v = views[src['bufferView']]
                start = v.get('byteOffset', 0)
                img = Image.open(io.BytesIO(blob[start: start + v['byteLength']]))
                img.load()
        except Exception:
            img = None
        cache[idx] = img
        return img

    def tex_image(info):
        """A textureInfo -> its image, following the DDS extension when present."""
        if not info or 'index' not in info:
            return None
        t = textures[info['index']]
        src = t.get('source')
        if src is None:
            src = t.get('extensions', {}).get('MSFT_texture_dds', {}).get('source')
        return image(src) if src is not None else None

    out = []
    used = set()
    for mat in js.get('materials', []):
        pbr = mat.get('pbrMetallicRoughness', {})
        slots = {}
        for gltf_name, our_name in _GLTF_SLOTS:
            info = pbr.get(gltf_name) if gltf_name in pbr else mat.get(gltf_name)
            if info and 'index' in info:
                t = textures[info['index']]
                src = t.get('source')
                if src is None:
                    src = t.get('extensions', {}).get('MSFT_texture_dds', {}).get('source')
                if src is not None:
                    used.add(src)
            im = tex_image(info)
            if im is not None:
                slots[our_name] = im
        out.append(slots)

    # 77% of the images Helldivers embeds are referenced by no PBR slot at all --
    # filediver puts them in the file but only wires up some of them, which is why
    # most units render grey despite carrying their textures. They are still the
    # asset's real maps, so hand them back tagged with a guess from their pixels
    # rather than dropping them.
    extras = []
    for idx in range(len(images)):
        if idx in used:
            continue
        im = image(idx)
        if im is not None:
            extras.append((images[idx].get('name') or f'image{idx}', guess_map_role(im), im))
    return out, extras


def guess_map_role(im):
    """Label an unassigned texture from its pixels: 'albedo', 'normal' or 'mask'.

    A guess, and named as one. Normal maps sit near 127 in R and G with little
    spread; albedo carries real saturation; anything flat and grey is a mask or a
    packed data channel.
    """
    small = im.convert('RGB')
    small.thumbnail((64, 64))
    a = np.asarray(small, np.float32)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    sat = float((a.max(2) - a.min(2)).mean())
    if abs(r.mean() - 127.5) < 32 and abs(g.mean() - 127.5) < 32 and sat < 60:
        return 'normal?'
    if sat > 28:
        return 'albedo?'
    return 'mask?'



LIGHT_DIR = np.array([0.35, 0.60, -0.72], np.float32)   # camera sits on -z


def _uv_row(v, height, flip_v=True):
    """Texture row for a V coordinate.

    Which way up V runs is genuinely ambiguous for this library. OBJ puts the
    origin bottom-left, Unreal puts it top-left, and xcom_convert.py's --flip-v is
    opt-in, so the exported files may carry either. xcom_parcel_render.py assumes
    the flip, which is why that is the default here.

    It is not a cosmetic choice: XCOM's emissive mask sits in a band at the very
    bottom of the MSK, and under one convention a given mesh reaches it while
    under the other it does not. Hence the toggle rather than a silent guess.
    """
    vv = v % 1.0
    return np.clip((1.0 - vv if flip_v else vv) * (height - 1), 0, height - 1).astype(np.int32)


def _view_rotation(yaw, pitch):
    ry, rx = np.radians(yaw), np.radians(pitch)
    cy, sy = np.cos(ry), np.sin(ry)
    cx, sx = np.cos(rx), np.sin(rx)
    return np.array([[cy, 0, -sy], [sx * sy, cx, sx * cy], [cx * sy, -sx, cx * cy]], np.float32)


def render_points(mesh, size=512, yaw=35.0, pitch=20.0, zoom=1.0, pan=(0.0, 0.0),
                  texture=None, budget=500000, seed=0):
    """Surface splat, for use while the camera is moving.

    The triangle rasteriser is a Python loop over faces and costs roughly 60us per
    face whatever the resolution, so it cannot be turned down into an interactive
    renderer -- 1,500 triangles is already only 11 fps. This draws the same
    surface by scattering samples across triangle *interiors* instead, entirely in
    numpy.

    Sampling interiors rather than vertices is the whole point. Splatting vertices
    alone leaves the faces empty, so the model visibly loses its surfaces the
    moment the camera moves and reassembles when it stops. Samples are allocated
    in proportion to each triangle's screen area, so a large flat face gets enough
    of them to read as solid and a distant sliver does not waste any.
    """
    V, T, N, F = mesh[0], mesh[1], mesh[2], mesh[3]
    img = np.zeros((size, size, 3), np.float32)
    if len(V) == 0 or len(F) == 0:
        return Image.fromarray((img * 255).astype(np.uint8))

    p = V[F[:, :, 0]] @ _view_rotation(yaw, pitch).T
    flat = p.reshape(-1, 3)
    lo, hi = flat.min(0), flat.max(0)
    centre = (lo + hi) / 2.0
    scale = float((hi - lo)[:2].max()) or 1.0
    m = size * 0.44 * float(zoom)

    sx = (p[:, :, 0] - centre[0]) / scale * 2 * m + size / 2 + pan[0] * zoom
    sy = size / 2 - (p[:, :, 1] - centre[1]) / scale * 2 * m + pan[1] * zoom
    sz = p[:, :, 2]

    # Samples proportional to screen area, so coverage is even across the model.
    area = 0.5 * np.abs((sx[:, 1] - sx[:, 0]) * (sy[:, 2] - sy[:, 0]) -
                        (sx[:, 2] - sx[:, 0]) * (sy[:, 1] - sy[:, 0]))
    n = np.clip(np.ceil(area * 1.6), 1, 4096).astype(np.int64)
    total = int(n.sum())
    if total > budget:                       # keep the frame affordable
        n = np.maximum(1, (n * (budget / total)).astype(np.int64))
        total = int(n.sum())

    tri = np.repeat(np.arange(len(F), dtype=np.int64), n)
    rng = np.random.default_rng(seed)        # fixed seed: no shimmer between frames
    u = rng.random(total, dtype=np.float32)
    v = rng.random(total, dtype=np.float32)
    over = u + v > 1.0
    u[over], v[over] = 1.0 - u[over], 1.0 - v[over]

    ax, ay, az = sx[tri, 0], sy[tri, 0], sz[tri, 0]
    ux, uy, uz = sx[tri, 1] - ax, sy[tri, 1] - ay, sz[tri, 1] - az
    vx, vy, vz = sx[tri, 2] - ax, sy[tri, 2] - ay, sz[tri, 2] - az
    px = (ax + u * ux + v * vx).astype(np.int32)
    py = (ay + u * uy + v * vy).astype(np.int32)
    pz = az + u * uz + v * vz

    keep = (px >= 0) & (px < size) & (py >= 0) & (py < size)
    if not keep.any():
        return Image.fromarray((img * 255).astype(np.uint8))
    px, py, pz, tri = px[keep], py[keep], pz[keep], tri[keep]
    u, v = u[keep], v[keep]

    e1 = p[:, 1] - p[:, 0]
    e2 = p[:, 2] - p[:, 0]
    fn = np.cross(e1, e2)
    fn /= np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-9)
    shade = 0.18 + 0.82 * np.abs(fn @ LIGHT_DIR)

    if texture is not None and len(T) > 1:
        tex = np.asarray(texture.convert('RGB'), np.float32) / 255.0
        uv = T[F[:, :, 1]]
        au, av = uv[tri, 0, 0], uv[tri, 0, 1]
        tu = au + u * (uv[tri, 1, 0] - au) + v * (uv[tri, 2, 0] - au)
        tv = av + u * (uv[tri, 1, 1] - av) + v * (uv[tri, 2, 1] - av)
        th, tw = tex.shape[:2]
        ti = np.clip((1.0 - (tv % 1.0)) * (th - 1), 0, th - 1).astype(np.int32)
        tj = np.clip((tu % 1.0) * (tw - 1), 0, tw - 1).astype(np.int32)
        colour = tex[ti, tj] * (0.55 + 0.45 * shade[tri])[:, None]
    else:
        colour = np.repeat(shade[tri][:, None], 3, axis=1)

    # Depth resolve without a Python loop: find the nearest z per pixel, then keep
    # only the samples that achieved it. Smaller z is nearer, as in render_mesh.
    flat_idx = py.astype(np.int64) * size + px
    zbuf = np.full(size * size, np.inf, np.float32)
    np.minimum.at(zbuf, flat_idx, pz)
    win = pz <= zbuf[flat_idx]
    out = img.reshape(-1, 3)
    out[flat_idx[win]] = colour[win]
    return Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))


def render_mesh(mesh, size=512, yaw=35.0, pitch=20.0, mode='flat',
                texture=None, max_tris=60000, zoom=1.0, pan=(0.0, 0.0),
                spec_texture=None, emis_texture=None, flip_v=True, tex_by_mat=None):
    """Rasterise a mesh to a PIL image. No GPU, no lighting rig.

    Orthographic on purpose: it keeps proportions readable and makes two meshes
    visually comparable, which perspective does not.
    """
    V, T, N, F = mesh[0], mesh[1], mesh[2], mesh[3]
    tri_mat = mesh[4] if len(mesh) > 4 else np.zeros(len(F), np.int32)
    img = np.zeros((size, size, 3), np.float32)
    if len(V) == 0 or len(F) == 0:
        return Image.fromarray((img * 255).astype(np.uint8))

    # Big meshes are subsampled rather than refused: a partial silhouette still
    # answers "what is this", and the alternative is a viewer that stalls.
    if len(F) > max_tris:
        keep_idx = np.linspace(0, len(F) - 1, max_tris).astype(np.int32)
        F = F[keep_idx]
        tri_mat = tri_mat[keep_idx]

    p = V[F[:, :, 0]]                                    # (n,3,3) triangle corners

    ry, rx = np.radians(yaw), np.radians(pitch)
    cy, sy = np.cos(ry), np.sin(ry)
    cx, sx = np.cos(rx), np.sin(rx)
    rot = np.array([[cy, 0, -sy], [sx * sy, cx, sx * cy], [cx * sy, -sx, cx * cy]], np.float32)
    p = p @ rot.T

    lo, hi = p.reshape(-1, 3).min(0), p.reshape(-1, 3).max(0)
    centre = (lo + hi) / 2.0
    scale = float((hi - lo)[:2].max())
    if scale <= 0:
        scale = 1.0
    # Zoom and pan are applied after the fit-to-frame, which keeps "framed by
    # default" true for every mesh regardless of its authored scale.
    #
    # Pan is held in zoom-1 pixels and scaled by zoom here, rather than being a
    # raw screen offset. A raw offset does not shrink when you zoom out, so a pan
    # set while zoomed in walks the model off the canvas as you pull back and the
    # mesh appears to vanish.
    m = size * 0.44 * float(zoom)
    sx_ = (p[:, :, 0] - centre[0]) / scale * 2 * m + size / 2 + pan[0] * zoom
    sy_ = size / 2 - (p[:, :, 1] - centre[1]) / scale * 2 * m + pan[1] * zoom
    depth = p[:, :, 2]

    # Geometric normals: the file's own normals may be absent or wrong, and the
    # face normal is what the flat shading wants anyway.
    e1 = p[:, 1] - p[:, 0]
    e2 = p[:, 2] - p[:, 0]
    fn = np.cross(e1, e2)
    ln = np.linalg.norm(fn, axis=1, keepdims=True)
    fn = fn / np.where(ln == 0, 1, ln)

    if mode == 'normals':
        col = (fn * 0.5 + 0.5)
    else:
        # abs(): shade two-sided. Winding is consistent in most of these exports
        # but not all, and a flipped triangle should read as lit rather than as a
        # black hole in the surface.
        lam = np.abs(fn @ LIGHT_DIR)
        col = np.repeat((0.18 + 0.82 * lam)[:, None], 3, axis=1)

    uv = None
    spec = None
    emis = None
    # A Helldivers unit carries its textures inside the .glb and can use several
    # materials across one mesh, so the texture is chosen per triangle rather than
    # per model. Arrays are prepared once here; the loop only indexes them.
    mat_tex = None
    if tex_by_mat and mode in ('textured', 'lit') and len(T) > 1:
        uv = T[F[:, :, 1]]
        mat_tex = {k: np.asarray(v.convert('RGB'), np.float32) / 255.0
                   for k, v in tex_by_mat.items() if v is not None}
    elif mode in ('textured', 'lit', 'emissive') and texture is not None and len(T) > 1:
        uv = T[F[:, :, 1]]
        tex = np.asarray(texture.convert('RGB'), np.float32) / 255.0
        # 'lit' adds a highlight driven by the spec map, which is the only way to
        # see what that map does to the surface rather than just what it looks
        # like flat. Face normals are flat-shaded, so the highlight is faceted --
        # it shows where the map puts gloss, not what the shipped shader draws.
        if mode == 'lit' and spec_texture is not None:
            sp = np.asarray(spec_texture.convert('L'), np.float32) / 255.0
            spec = sp
        if mode in ('lit', 'emissive') and emis_texture is not None:
            em = emis_texture
            emis = np.asarray(em.getchannel('A') if 'A' in em.getbands()
                              else em.convert('L'), np.float32) / 255.0

    # SMALLER z is nearer: the camera sits on -z looking toward +z, so the visible
    # surface of a closed mesh is the one whose face normals have fn.z < 0.
    #
    # Verified rather than assumed, because getting it backwards does not produce
    # an obviously broken image -- it quietly draws the far side, and only shows
    # up as interior faces winning a few percent of pixels. Measured as the share
    # of pixels whose winning triangle faces the camera: this way round gives
    # 100% / 100% / 99.7% on three test meshes, the other way 91.5% / 100% / 70%.
    zbuf = np.full((size, size), np.inf, np.float32)
    order = np.argsort(-depth.mean(1))                   # far to near

    for i in order:
        x0, x1 = sx_[i], sy_[i]
        minx = max(int(np.floor(x0.min())), 0)
        maxx = min(int(np.ceil(x0.max())) + 1, size)
        miny = max(int(np.floor(x1.min())), 0)
        maxy = min(int(np.ceil(x1.max())) + 1, size)
        if minx >= maxx or miny >= maxy:
            continue

        xs = np.arange(minx, maxx, dtype=np.float32)
        ys = np.arange(miny, maxy, dtype=np.float32)
        gx, gy = np.meshgrid(xs, ys)

        ax, ay = x0[0], x1[0]
        bx, by = x0[1], x1[1]
        cx2, cy2 = x0[2], x1[2]
        den = (by - cy2) * (ax - cx2) + (cx2 - bx) * (ay - cy2)
        if abs(den) < 1e-9:
            continue
        w0 = ((by - cy2) * (gx - cx2) + (cx2 - bx) * (gy - cy2)) / den
        w1 = ((cy2 - ay) * (gx - cx2) + (ax - cx2) * (gy - cy2)) / den
        w2 = 1.0 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        # A triangle smaller than a pixel contains no pixel centre, so the
        # barycentric test rejects every sample and the face contributes nothing.
        # Zoomed out far enough that is every face at once and the mesh vanishes
        # rather than becoming small, so such a face is written at its centroid.
        #
        # This has to hang off "no sample was inside" and NOT off "no sample won
        # the depth test". The two look interchangeable and are not: a fully
        # occluded triangle also has nothing to write, and giving that one a
        # centroid pixel punches interior and back faces straight through the
        # surface in front of them. That is a depth bug that only shows on the
        # settled render, because the interactive preview never takes this path.
        if not inside.any():
            cxp = int(round(float(x0.mean())))
            cyp = int(round(float(x1.mean())))
            if 0 <= cxp < size and 0 <= cyp < size:
                zc = float(depth[i].mean())
                if zc < zbuf[cyp, cxp]:
                    zbuf[cyp, cxp] = zc
                    img[cyp, cxp] = col[i]
            continue

        z = w0 * depth[i, 0] + w1 * depth[i, 1] + w2 * depth[i, 2]
        sub = zbuf[miny:maxy, minx:maxx]
        write = inside & (z < sub)
        if not write.any():
            continue                      # occluded: draw nothing
        sub[write] = z[write]

        if uv is not None:
            u = w0 * uv[i, 0, 0] + w1 * uv[i, 1, 0] + w2 * uv[i, 2, 0]
            v = w0 * uv[i, 0, 1] + w1 * uv[i, 1, 1] + w2 * uv[i, 2, 1]
            tex_i = tex if mat_tex is None else mat_tex.get(int(tri_mat[i]))
            if tex_i is None:
                px = np.broadcast_to(col[i], (maxy - miny, maxx - minx, 3))
                tile = img[miny:maxy, minx:maxx]
                tile[write] = px[write]
                continue
            tex = tex_i
            tw, th = tex.shape[1], tex.shape[0]
            ui = np.clip((u % 1.0) * (tw - 1), 0, tw - 1).astype(np.int32)
            vi = _uv_row(v, th, flip_v)
            shade = abs(float(fn[i] @ LIGHT_DIR))
            px = tex[vi, ui] * (0.55 + 0.45 * shade)
            if emis is not None:
                eh, ew = emis.shape[:2]
                ei = _uv_row(v, eh, flip_v)
                ej = np.clip((u % 1.0) * (ew - 1), 0, ew - 1).astype(np.int32)
                glow = emis[ei, ej][..., None] * tex[vi, ui]
                # Emissive is unlit by definition: add it on top of the shaded
                # albedo rather than letting the lambert term dim it.
                px = glow * 1.7 if mode == 'emissive' else px + glow * 1.4
            if spec is not None:
                sh, sw = spec.shape[:2]
                si = _uv_row(v, sh, flip_v)
                sj = np.clip((u % 1.0) * (sw - 1), 0, sw - 1).astype(np.int32)
                px = px + (spec[si, sj] * (shade ** 16) * 1.6)[..., None]
        else:
            px = np.broadcast_to(col[i], (maxy - miny, maxx - minx, 3))

        tile = img[miny:maxy, minx:maxx]
        tile[write] = px[write] if px.ndim == 3 else px
        img[miny:maxy, minx:maxx] = tile

    return Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))


# --------------------------------------------------------------------------
# textures
# --------------------------------------------------------------------------

def load_texture(path):
    im = Image.open(path)
    im.load()
    return im


def is_two_channel_normal(im):
    """True for a BC5-style normal map: X and Y in R and G, blue unused.

    Worth detecting rather than assuming. Siege's normal maps come out with B
    flat at zero and R/G averaging ~127, because Z is meant to be rebuilt in the
    shader; displaying such a texture as RGB shows a flat red-green field that
    looks broken but is not.
    """
    if 'B' not in im.getbands():
        return False
    b = np.asarray(im.getchannel('B'), np.float32)
    r = np.asarray(im.getchannel('R'), np.float32)
    g = np.asarray(im.getchannel('G'), np.float32)
    return b.std() < 1.0 and abs(r.mean() - 127.5) < 40 and abs(g.mean() - 127.5) < 40


def reconstruct_normal_z(im):
    """Rebuild Z from X and Y and return a conventional tangent-space normal map,
    which is what the surface actually looks like."""
    a = np.asarray(im.convert('RGB'), np.float32) / 255.0
    x = a[..., 0] * 2.0 - 1.0
    y = a[..., 1] * 2.0 - 1.0
    z = np.sqrt(np.clip(1.0 - x * x - y * y, 0.0, 1.0))
    out = np.stack([x, y, z], -1) * 0.5 + 0.5
    return Image.fromarray((out * 255).astype(np.uint8))


def channel_view(im, channel):
    """One channel as greyscale. The whole reason this tool exists for textures:
    packed maps are meaningless as RGB and legible per channel."""
    if channel == 'rgb':
        return im.convert('RGB')
    if channel == 'z':
        return reconstruct_normal_z(im)
    bands = im.getbands()
    if channel.upper() not in bands:
        return Image.new('L', im.size, 0)
    return im.getchannel(channel.upper())


def texture_info(im, path):
    size_kb = os.path.getsize(path) / 1024.0
    return f'{im.width}x{im.height}  {im.mode}  {im.format or "?"}  {size_kb:,.0f} KB'


# --------------------------------------------------------------------------
# headless entry points
# --------------------------------------------------------------------------

def cmd_sheet(rows, spec, out, n, size):
    """Contact sheet of the first n matches. The fastest way to see whether a
    library extracted sanely, and it needs no GUI."""
    lib, _, kind = spec.partition(':')
    sel = search(rows, '', lib or None, kind or None, limit=n)
    if not sel:
        print(f'nothing matches {spec}')
        return
    cols = int(np.ceil(np.sqrt(len(sel))))
    rows_n = int(np.ceil(len(sel) / cols))
    sheet = Image.new('RGB', (cols * size, rows_n * size), (16, 16, 20))
    for i, r in enumerate(sel):
        try:
            if r[KIND] == 'mesh':
                tile = render_mesh(load_mesh(r[PATH]), size=size)
            else:
                tile = load_texture(r[PATH]).convert('RGB')
                tile.thumbnail((size, size))
        except Exception as e:
            print(f'  !! {r[NAME]}: {e}')
            continue
        sheet.paste(tile, ((i % cols) * size, (i // cols) * size))
    sheet.save(out)
    print(f'{len(sel)} tiles -> {out}')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--list', action='store_true', help='per-library counts')
    ap.add_argument('--find')
    ap.add_argument('--lib', choices=sorted(LIBRARIES))
    ap.add_argument('--kind', choices=['mesh', 'material', 'texture', 'guitexture', 'other'])
    ap.add_argument('--role', choices=['diffuse', 'normal', 'specular', 'mask', 'misc', 'other'],
                    help='texture role, read off the filename')
    ap.add_argument('--material', choices=['linked', 'embedded', 'none'],
                    help='meshes: whether textures can be found for them')
    ap.add_argument('--render', help='mesh path -> png')
    ap.add_argument('--sheet', help='contact sheet, e.g. "siege:mesh"')
    ap.add_argument('--out', default='preview.png')
    ap.add_argument('-n', type=int, default=16)
    ap.add_argument('--size', type=int, default=256)
    ap.add_argument('--yaw', type=float, default=35.0)
    ap.add_argument('--pitch', type=float, default=20.0)
    ap.add_argument('--mode', default='flat', choices=['flat', 'normals', 'textured'])
    ap.add_argument('--refresh', action='store_true', help='rebuild the catalogue')
    a = ap.parse_args()

    if a.render:
        mesh = load_mesh(a.render)
        tex = None
        if a.mode == 'textured':
            mats = xcom_material(a.render)
            if mats and mats.get('diffuse'):
                tex = load_texture(mats['diffuse'])
        img = render_mesh(mesh, size=max(a.size, 512), yaw=a.yaw, pitch=a.pitch,
                          mode=a.mode, texture=tex)
        img.save(a.out)
        print(f'{len(mesh[0])} verts, {len(mesh[3])} tris -> {a.out}')
        return

    rows = load_catalog(a.refresh)

    if a.list:
        import collections
        c = collections.Counter((r[LIB], r[KIND], r[ROLE], r[MAT]) for r in rows)
        print(f'{"library":9s} {"kind":11s} {"role":9s} {"material":9s} {"files":>10s}')
        for (lib, kind, role, mat), n in sorted(c.items()):
            print(f'{lib:9s} {kind:11s} {role or "-":9s} {mat or "-":9s} {n:10,d}')
        print(f'\n{len(rows):,} assets across {len({r[0] for r in rows})} libraries')
        return

    if a.sheet:
        cmd_sheet(rows, a.sheet, a.out, a.n, a.size)
        return

    if a.find is not None:
        kw = dict(lib=a.lib, kind=a.kind, role=a.role, material=a.material)
        sel = search(rows, a.find, limit=a.n, **kw)
        print(f'{len(sel)} shown of {len(search(rows, a.find, **kw)):,} matches')
        for r in sel:
            print(f'  {r[LIB]:8s} {r[KIND]:10s} {r[ROLE] or "-":9s} '
                  f'{r[MAT] or "-":9s} {r[NAME][:52]:52s} {r[PATH]}')
        return

    from asset_browser_ui import run_gui
    run_gui(rows)


if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    main()
