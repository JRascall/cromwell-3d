"""Browse the extracted asset libraries: search, preview meshes, inspect textures.

One window over every library the extraction tools have produced -- XCOM 2,
Helldivers 2, Rainbow Six: Siege, UNIGINE -- because the useful question is
usually "what does this look like" and the answer currently requires finding a
file by hand and opening it in something else.

Three things it does that a general image viewer does not:

*Renders a mesh, with or without a GPU.* The fallback is a numpy software
rasteriser, same approach as xcom_parcel_render.py: flat, normal-shaded,
UV-checker and wireframe modes, no lighting model worth the name, which is the
point -- it is for reading shape and topology, not for looking pretty. When
moderngl is installed the same picture is drawn on the GPU instead, about 50x
faster (measured: 3.9 ms against 202 ms for a 3,216-triangle character), which
is what makes orbiting and animation playback keep their texture and detail
instead of dropping to scattered samples.

*Plays animation.* Any rigged .glb gets a clip list beside the preview: click one
and it plays, with a repeat toggle. Clips come from inside the file and from the
standalone .anim library beside it, bound to the skeleton by joint name -- which
is what lets all 88 of Mercenaries' human models play the same 1,639 clips
rather than 85 of them showing an empty panel. The list has a filter over it,
because 1,639 rows is not a list anyone reads.

*Splits a texture into channels.* This matters more than the composite view.
These engines pack unrelated data per channel: XCOM's MSK carries alpha cutout in
BLUE only, and Siege's specular map "usually holds gloss, metalness and cavity".
Viewing such a texture as RGB shows a meaningless colour; viewing R, G, B, A
separately shows what is actually stored.

Materials are shown beside a mesh where the library records them. XCOM resolves
them properly (xcom_materials.py wrote materials.csv). Siege cannot: it ships no
asset names and no material links, so its meshes and textures are browsable but
not pairable -- see study/games/shooters/rainbow_six_formats.md.

Headless use, which is also how the rendering is tested:

    py -3 tools/asset_browser/asset_browser.py --list
    py -3 tools/asset_browser/asset_browser.py --find "clubhouse" --kind texture
    py -3 tools/asset_browser/asset_browser.py --render <mesh.obj> --out preview.png
    py -3 tools/asset_browser/asset_browser.py --sheet siege:mesh --out sheet.png -n 24

With no arguments it opens the GUI. Needs Python 3 with numpy and Pillow; tkinter
ships with Python. `pip install moderngl` is optional and only buys speed -- the
tool runs without it, on the numpy renderer, and says which one it is using
under the preview.
"""
import argparse
import csv
import os
import struct
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parent.parent.parent

# Where each library landed. Siege is the odd one out: 139 GB does not fit on the
# repo's drive, so it is passed with -Out and lives elsewhere. Missing libraries
# are skipped silently -- not everyone has run every extractor.
LIBRARIES = {
    'xcom':    REPO / 'xcom_extracted' / 'models',
    'hd2':     REPO / 'hd2_extracted',
    'unigine': REPO / 'unigine_extracted',
    'siege':   Path(os.environ.get('R6_LIBRARY', r'D:\r6_extracted')),
    'mercs':   REPO / 'mercs_extracted',
    # Broken Arrow, same situation as Siege: the sweep plus the Unity project it
    # goes through is far too large for the repo's drive, so it lives elsewhere
    # and is passed in.
    'ba':      Path(os.environ.get('BA_LIBRARY', r'D:\ba_extracted')),
}

# Directories inside a library that are working state, not content. Broken
# Arrow's tree holds an AssetRipper project and a Unity project beside its
# output, and both contain thousands of meshes and textures that are *copies*
# of what is already catalogued from the sweep - cataloguing them means every
# search returns each asset three times.
SCRATCH_DIRS = {'_tools', '_ripper_units', '_bake', '_probe', '_lists', '_done'}

MESH_EXT = {'.obj', '.glb'}
# Everything the mesh viewer can open. Split out because a composed world and a
# terrain are meshes but wanting to FIND one is a different question from
# wanting to find a prop - and mercs_extracted holds two files called
# aclubs.obj, one the terrain and one the whole level.
MESH_KINDS = {'mesh', 'scene', 'terrain'}
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
        # A composed level and a bare heightmap are both .obj and both often
        # carry the level's name, so the directory is the only thing that tells
        # them apart. Without this the browser lists two identical "aclubs.obj".
        if lib == 'mercs':
            parts = {p.lower() for p in path.parts}
            if 'scenes' in parts:
                return 'scene'
            if 'terrain' in parts:
                return 'terrain'
        # filediver writes one .glb per Helldivers *material* as well as per unit,
        # and a material export is a flat two-triangle quad carrying the material.
        # There are 5,587 of them against 7,541 real units, so leaving them in the
        # mesh bucket means 43% of "HD2 models" are planes on the floor.
        if lib == 'hd2' and 'materials' in {p.lower() for p in path.parts}:
            return 'material'
        return 'mesh'
    if lib == 'ba' and ext == '.png':
        # Broken Arrow's textures arrive from two passes at different
        # granularity: the sweep's texture/ tree keeps the game's own
        # addressable paths, and model/ holds the per-rig maps AssetStudio
        # wrote beside each FBX. Same pixels, different organisation, and both
        # are worth having - the second is how you find "the maps for THIS
        # soldier" without knowing the container path.
        return 'texture'
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
    # Mercenaries encodes nothing in its filenames because there is nothing to
    # encode: every one of its 4,643 textures is a palettised diffuse map, with
    # no normal, specular or mask maps in the game at all. Matching on
    # substrings here would scatter them across roles on the accident of a name
    # containing "mask" or "misc", and leave a role=diffuse filter returning
    # none of them.
    if lib == 'mercs':
        return 'diffuse'
    # Broken Arrow is Unity URP/HDRP, so the suffixes are the pipeline's own and
    # unambiguous. Checked before the generic rules because "MaskMap" would
    # otherwise fall through to the substring soup below and land as 'mask'
    # only by luck, while "BaseMap" matches none of them at all.
    if lib == 'ba':
        n = name.lower()
        if 'normal' in n or '_nrm' in n:
            return 'normal'
        if 'maskmap' in n or '_mask' in n:
            return 'mask'
        if 'basemap' in n or 'albedo' in n or 'basecolor' in n or '_diff' in n:
            return 'diffuse'
        if 'emissive' in n or 'emission' in n:
            return 'emissive'
        return 'other'
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
    if kind not in MESH_KINDS and kind != 'material':
        return ''
    if lib == 'xcom':
        return xcom_index.get((path.parent.name, path.stem), 'none')
    if lib == 'hd2':
        return 'embedded'
    if lib == 'mercs':
        # PS2 art is diffuse-only, so the value is 'dif' or nothing - there is
        # no map set to describe. A mesh with no textured segment (collision
        # and shadow proxies) honestly has none.
        #
        # The .glb from mercs_gltf.py name their textures by relative URI rather
        # than embedding them, which resolves the same way for the viewer - so
        # they are 'dif' too. Reporting 'none' made every rigged character open
        # untextured, since the default view mode is chosen from this value.
        return 'dif'
    if lib == 'ba':
        # The .glb are the rigged units - geometry, skin and every animation
        # clip - and their textures resolve per material through
        # ba_materials.csv, so they are the ones worth opening textured. The
        # .obj from the bundle sweep are raw individual meshes with no material
        # link of any kind, and saying otherwise would open them in diffuse
        # mode showing nothing.
        return 'dif+msk+nrm' if path.suffix.lower() == '.glb' else 'none'
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
        for dirpath, dirnames, filenames in os.walk(root):
            # Prune working state in place, so os.walk never descends into it.
            dirnames[:] = [x for x in dirnames if x not in SCRATCH_DIRS]
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


# --------------------------------------------------------------------------
# Mercenaries material links
# --------------------------------------------------------------------------

_MERCS_MATS = None


def mercs_materials(mesh_path):
    """Per-material textures for a Mercenaries mesh, shaped like glb_materials.

    Returned as a list indexed by the mesh's material number, so the renderer's
    tex_by_mat can select per triangle. That matters more here than anywhere
    else in this browser: a PS2 vehicle is dozens of segments over a handful of
    textures, and painting one texture across the whole mesh would show the
    fuselage sheet stretched over the windows and rotor blur.

    Only 'diffuse' is ever populated, and that is the whole truth rather than a
    limitation of the extractor - the art is palettised diffuse maps, with no
    normal, specular or mask maps anywhere in the game's 4,643 textures.
    """
    global _MERCS_MATS
    lib = LIBRARIES['mercs']
    csv_path = lib / 'materials.csv'
    if not csv_path.exists():
        return []
    if _MERCS_MATS is None:
        _MERCS_MATS = {}
        with csv_path.open(encoding='utf-8') as f:
            for r in csv.DictReader(f):
                _MERCS_MATS[(r['package'], r['mesh'])] = r
    p = Path(mesh_path)
    row = _MERCS_MATS.get((p.parent.name, p.stem))
    if not row:
        return []
    # Drive the order off the .obj itself rather than the csv: load_obj numbers
    # materials by first `usemtl`, and the two must not be able to drift.
    names = obj_material_names(mesh_path)
    by_name = dict(zip([n for n in row.get('texture_names', '').split(';') if n],
                       [t for t in row.get('textures', '').split(';')]))
    tex_dir = lib / 'textures_png'
    out = []
    for n in names:
        png = by_name.get(n)
        im = None
        if png and (tex_dir / png).exists():
            try:
                im = load_texture(str(tex_dir / png))
            except Exception:
                im = None
        out.append({'diffuse': im} if im is not None else {})
    return out


def _in_library(path, lib):
    """Is this path inside the named library's root?

    Compared as resolved paths rather than by substring: the library roots are
    configurable, so 'ba_extracted' in the string is a guess that breaks the
    moment someone points BA_LIBRARY somewhere else.
    """
    root = LIBRARIES.get(lib)
    if root is None:
        return False
    try:
        Path(path).resolve().relative_to(root.resolve())
        return True
    except (ValueError, OSError):
        return False


_BA_MATS = None      # material name -> {albedo, mask, normal} png filenames
_BA_TEX = None       # texture stem -> full path, over the whole sweep


def _ba_texture_index():
    """Every extracted Broken Arrow texture, keyed by the name a material uses.

    The join needs one adjustment. ba_materials.csv names textures the way the
    Unity project does - `ranger_low_Ranger_BaseMap.png` - while the sweep
    writes them as `ranger_low_Ranger_BaseMap @-4133...png`, because names alone
    collide constantly and the PathID suffix is what makes the file count add
    up. So the index is keyed on the part before the ' @'.
    """
    global _BA_TEX
    if _BA_TEX is not None:
        return _BA_TEX
    _BA_TEX = {}
    lib = LIBRARIES['ba']
    for sub in ('texture', 'model'):
        root = lib / sub
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [x for x in dirnames if x not in SCRATCH_DIRS]
            for fn in filenames:
                if not fn.lower().endswith('.png'):
                    continue
                stem = fn[:-4].split(' @')[0]
                # First writer wins. The same texture appears under both trees;
                # they are the same pixels, so which one is found is immaterial.
                _BA_TEX.setdefault(stem, os.path.join(dirpath, fn))
    return _BA_TEX


def ba_materials(mesh_path):
    """Per-material textures for a Broken Arrow .glb, shaped like glb_materials.

    The .glb carry no pixels on purpose - the rigs share texture sets heavily
    and embedding would multiply the library for no new information, see
    tools/ba/ba_glb.py. What they do carry is the material NAME, which is the
    key into ba_materials.csv, which was built by following the material
    assets' texture GUIDs rather than by matching names. That distinction
    matters here: `VDV.mat`, `VDV_Razvedka.mat` and `RU_VDVSpetsnaz.mat` are
    three different soldiers whose names are substrings of one another.

    Indexed by the mesh's material number so the renderer can pick per triangle,
    which a unit needs - a rifleman is body, head, webbing and weapon over
    separate maps.
    """
    global _BA_MATS
    lib = LIBRARIES['ba']
    csv_path = lib / 'ba_materials.csv'
    if not csv_path.exists():
        return []
    if _BA_MATS is None:
        _BA_MATS = {}
        with csv_path.open(encoding='utf-8') as f:
            for r in csv.DictReader(f):
                _BA_MATS[r['material']] = r

    names = glb_material_names(mesh_path)
    if not names:
        return []
    index = _ba_texture_index()
    out = []
    for n in names:
        # Blender's FBX round-trip can suffix a material that collided on
        # import; the base name is what the csv knows.
        row = _BA_MATS.get(n) or _BA_MATS.get(n.rsplit('.', 1)[0])
        slots = {}
        if row:
            for col, slot in (('albedo', 'diffuse'), ('mask', 'mask'),
                              ('normal', 'normal')):
                fn = row.get(col)
                if not fn:
                    continue
                path = index.get(fn[:-4] if fn.lower().endswith('.png') else fn)
                if path:
                    try:
                        slots[slot] = load_texture(path)
                    except Exception:
                        pass
        out.append(slots)
    return out


def glb_material_names(path):
    """Material names from a .glb, in the file's own material order."""
    try:
        js, _read = _glb_chunks(path)
    except Exception:
        return []
    return [m.get('name', '') for m in js.get('materials', [])]


def obj_mtl_materials(mesh_path):
    """Textures from the .mtl beside an .obj, in the file's own usemtl order.

    The plain .obj/.mtl route, which is what a composed scene uses and what any
    exporter writes. It is tried before the per-library routes because it is
    self-describing: the file says which texture each material uses and where it
    lives, so nothing has to know which game the mesh came from.
    """
    p = Path(mesh_path)
    if p.suffix.lower() != '.obj':
        return []
    mtl = None
    try:
        with p.open('r', errors='replace') as f:
            for line in f:
                if line.startswith('mtllib '):
                    mtl = p.with_name(line[7:].strip())
                    break
                if line.startswith(('v ', 'f ')):
                    break            # past the header; there is no mtllib
    except OSError:
        return []
    if mtl is None:
        mtl = p.with_suffix('.mtl')
    if not mtl.exists():
        return []

    maps, cur = {}, None
    with mtl.open('r', errors='replace') as f:
        for line in f:
            if line.startswith('newmtl '):
                cur = line[7:].strip()
            elif line.startswith('map_Kd ') and cur:
                maps[cur] = line[7:].strip()
    if not maps:
        return []

    cache, out = {}, []
    for name in obj_material_names(mesh_path):
        rel = maps.get(name)
        im = None
        if rel:
            cand = mtl.parent / rel
            key = str(cand)
            if key in cache:
                im = cache[key]
            elif cand.exists():
                try:
                    im = load_texture(str(cand))
                except Exception:
                    im = None
                cache[key] = im
        out.append({'diffuse': im} if im is not None else {})
    return out


def mesh_materials(path):
    """(materials, extra_images) for any mesh the browser can texture.

    One entry point so the UI does not have to know which library a file came
    from: .glb carries its materials inside it, a plain .obj declares them in an
    .mtl, and Mercenaries' per-model exports also resolve through materials.csv.
    """
    sp = str(path)
    if sp.lower().endswith('.glb'):
        # Broken Arrow's .glb name their materials but ship no pixels, so the
        # generic glTF route returns a list of empty slots - which is not
        # nothing, and would win over the csv lookup if tried first.
        if _in_library(sp, 'ba'):
            mats = ba_materials(path)
            if any(m for m in mats):
                return mats, []
        return glb_materials(path)
    mats = obj_mtl_materials(path)
    if any(m.get('diffuse') is not None for m in mats):
        return mats, []
    if 'mercs_extracted' in sp.replace('/', os.sep):
        return mercs_materials(path), []
    return [], []


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

def obj_material_names(path):
    """The `usemtl` names an .obj declares, in first-use order.

    That order is the material INDEX the renderer selects on, so it has to match
    load_obj's numbering exactly - hence one shared rule rather than two.
    """
    order, seen = [], set()
    try:
        with open(path, 'r', errors='replace') as fh:
            for line in fh:
                if line.startswith('usemtl '):
                    name = line[7:].strip()
                    if name and name not in seen:
                        seen.add(name)
                        order.append(name)
    except OSError:
        return []
    return order


def load_obj(path):
    """Positions, UVs, normals and triangulated faces. Faces are (n,3,3) of
    vertex/uv/normal indices, matching xcom_parcel_render.py.

    `usemtl` is honoured so a mesh built from many textured segments renders with
    the right texture on each. XCOM's exports declare no materials, so they get a
    single zero index exactly as before; Mercenaries' do, and a vehicle there is
    dozens of segments over a handful of textures.
    """
    V, T, N, F = [], [], [], []
    tri_mat = []
    mat_ids, cur = {}, 0
    with open(path, 'r', errors='replace') as fh:
        for line in fh:
            tok, _, rest = line.partition(' ')
            if tok == 'v':
                V.append([float(x) for x in rest.split()[:3]])
            elif tok == 'vt':
                T.append([float(x) for x in rest.split()[:2]])
            elif tok == 'vn':
                N.append([float(x) for x in rest.split()[:3]])
            elif tok == 'usemtl':
                name = rest.strip()
                if name not in mat_ids:
                    mat_ids[name] = len(mat_ids)
                cur = mat_ids[name]
            elif tok == 'f':
                # Every corner is normalised to (v, vt, vn). A face may be
                # written 'v', 'v/vt', 'v//vn' or 'v/vt/vn', and a single file
                # can mix them - a composed scene does, because some segments
                # carry UVs and some do not. Leaving them ragged makes the
                # numpy conversion fail on the whole mesh rather than on the
                # face that lacked a field.
                c = []
                for t in rest.split():
                    q = t.split('/')
                    c.append([int(q[0]) - 1 if q[0] else 0,
                              int(q[1]) - 1 if len(q) > 1 and q[1] else 0,
                              int(q[2]) - 1 if len(q) > 2 and q[2] else 0])
                for k in range(1, len(c) - 1):
                    F.append([c[0], c[k], c[k + 1]])
                    tri_mat.append(cur)
    faces = np.array(F, np.int32) if F else np.zeros((0, 3, 3), np.int32)
    return (np.array(V, np.float32),
            np.array(T or [[0, 0]], np.float32),
            np.array(N or [[0, 1, 0]], np.float32),
            faces,
            np.array(tri_mat, np.int32) if tri_mat else np.zeros(len(faces), np.int32))


_GLTF_COMPONENT = {5120: np.int8, 5121: np.uint8, 5122: np.int16,
                   5123: np.uint16, 5125: np.uint32, 5126: np.float32}
_GLTF_COUNT = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4,
               'MAT2': 4, 'MAT3': 9, 'MAT4': 16}   # MAT4: inverse bind matrices


def _glb_chunks(path):
    """The JSON and BIN chunks of a binary glTF, and a reader over its accessors.

    Split out of load_glb so the skinning loader below reads the SAME file the
    same way. The vertex order the two produce has to agree exactly - a posed
    vertex array is applied to geometry loaded separately, and an order that
    drifts by one primitive scrambles the model in a way that still looks like a
    mesh.
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

    return js, read


def _glb_primitives(js):
    """The primitives load_glb keeps, in the order it keeps them.

    One generator, so the geometry loader and the skinning loader cannot
    disagree about which primitives exist or what order their vertices are in.
    """
    for m in js.get('meshes', []):
        for prim in m.get('primitives', []):
            attrs = prim.get('attributes', {})
            if 'POSITION' not in attrs or 'indices' not in prim:
                continue
            yield prim, attrs


def load_glb(path):
    """Positions/UVs/normals/faces out of a binary glTF.

    Deliberately minimal: Helldivers' filediver output is one .glb per unit with
    everything embedded, and all this viewer needs is the geometry. No scene
    graph and no node transforms -- every primitive in the file is merged into
    one soup, which is right for "what shape is this" and wrong for anything
    else. Skinning is not applied here either; it lives in load_glb_rig, which
    poses this same soup.
    """
    js, read = _glb_chunks(path)

    V, T, N, F, M = [], [], [], [], []
    base = 0
    for prim, attrs in _glb_primitives(js):
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


# --------------------------------------------------------------------------
# skinning and animation
# --------------------------------------------------------------------------
#
# Enough of glTF's animation model to PLAY a clip, which is a different job
# from importing one: no retargeting, no blending, no root motion handling.
# Mercenaries' rigs are rigid - one bone per vertex - but the code handles the
# general four-influence case because Helldivers' .glb do use real weights and a
# viewer that quietly ignored three of them would show a subtly wrong mesh
# rather than fail.

def load_glb_rig(path):
    """The skin and clip list of a .glb, or None if it carries neither.

    The clips' sampler data is NOT read here. A Mercenaries character file holds
    1,639 clips and 116 MB of curves; reading them all to populate a list would
    make selecting the asset take seconds and would hold the lot in memory to
    play one. Only the chosen clip's accessors are read, in pose_glb.
    """
    js, read = _glb_chunks(path)
    skins = js.get('skins') or []
    clips = js.get('animations') or []
    if not skins and not clips:
        return None

    nodes = js.get('nodes', [])
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get('children', ()):
            parent[c] = i

    rest = []
    for n in nodes:
        rest.append((np.array(n.get('translation', (0, 0, 0)), np.float64),
                     np.array(n.get('rotation', (0, 0, 0, 1)), np.float64),
                     np.array(n.get('scale', (1, 1, 1)), np.float64)))

    joints, ibm = [], None
    if skins:
        joints = list(skins[0].get('joints', ()))
        if 'inverseBindMatrices' in skins[0]:
            # glTF stores a matrix column-major; transposing once here means the
            # rest of this file can think in rows.
            ibm = read(skins[0]['inverseBindMatrices']).astype(np.float64)
            ibm = ibm.reshape(-1, 4, 4).transpose(0, 2, 1)
        else:
            ibm = np.tile(np.eye(4), (len(joints), 1, 1))

    # Per-vertex influences, in load_glb's vertex order - see _glb_primitives.
    JI, JW, base = [], [], 0
    for prim, attrs in _glb_primitives(js):
        n = js['accessors'][attrs['POSITION']]['count']
        if 'JOINTS_0' in attrs and 'WEIGHTS_0' in attrs:
            JI.append(read(attrs['JOINTS_0']).astype(np.int32))
            w = read(attrs['WEIGHTS_0']).astype(np.float64)
            if w.dtype != np.float64:
                w = w.astype(np.float64)
            JW.append(w)
        else:
            # An unskinned primitive in a skinned file rides the skeleton root,
            # which is what a viewer showing it attached to nothing would get
            # wrong.
            JI.append(np.zeros((n, 4), np.int32))
            w = np.zeros((n, 4), np.float64)
            w[:, 0] = 1.0
            JW.append(w)
        base += n

    return {'path': str(path), 'js': js, 'read': read, 'nodes': nodes,
            'parent': parent, 'rest': rest, 'joints': joints, 'ibm': ibm,
            'joint_index': np.concatenate(JI) if JI else np.zeros((0, 4), np.int32),
            'joint_weight': np.concatenate(JW) if JW else np.zeros((0, 4)),
            'clips': [c.get('name') or 'clip %d' % i for i, c in enumerate(clips)],
            'vertex_count': base, '_sampled': {}}


def _anim_header(path):
    """(name, frames, fps, duration, [joint names]) from a .anim, header only.

    mercs_anim.py's format: a fixed header, then length-prefixed strings for the
    clip name, the events and the joints, then the dense float block at
    dataOffset. Everything this needs is in front of that offset, so a clip can
    be listed and matched to a skeleton without reading its curves - which is
    what makes indexing 1,835 of them cheap enough to do on selection.
    """
    # 8 KB covers a name, its events and 38 joint names with room to spare, and
    # the whole point is to index 1,835 clips without reading 172 MB of curves.
    # Re-read in full only if the strings actually run past it.
    with open(path, 'rb') as f:
        head = f.read(8192)
        if head[:4] != b'MRCA':
            return None
        if struct.unpack_from('<I', head, 40)[0] > len(head):
            f.seek(0)
            head = f.read(struct.unpack_from('<I', head, 40)[0])
    ver, frames, njoints = struct.unpack_from('<III', head, 4)
    fps, duration = struct.unpack_from('<2f', head, 16)
    nev, data_off = struct.unpack_from('<II', head, 36)
    p = 44

    def pstr(p):
        n = struct.unpack_from('<H', head, p)[0]
        return head[p + 2:p + 2 + n].decode('ascii', 'replace'), p + 2 + n

    name, p = pstr(p)
    for _ in range(nev):
        _, p = pstr(p + 4)
    joints = []
    for _ in range(njoints):
        s, p = pstr(p)
        joints.append(s)
        p += 1                                   # the flags byte
    return name, frames, fps, duration, joints


def anim_index(directory):
    """[(name, path, {joint names})] for a directory of .anim, cached.

    The scan is ~1,800 header reads. Done once per session and kept, because it
    is what lets any rigged mesh be offered the clips that fit it.
    """
    directory = str(Path(directory))
    if directory in _ANIM_INDEX:
        return _ANIM_INDEX[directory]
    out, failed, why = [], 0, None
    for f in sorted(Path(directory).glob('*.anim')):
        try:
            h = _anim_header(f)
        except Exception as e:
            h, failed, why = None, failed + 1, why or e
        if h:
            out.append((h[0], str(f), {j.lower() for j in h[4]}))
    # An empty library must not look like "this rig has no clips". A blanket
    # except here once turned a NameError into a silently empty panel, which is
    # indistinguishable from a model that genuinely has no animation.
    if failed:
        print('asset_browser: %d of %d clips in %s failed to index (%s: %s)'
              % (failed, failed + len(out), directory, type(why).__name__, why))
    _ANIM_INDEX[directory] = out
    return out


def anim_library(mesh_path, rig, threshold=0.75):
    """Standalone clips that fit this mesh's skeleton, as [(name, path)].

    The point: every human in Mercenaries shares one skeleton, so 1,639 clips
    drive any of 88 models. Binding them here by joint name means the viewer can
    play the lot against whichever character is selected, instead of the export
    duplicating the same curves into 88 files.
    """
    if not rig or not rig.get('joints'):
        return []
    p = Path(mesh_path).resolve()
    root = None
    for parent in p.parents:
        if (parent / 'animations').is_dir() and parent.name.endswith('_extracted'):
            root = parent / 'animations'
            break
    if root is None:
        return []
    names = {(rig['nodes'][n].get('name') or '').lower() for n in rig['joints']}
    out = []
    for name, path, cj in anim_index(root):
        # Overlap against the SMALLER set, not against the clip. A clip may
        # animate more joints than a given model has - the 25-joint NK soldiers
        # against a 36-joint clip is the common case - and it drives the joints
        # they share perfectly well; measuring against the clip rejected every
        # one of them at 69%. Measuring against the smaller set still keeps a
        # 4-joint flag clip away from a character, which is what the threshold
        # is actually for.
        if cj and len(cj & names) >= max(1, int(min(len(cj), len(names)) * threshold)):
            out.append((name, path))
    return out


def load_anim(path):
    """A whole .anim: header plus the dense (joints, frames, 7) float block."""
    h = _anim_header(path)
    if not h:
        raise ValueError('not a .anim')
    name, frames, fps, duration, joints = h
    raw = Path(path).read_bytes()
    data_off = struct.unpack_from('<I', raw, 40)[0]
    n = len(joints) * frames * 7
    data = np.frombuffer(raw, np.float32, count=n, offset=data_off)
    return {'name': name, 'frames': frames, 'fps': fps, 'duration': duration,
            'joints': joints, 'data': data.reshape(len(joints), frames, 7)}


def anim_sample(a, t):
    """{joint name: (translation, quaternion)} at time t, interpolated.

    The curves are dense at the clip's own rate, so this only ever blends
    between two neighbouring frames - and it has to, because the viewer redraws
    faster than 30 Hz and stepping to the nearest frame makes a smooth walk
    stutter at the redraw rate rather than at the animation's.
    """
    frames = a['frames']
    if frames <= 0:
        return {}
    x = max(0.0, min(float(t) * a['fps'], frames - 1))
    i = int(x)
    j = min(i + 1, frames - 1)
    u = x - i
    lo, hi = a['data'][:, i, :], a['data'][:, j, :]
    out = {}
    for k, name in enumerate(a['joints']):
        q0, q1 = lo[k, :4], hi[k, :4]
        q = _slerp(q0.astype(np.float64), q1.astype(np.float64), u) if u else q0
        tr = lo[k, 4:7] * (1.0 - u) + hi[k, 4:7] * u
        out[name.lower()] = (np.asarray(tr, np.float64), np.asarray(q, np.float64))
    return out


def clip_duration(rig, clip):
    """Seconds, from the largest time in any of the clip's inputs."""
    a = rig['js']['animations'][clip]
    end = 0.0
    for s in a['samplers']:
        acc = rig['js']['accessors'][s['input']]
        if 'max' in acc:
            end = max(end, float(acc['max'][0]))
        else:
            end = max(end, float(rig['read'](s['input']).max()))
    return end


def _clip_channels(rig, clip):
    """(node, path, times, values, interpolation) per channel, read once."""
    if clip in rig['_sampled']:
        return rig['_sampled'][clip]
    a = rig['js']['animations'][clip]
    out = []
    times = {}
    for ch in a['channels']:
        s = a['samplers'][ch['sampler']]
        if s['input'] not in times:
            times[s['input']] = rig['read'](s['input']).astype(np.float64).ravel()
        out.append((ch['target']['node'], ch['target']['path'],
                    times[s['input']],
                    rig['read'](s['output']).astype(np.float64),
                    s.get('interpolation', 'LINEAR')))
    rig['_sampled'] = {clip: out}          # one clip at a time; see load_glb_rig
    return out


def _slerp(a, b, u):
    """Shortest-arc quaternion interpolation.

    The sign fix is the part that matters: q and -q are the same rotation, so
    without it a joint whose keys straddle the double cover spins the long way
    round - one frame of a 359-degree flip in the middle of a walk cycle.
    """
    d = float(np.dot(a, b))
    if d < 0.0:
        b, d = -b, -d
    if d > 0.9995:
        q = a + (b - a) * u
    else:
        th = np.arccos(np.clip(d, -1.0, 1.0))
        s = np.sin(th)
        q = a * (np.sin((1 - u) * th) / s) + b * (np.sin(u * th) / s)
    n = np.linalg.norm(q)
    return q / n if n else np.array((0.0, 0.0, 0.0, 1.0))


def _sample(times, values, t, path, interp):
    if len(times) == 0:
        return None
    if interp == 'CUBICSPLINE':
        # Three values per key (in-tangent, value, out-tangent). Taking the
        # middle one is a linear approximation, and saying so beats pretending.
        values = values[1::3]
    i = int(np.searchsorted(times, t, side='right')) - 1
    if i < 0:
        return values[0]
    if i >= len(times) - 1:
        return values[len(values) - 1]
    span = times[i + 1] - times[i]
    u = 0.0 if span <= 0 else float((t - times[i]) / span)
    if interp == 'STEP':
        return values[i]
    if path == 'rotation':
        return _slerp(values[i], values[i + 1], u)
    return values[i] + (values[i + 1] - values[i]) * u


def _trs_matrix(t, q, s):
    x, y, z, w = q
    R = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4)
    m[:3, :3] = R * np.asarray(s)[None, :]
    m[:3, 3] = t
    return m


def pose_glb(rig, mesh, clip, t, anim=None):
    """(positions, normals) for the mesh at time t.

    `clip` is an index into the file's own animations; `anim` is a standalone
    .anim from load_anim, bound to the skeleton BY JOINT NAME. The second path
    is what lets one clip library drive 88 different characters - they share a
    skeleton, so a name is all the correspondence needed, and nothing about the
    clip is specific to the model it was authored on.

    Returns the bind pose unchanged when the file has no skin, so a .glb whose
    animation moves whole nodes rather than joints still previews rather than
    raising.
    """
    local = [_trs_matrix(*r) for r in rig['rest']]
    if anim is not None:
        by_name = anim_sample(anim, t)
        for i, n in enumerate(rig['nodes']):
            hit = by_name.get((n.get('name') or '').lower())
            if hit is None:
                continue          # joint the clip does not touch: keep the rest pose
            tr, q = hit
            local[i] = _trs_matrix(tr, q, rig['rest'][i][2])
    elif clip is not None:
        pose = {i: list(r) for i, r in enumerate(rig['rest'])}
        for node, path, times, values, interp in _clip_channels(rig, clip):
            v = _sample(times, values, t, path, interp)
            if v is None:
                continue
            pose[node][{'translation': 0, 'rotation': 1, 'scale': 2}[path]] = v
        for i, r in pose.items():
            local[i] = _trs_matrix(*r)

    world, parent = {}, rig['parent']

    def resolve(i, guard=0):
        if i in world:
            return world[i]
        p = parent.get(i)
        # A cycle cannot happen in a valid file, but a truncated one is a hang
        # rather than an error without this.
        m = local[i] if p is None or guard > 256 else resolve(p, guard + 1) @ local[i]
        world[i] = m
        return m

    V, N = mesh[0].astype(np.float64), mesh[2].astype(np.float64)
    if not rig['joints'] or rig['ibm'] is None or len(V) != rig['vertex_count']:
        return mesh[0], mesh[2]

    skin = np.stack([resolve(n) @ rig['ibm'][k] for k, n in enumerate(rig['joints'])])
    JI, JW = rig['joint_index'], rig['joint_weight']
    out = np.zeros_like(V)
    nrm = np.zeros_like(N)
    for k in range(JI.shape[1]):
        w = JW[:, k]
        if not np.any(w):
            continue
        m = skin[np.clip(JI[:, k], 0, len(skin) - 1)]
        out += w[:, None] * (np.einsum('vij,vj->vi', m[:, :3, :3], V) + m[:, :3, 3])
        nrm += w[:, None] * np.einsum('vij,vj->vi', m[:, :3, :3], N)
    ln = np.linalg.norm(nrm, axis=1, keepdims=True)
    nrm = np.divide(nrm, ln, out=np.zeros_like(nrm), where=ln > 1e-9)
    return out.astype(np.float32), nrm.astype(np.float32)


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
            elif src.get('uri'):
                # An image BESIDE the file, not inside it. mercs_gltf.py writes
                # these: embedding 4,643 PNGs into every character would have
                # duplicated the whole texture library per model. Resolved
                # relative to the .glb, as the spec says, and percent-decoded
                # because a URI is not a path.
                from urllib.parse import unquote
                uri = src['uri']
                if not uri.startswith('data:'):
                    p = (Path(path).parent / unquote(uri)).resolve()
                    if p.exists():
                        img = Image.open(p)
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


def default_flip_v(path):
    """Whether V should be flipped for this file, before any manual override.

    Only OBJ is ambiguous. glTF SPECIFIES the origin as top-left, so a .glb
    needs no flip and applying the OBJ one turns a face upside down - which is
    exactly what happened to Mercenaries' characters the moment their textures
    started resolving. Same model, same texture, exported both ways: the .obj
    (which stores 1-v) and the .glb (which stores v) only agree when the flip is
    applied to one and not the other.
    """
    return not str(path).lower().endswith('.glb')


def _uv_row(v, height, flip_v=True):
    """Texture row for a V coordinate.

    Which way up V runs is genuinely ambiguous for an OBJ from this library. OBJ
    puts the origin bottom-left, Unreal puts it top-left, and xcom_convert.py's
    --flip-v is opt-in, so the exported files may carry either.
    xcom_parcel_render.py assumes the flip, which is why that is the default
    here - see default_flip_v for the formats where it is not a guess.

    It is not a cosmetic choice: XCOM's emissive mask sits in a band at the very
    bottom of the MSK, and under one convention a given mesh reaches it while
    under the other it does not. Hence the toggle rather than a silent guess.
    """
    vv = v % 1.0
    return np.clip((1.0 - vv if flip_v else vv) * (height - 1), 0, height - 1).astype(np.int32)


# --------------------------------------------------------------------------
# GPU renderer
# --------------------------------------------------------------------------
#
# The numpy rasteriser is a Python loop over triangles at roughly 60 us each, so
# its cost is the TRIANGLE COUNT and not the resolution: 3,216 triangles is
# 0.20 s a frame and a 7,077-triangle interior is 0.93 s. Nothing tunable fixes
# that shape, which is why every interactive path used to swap to scattered
# samples and lose the texture and the surface detail with it.
#
# So: draw it on the GPU, where a few thousand triangles is free, and keep one
# renderer for moving and settled frames. moderngl is an OPTIONAL import -- the
# numpy path stays as the fallback, so the tool still runs on numpy and Pillow
# alone, just slower and grainy while moving.
#
# The projection maths below is copied from render_mesh deliberately, constant
# for constant, so swapping renderer does not move the camera. The same is true
# of the shading (0.18 + 0.82 * |n . LIGHT_DIR| off the GEOMETRIC normal) and of
# the two-pass alpha rule: opaque texels write depth, translucent ones draw
# without writing it, or a rotor-blur quad punches a hole through the fuselage.

_GPU = {'ctx': None, 'prog': None, 'fbo': {}, 'tex': {}, 'tried': False,
        'error': None}
_ANIM_INDEX = {}                 # directory -> [(name, path, joint names)]


def gpu_status():
    """(available, message). Called by the UI to decide which path to take."""
    if _GPU['ctx'] is not None:
        return True, _GPU['error'] or 'gpu'
    return False, _GPU['error'] or 'not initialised'


def gpu_init():
    """Create the offscreen context once. Returns True if the GPU path is live.

    Failure is expected and survivable -- a machine with no GL driver, a remote
    session, moderngl not installed -- so the reason is recorded and the caller
    falls back rather than the tool refusing to start.

    THREAD AFFINITY. A GL context belongs to the thread that made it, so this
    and every render_gpu call must happen on one thread - the Tk thread, here.
    The mesh loader deliberately does NOT touch it: it parses on a worker and
    hands the arrays back, and the drawing happens after that on the main
    thread. Creating the context in the worker would work exactly once and then
    fail on every redraw.
    """
    if _GPU['tried']:
        return _GPU['ctx'] is not None
    _GPU['tried'] = True
    try:
        import moderngl
    except ImportError:
        _GPU['error'] = 'moderngl not installed (pip install moderngl)'
        return False
    try:
        ctx = moderngl.create_standalone_context(require=330)
        prog = ctx.program(
            vertex_shader='''
                #version 330
                in vec3 in_pos;
                in vec3 in_nrm;
                in vec2 in_uv;
                out vec3 v_nrm;
                out vec2 v_uv;
                uniform mat3 u_rot;
                uniform vec3 u_centre;
                uniform vec3 u_cam;
                uniform vec2 u_pan;
                uniform float u_scale, u_zscale, u_m, u_size, u_zoom, u_fl,
                              u_persp, u_far;
                void main() {
                    vec3 p = u_persp > 0.5 ? u_rot * (in_pos - u_cam)
                                           : u_rot * in_pos;
                    float sx, sy, z;
                    if (u_persp > 0.5) {
                        sx = u_size * 0.5 + u_fl * p.x / p.z;
                        sy = u_size * 0.5 - u_fl * p.y / p.z;
                        z  = clamp(p.z / max(u_far, 1.0), -0.999, 0.999);
                    } else {
                        sx = (p.x - u_centre.x) / u_scale * 2.0 * u_m
                           + u_size * 0.5 + u_pan.x * u_zoom;
                        sy = u_size * 0.5
                           - (p.y - u_centre.y) / u_scale * 2.0 * u_m
                           + u_pan.y * u_zoom;
                        // Depth uses the mesh's own Z extent, not the X/Y one
                        // the framing is fitted to: a wide flat mesh seen edge
                        // on has a Z range far larger than u_scale, and reusing
                        // it clips the model in half at the near plane.
                        z  = clamp((p.z - u_centre.z) / u_zscale, -0.999, 0.999);
                    }
                    v_nrm = in_nrm;
                    v_uv = in_uv;
                    gl_Position = vec4(sx / u_size * 2.0 - 1.0,
                                       1.0 - sy / u_size * 2.0, z, 1.0);
                }
            ''',
            fragment_shader='''
                #version 330
                in vec3 v_nrm;
                in vec2 v_uv;
                out vec4 f_col;
                uniform sampler2D u_tex;
                uniform int u_mode;          // 0 flat, 1 normals, 2 textured
                uniform int u_pass;          // 0 opaque, 1 translucent
                uniform int u_flip_v;
                uniform vec3 u_light;
                void main() {
                    // v_nrm is the GEOMETRIC face normal, computed on the CPU
                    // and given to all three corners, so this is flat shading
                    // without needing a flat qualifier. abs() shades two-sided:
                    // a flipped winding should read as lit, not as a black hole.
                    vec3 n = normalize(v_nrm);
                    float lam = abs(dot(n, u_light));
                    vec3 col = vec3(0.18 + 0.82 * lam);
                    float a = 1.0;
                    if (u_mode == 1) {
                        col = n * 0.5 + 0.5;
                    } else if (u_mode == 2) {
                        vec2 uv = vec2(fract(v_uv.x),
                                       u_flip_v == 1 ? 1.0 - fract(v_uv.y)
                                                     : fract(v_uv.y));
                        vec4 t = texture(u_tex, uv);
                        col = t.rgb * (0.55 + 0.45 * (0.18 + 0.82 * lam));
                        a = t.a;
                    }
                    // Two passes rather than one threshold - see the note above
                    // render_gpu. 0.02 and 0.95 are render_mesh's own cutoffs.
                    if (a <= 0.02) discard;
                    if (u_pass == 0 && a < 0.95) discard;
                    if (u_pass == 1 && a >= 0.95) discard;
                    f_col = vec4(col, a);
                }
            ''')
        _GPU['ctx'], _GPU['prog'] = ctx, prog
        _GPU['error'] = ctx.info.get('GL_RENDERER', 'gpu')
        return True
    except Exception as e:                       # any driver or context failure
        _GPU['error'] = '%s: %s' % (type(e).__name__, e)
        return False


def _gpu_fbo(size):
    ctx = _GPU['ctx']
    if size not in _GPU['fbo']:
        # 4x MSAA, then resolved on read: the numpy path has no antialiasing at
        # all and its edges crawl, which is worse on a moving frame than on a
        # still one.
        colour = ctx.renderbuffer((size, size), 4, samples=4)
        depth = ctx.depth_renderbuffer((size, size), samples=4)
        ms = ctx.framebuffer(color_attachments=[colour], depth_attachment=depth)
        plain = ctx.simple_framebuffer((size, size), 4)
        _GPU['fbo'][size] = (ms, plain)
    return _GPU['fbo'][size]


def _gpu_texture(im):
    """Upload a PIL image once and keep it, keyed by identity."""
    ctx = _GPU['ctx']
    key = id(im)
    if key not in _GPU['tex']:
        if len(_GPU['tex']) > 64:                # bounded, not a leak
            for k in list(_GPU['tex'])[:32]:
                _GPU['tex'].pop(k).release()
        rgba = im.convert('RGBA')
        t = ctx.texture(rgba.size, 4, rgba.tobytes())
        t.build_mipmaps()
        t.anisotropy = 8.0
        _GPU['tex'][key] = t
    return _GPU['tex'][key]


def render_gpu(mesh, size=512, yaw=35.0, pitch=20.0, mode='flat',
               texture=None, zoom=1.0, pan=(0.0, 0.0), flip_v=True,
               tex_by_mat=None, camera=None, **_ignored):
    """render_mesh's picture, drawn by the GPU. Returns None if unavailable.

    Returning None rather than raising lets the caller treat "no GPU here" as a
    fallback rather than an error path, which is what it is.
    """
    if not gpu_init():
        return None
    import moderngl
    ctx = _GPU['ctx']
    prog = _GPU['prog']

    V, T, N, F = mesh[0], mesh[1], mesh[2], mesh[3]
    tri_mat = mesh[4] if len(mesh) > 4 else np.zeros(len(F), np.int32)
    if len(V) == 0 or len(F) == 0:
        return Image.new('RGB', (size, size), (0, 0, 0))

    p = V[F[:, :, 0]].astype(np.float32)             # (n,3,3) triangle corners
    rot = _view_rotation(yaw, pitch) if camera is None else \
        _view_rotation(camera.get('yaw', 0.0), camera.get('pitch', 0.0))

    persp = camera is not None
    cam = np.zeros(3, np.float32)
    fl = far = 0.0
    centre = np.zeros(3, np.float32)
    scale = zscale = 1.0
    if persp:
        cam = np.asarray(camera.get('pos', (0, 0, 0)), np.float32)
        far = float(camera.get('far', 0.0))
        near = float(camera.get('near', 0.25))
        q = (p - cam) @ rot.T
        # Same near/far rejection as the numpy path. The GPU could clip properly,
        # but keeping the same triangles keeps the two renderers comparable.
        keep = (q[:, :, 2] > near).all(axis=1)
        if far > 0:
            keep &= q[:, :, 2].min(axis=1) < far
        if not keep.any():
            return Image.new('RGB', (size, size), (0, 0, 0))
        F, tri_mat, p = F[keep], tri_mat[keep], p[keep]
        fl = (size * 0.5) / np.tan(np.radians(float(camera.get('fov', 70.0))) * 0.5)
        far = max(far, float(q[:, :, 2].max())) if far > 0 else float(q[:, :, 2].max())
    else:
        q = p @ rot.T
        lo, hi = q.reshape(-1, 3).min(0), q.reshape(-1, 3).max(0)
        centre = ((lo + hi) / 2.0).astype(np.float32)
        scale = float((hi - lo)[:2].max()) or 1.0
        zscale = max(float(hi[2] - lo[2]), scale) or 1.0

    # Geometric face normal per triangle, replicated to its corners: the shader
    # shades exactly what render_mesh shades.
    e1, e2 = p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]
    fn = np.cross(e1, e2)
    ln = np.linalg.norm(fn, axis=1, keepdims=True)
    fn = (fn / np.where(ln == 0, 1, ln)).astype(np.float32)

    uv = (T[F[:, :, 1]] if len(T) > 1 else np.zeros((len(F), 3, 2), np.float32))
    want_tex = mode in ('textured', 'lit', 'emissive') and (texture is not None
                                                            or tex_by_mat)
    gl_mode = 1 if mode == 'normals' else (2 if want_tex else 0)

    # One draw per texture: triangles are grouped by material so a mesh made of
    # many textured segments needs a handful of draws, not one per triangle.
    groups = []
    if gl_mode == 2 and tex_by_mat:
        order = np.argsort(tri_mat, kind='stable')
        p, fn, uv, tri_mat = p[order], fn[order], uv[order], tri_mat[order]
        bounds = np.flatnonzero(np.diff(tri_mat)) + 1
        for a, b in zip(np.r_[0, bounds], np.r_[bounds, len(tri_mat)]):
            groups.append((int(a), int(b), tex_by_mat.get(int(tri_mat[a]))))
    else:
        groups.append((0, len(p), texture if gl_mode == 2 else None))

    verts = np.empty((len(p) * 3, 8), np.float32)
    verts[:, 0:3] = p.reshape(-1, 3)
    verts[:, 3:6] = np.repeat(fn, 3, axis=0)
    verts[:, 6:8] = uv.reshape(-1, 2)
    vbo = ctx.buffer(verts.tobytes())
    vao = ctx.vertex_array(prog, [(vbo, '3f 3f 2f', 'in_pos', 'in_nrm', 'in_uv')])

    ms, plain = _gpu_fbo(size)
    ms.use()
    ctx.clear(0.0, 0.0, 0.0, 1.0)
    ctx.enable(moderngl.DEPTH_TEST)
    ctx.disable(moderngl.CULL_FACE)              # two-sided, like the numpy path
    prog['u_rot'].write(np.ascontiguousarray(rot.T, np.float32).tobytes())
    prog['u_centre'].value = tuple(float(c) for c in centre)
    prog['u_cam'].value = tuple(float(c) for c in cam)
    prog['u_pan'].value = (float(pan[0]), float(pan[1]))
    prog['u_scale'].value = float(scale)
    prog['u_zscale'].value = float(zscale)
    prog['u_m'].value = float(size * 0.44 * zoom)
    prog['u_size'].value = float(size)
    prog['u_zoom'].value = float(zoom)
    prog['u_fl'].value = float(fl)
    prog['u_persp'].value = 1.0 if persp else 0.0
    prog['u_far'].value = float(far)
    prog['u_mode'].value = gl_mode
    prog['u_flip_v'].value = 1 if flip_v else 0
    prog['u_light'].value = tuple(float(c) for c in LIGHT_DIR)

    for pass_i in (0, 1):
        prog['u_pass'].value = pass_i
        ctx.depth_mask = (pass_i == 0)
        if pass_i == 1:
            ctx.enable(moderngl.BLEND)
            ctx.blend_func = moderngl.SRC_ALPHA, moderngl.ONE_MINUS_SRC_ALPHA
        for a, b, im in groups:
            if b <= a:
                continue
            if gl_mode == 2:
                if im is None:
                    continue
                _gpu_texture(im).use(0)
                prog['u_tex'].value = 0
            vao.render(moderngl.TRIANGLES, vertices=(b - a) * 3, first=a * 3)
    ctx.disable(moderngl.BLEND)
    ctx.depth_mask = True

    ctx.copy_framebuffer(plain, ms)              # resolve the MSAA samples
    # GL's framebuffer origin is bottom-left and PIL's is top-left, so the
    # read comes back upside down. Flipping here rather than in the projection
    # keeps the vertex shader's arithmetic identical to render_mesh's.
    img = Image.frombytes('RGBA', (size, size), plain.read(components=4))
    img = img.transpose(Image.FLIP_TOP_BOTTOM)
    vao.release()
    vbo.release()
    return img.convert('RGB')


def _view_rotation(yaw, pitch):
    ry, rx = np.radians(yaw), np.radians(pitch)
    cy, sy = np.cos(ry), np.sin(ry)
    cx, sx = np.cos(rx), np.sin(rx)
    return np.array([[cy, 0, -sy], [sx * sy, cx, sx * cy], [cx * sy, -sx, cx * cy]], np.float32)


def render_points(mesh, size=512, yaw=35.0, pitch=20.0, zoom=1.0, pan=(0.0, 0.0),
                  texture=None, budget=500000, seed=0, camera=None):
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

    if camera is not None:
        # Same perspective camera as render_mesh, so flying gets the vectorised
        # renderer too. Without this, moving in fly mode falls back to the
        # per-face loop and runs at about three frames a second on an assembled
        # world; the splat path is numpy end to end and is the only reason the
        # orbit view is interactive at all.
        cam = np.asarray(camera.get('pos', (0.0, 0.0, 0.0)), np.float32)
        near = float(camera.get('near', 0.25))
        far = float(camera.get('far', 0.0))

        # Cull in WORLD space first, before the rotation. Culling after the
        # transform still pays to rotate every triangle in the scene, and on an
        # assembled world that fixed cost is the frame - sample budget and
        # resolution barely move it. One vertex per triangle is enough for a
        # coarse reject; the exact near/far test still runs afterwards.
        idx = F[:, 0, 0]
        if far > 0:
            d = V[idx] - cam
            rough = (d * d).sum(1) < (far + 64.0) ** 2
            if not rough.any():
                return Image.fromarray((img * 255).astype(np.uint8))
            F = F[rough]

        p = (V[F[:, :, 0]] - cam) @ _view_rotation(camera.get('yaw', 0.0),
                                                   camera.get('pitch', 0.0)).T
        keep = (p[:, :, 2] > near).all(axis=1)
        if far > 0:
            keep &= p[:, :, 2].min(axis=1) < far
        if not keep.any():
            return Image.fromarray((img * 255).astype(np.uint8))
        F, p = F[keep], p[keep]
        fl = (size * 0.5) / np.tan(np.radians(float(camera.get('fov', 70.0))) * 0.5)
        sx = size / 2 + fl * p[:, :, 0] / p[:, :, 2]
        sy = size / 2 - fl * p[:, :, 1] / p[:, :, 2]
        sz = p[:, :, 2]
    else:
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
                spec_texture=None, emis_texture=None, flip_v=True, tex_by_mat=None,
                camera=None):
    """Rasterise a mesh to a PIL image. No GPU, no lighting rig.

    Orthographic by default and on purpose: it keeps proportions readable and
    makes two meshes visually comparable, which perspective does not.

    Pass `camera` -- {'pos': (x,y,z), 'yaw':, 'pitch':, 'fov':, 'near':} -- for a
    free-fly perspective view instead. That exists for terrain and assembled
    worlds, where fitting a bounding box to the frame is useless: a 4 km map
    reduces to a smudge, and the only way to read it is to stand in it.
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

    def _rot(yaw_deg, pitch_deg):
        ry, rx = np.radians(yaw_deg), np.radians(pitch_deg)
        cy, sy = np.cos(ry), np.sin(ry)
        cx, sx = np.cos(rx), np.sin(rx)
        return np.array([[cy, 0, -sy], [sx * sy, cx, sx * cy],
                         [cx * sy, -sx, cx * cy]], np.float32)

    if camera is not None:
        # FREE-FLY PERSPECTIVE. The orbit path below frames a bounding box,
        # which is right for one object and useless for a 4 km terrain: the
        # whole map collapses to a smudge and there is no way to stand in it.
        # Here the camera has a position and looks where it is pointed, and
        # distant geometry actually gets smaller.
        cam = np.asarray(camera.get('pos', (0.0, 0.0, 0.0)), np.float32)
        near = float(camera.get('near', 0.25))
        far = float(camera.get('far', 0.0))
        # Coarse world-space reject before the rotation - see render_points for
        # why the order matters. Transforming the whole scene and culling after
        # costs the same whatever the cull distance is.
        if far > 0:
            d = V[F[:, 0, 0]] - cam
            rough = (d * d).sum(1) < (far + 64.0) ** 2
            if not rough.any():
                return Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))
            F, tri_mat = F[rough], tri_mat[rough]
        p = (V[F[:, :, 0]] - cam) @ _rot(camera.get('yaw', 0.0),
                                         camera.get('pitch', 0.0)).T
        # Triangles crossing the near plane are dropped rather than clipped.
        # Clipping properly means splitting them, which this rasteriser has no
        # machinery for; dropping costs a sliver at the very edge of view.
        keep = (p[:, :, 2] > near).all(axis=1)
        # A far cull is not a quality setting here, it is the difference between
        # a viewable scene and a frozen window: the rasteriser is a Python loop
        # at ~60 us a face, so 172,000 triangles is ten seconds whatever they
        # are. This test is vectorised and runs BEFORE that loop, so culling is
        # nearly free and the loop only sees what is close enough to matter.
        #
        # It is also what the game did. There are no LOD meshes on disc; instead
        # every modl carries distance thresholds in its INFO - 80/180/1000 m on
        # 412 models, and -1 (never cull) on 902 of them.
        if far > 0:
            keep &= p[:, :, 2].min(axis=1) < far
        if not keep.any():
            return Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))
        F, tri_mat, p = F[keep], tri_mat[keep], p[keep]
        fl = (size * 0.5) / np.tan(np.radians(float(camera.get('fov', 70.0))) * 0.5)
        sx_ = size / 2 + fl * p[:, :, 0] / p[:, :, 2]
        sy_ = size / 2 - fl * p[:, :, 1] / p[:, :, 2]
        depth = p[:, :, 2]
    else:
        p = V[F[:, :, 0]]                                # (n,3,3) triangle corners

        rot = _rot(yaw, pitch)
        p = p @ rot.T

        lo, hi = p.reshape(-1, 3).min(0), p.reshape(-1, 3).max(0)
        centre = (lo + hi) / 2.0
        scale = float((hi - lo)[:2].max())
        if scale <= 0:
            scale = 1.0
        # Zoom and pan are applied after the fit-to-frame, which keeps "framed by
        # default" true for every mesh regardless of its authored scale.
        #
        # Pan is held in zoom-1 pixels and scaled by zoom here, rather than being
        # a raw screen offset. A raw offset does not shrink when you zoom out, so
        # a pan set while zoomed in walks the model off the canvas as you pull
        # back and the mesh appears to vanish.
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
        # RGBA, not RGB: the alpha channel is what lets a cutout texture -- rotor
        # blur, foliage, chain-link -- read as its shape instead of a black quad.
        mat_tex = {k: np.asarray(v.convert('RGBA'), np.float32) / 255.0
                   for k, v in tex_by_mat.items() if v is not None}
    elif mode in ('textured', 'lit', 'emissive') and texture is not None and len(T) > 1:
        uv = T[F[:, :, 1]]
        tex = np.asarray(texture.convert('RGBA'), np.float32) / 255.0
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

        # The texture is sampled BEFORE the depth write, so a transparent texel
        # can be discarded without leaving its depth behind. Doing it the other
        # way round - write depth, then discard - punches invisible holes
        # through whatever is behind, which is exactly what a rotor-blur quad or
        # a chain-link fence would do to the rest of the model.
        u = v = ui = vi = tex_i = None
        alpha = None
        if uv is not None and write.any():
            u = w0 * uv[i, 0, 0] + w1 * uv[i, 1, 0] + w2 * uv[i, 2, 0]
            v = w0 * uv[i, 0, 1] + w1 * uv[i, 1, 1] + w2 * uv[i, 2, 1]
            tex_i = tex if mat_tex is None else mat_tex.get(int(tri_mat[i]))
            if tex_i is not None:
                tw, th = tex_i.shape[1], tex_i.shape[0]
                ui = np.clip((u % 1.0) * (tw - 1), 0, tw - 1).astype(np.int32)
                vi = _uv_row(v, th, flip_v)
                if tex_i.shape[2] == 4:
                    alpha = tex_i[vi, ui, 3]
                    # Only near-zero texels are dropped outright. A hard cutout
                    # at 0.5 is wrong for soft art: a rotor blur is a GRADIENT,
                    # mostly half-transparent rather than fully so, and a
                    # threshold keeps all of it and draws a solid square.
                    write = write & (alpha > 0.02)
        if not write.any():
            continue                      # occluded, or entirely transparent

        # Depth is written only where the surface is essentially opaque. A
        # half-transparent texel must not occlude what is behind it, or the
        # blur disc stamps a hole through the fuselage.
        if alpha is None:
            sub[write] = z[write]
        else:
            solid = write & (alpha >= 0.95)
            sub[solid] = z[solid]

        if uv is not None:
            if tex_i is None:
                px = np.broadcast_to(col[i], (maxy - miny, maxx - minx, 3))
                tile = img[miny:maxy, minx:maxx]
                tile[write] = px[write]
                continue
            tex = tex_i
            shade = abs(float(fn[i] @ LIGHT_DIR))
            px = tex[vi, ui, :3] * (0.55 + 0.45 * shade)
            if emis is not None:
                eh, ew = emis.shape[:2]
                ei = _uv_row(v, eh, flip_v)
                ej = np.clip((u % 1.0) * (ew - 1), 0, ew - 1).astype(np.int32)
                glow = emis[ei, ej][..., None] * tex[vi, ui, :3]
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
        if alpha is not None and px.ndim == 3:
            # Blend rather than replace. Triangles are not sorted back to front,
            # so overlapping transparent surfaces can composite in the wrong
            # order - acceptable here because the alternative is a black square
            # where the art is a soft gradient, and the common case is one
            # transparent quad over an opaque hull.
            a3 = alpha[..., None]
            blended = tile * (1.0 - a3) + px * a3
            tile[write] = blended[write]
        else:
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
    ap.add_argument('--kind', choices=['mesh', 'scene', 'terrain', 'material',
                                       'texture', 'guitexture', 'other'])
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
    ap.add_argument('--fly', metavar='X,Y,Z',
                    help='free-fly camera at this position instead of orbiting')
    ap.add_argument('--fov', type=float, default=70.0)
    ap.add_argument('--far', type=float, default=0.0,
                    help='cull geometry beyond this distance (metres); 0 = no cull')
    ap.add_argument('--refresh', action='store_true', help='rebuild the catalogue')
    ap.add_argument('--flip-v', dest='flip_v', default=None,
                    action=argparse.BooleanOptionalAction,
                    help='override the V flip; defaults per format, see '
                         'default_flip_v')
    ap.add_argument('--gpu', default=None, action=argparse.BooleanOptionalAction,
                    help='force the GPU renderer on or off (default: use it '
                         'when moderngl is importable)')
    a = ap.parse_args()

    if a.render:
        mesh = load_mesh(a.render)
        tex = None
        tex_by_mat = None
        if a.mode == 'textured':
            # Per-material first, so a mesh made of many textured segments
            # renders correctly rather than wearing one sheet all over. Falls
            # back to XCOM's single-diffuse link when the mesh declares none.
            mats, _ = mesh_materials(a.render)
            tex_by_mat = {i: m['diffuse'] for i, m in enumerate(mats) if m.get('diffuse')}
            if not tex_by_mat:
                tex_by_mat = None
                xm = xcom_material(a.render)
                if xm and xm.get('diffuse'):
                    tex = load_texture(xm['diffuse'])
        cam = None
        if a.fly:
            cam = {'pos': tuple(float(v) for v in a.fly.split(',')),
                   'yaw': a.yaw, 'pitch': a.pitch, 'fov': a.fov, 'far': a.far}
        flip = a.flip_v if a.flip_v is not None else default_flip_v(a.render)
        img = None
        if a.gpu is not False and a.mode in ('flat', 'normals', 'textured'):
            img = render_gpu(mesh, size=max(a.size, 512), yaw=a.yaw,
                             pitch=a.pitch, mode=a.mode, texture=tex,
                             tex_by_mat=tex_by_mat, camera=cam, flip_v=flip)
        if img is None:
            img = render_mesh(mesh, size=max(a.size, 512), yaw=a.yaw, pitch=a.pitch,
                              mode=a.mode, texture=tex, tex_by_mat=tex_by_mat,
                              camera=cam, flip_v=flip,
                              max_tris=400000 if cam else 60000)
        img.save(a.out)
        print(f'{len(mesh[0])} verts, {len(mesh[3])} tris'
              f'{f", {len(tex_by_mat)} textured materials" if tex_by_mat else ""}'
              f' -> {a.out}')
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
