"""xcom_materials.py - recover mesh -> texture assignments for the library.

The bulk sweep drops meshes and textures into a folder side by side with no
pairing, because UE3's OBJ exporter merges material sections away. But the
assignment is recoverable from the SDK, in two hops:

    1. pkginfo <pkg> -all lists every StaticMesh export together with its
       DependsMap, which names the MaterialInstanceConstant it uses --
       fully qualified, so cross-package references resolve:

           Export 12: 'DirtPileCrnDeco_2x2A'   Class: 'StaticMesh'
             1) MaterialInstanceConstant TextureLibrary_ClimateZones.WLD_Materials.WLD_MudA

    2. batchexport <pkg> MaterialInstanceConstant T3D dumps each material with
       its texture parameters by name:

           TextureParameterValues(0)=(ParameterName="Diffuse",
               ParameterValue=Texture2D'Foliage_Temperate.Textures.WLD_MapleTreeBarkA_DIF')

Join the two and every mesh gets its diffuse/normal/mask. That is what this
does: `scan` parses one pkginfo dump, `build` joins everything and writes the
.mtl files (and patches the OBJs to reference them).

Subcommands:
    scan  --pkg NAME --dump FILE --out TSV
    build --library DIR --mats DIR [--write-mtl] [--patch-obj]
"""

from __future__ import annotations

import argparse
import csv
import re
from collections import defaultdict
from pathlib import Path

EXPORT_RE = re.compile(r"^\s*Export \d+: '(.+)'")
CLASS_RE = re.compile(r"Class: '(.+?)'")
MIC_DEP_RE = re.compile(r"^\s*\d+\)\s+(?:MaterialInstanceConstant|Material)\s+(\S+)")
TEXPARAM_RE = re.compile(
    r'TextureParameterValues\(\d+\)=\(ParameterName="([^"]+)",'
    r"ParameterValue=Texture2D'([^']+)'")
PARENT_RE = re.compile(r"^\s*Parent=Material'([^']+)'", re.M)

# Parameter naming is NOT uniform. Plain props use "Diffuse"/"Normal"/"Masks",
# but terrain materials parented to Terrains_VP are vertex-paint BLENDS and
# name their layers "Mid_Diffuse", "Bottom_Diffuse", "Top_Normal", "BlendMask".
# We have no vertex colours (OBJ carries none), so a blend cannot be
# reproduced; the middle layer is taken as the representative one.
LAYER_RANK = ['', 'mid_', 'top_', 'base', 'bottom_']
# Every terrain material carries a fixed engine slot pointing at t_drops.
IGNORE_PARAM = 'donotchange'


def pick(tex: dict, kind: str) -> str:
    """Best texture for 'diffuse' | 'normal' | 'mask' out of a parameter set."""
    keys = [k for k in tex
            if (kind in k or (kind == 'mask' and 'msk' in k))
            and IGNORE_PARAM not in k]
    if not keys:
        return ''

    def rank(k: str):
        if k in (kind, kind + 's'):
            return (0, len(k))
        for i, pre in enumerate(LAYER_RANK[1:], start=1):
            if k.startswith(pre):
                return (i, len(k))
        return (len(LAYER_RANK), len(k))

    return tex[min(keys, key=rank)]


# ------------------------------------------------------------------- scan
def scan(dump: Path, pkg: str, out: Path):
    """Extract StaticMesh -> material references from one pkginfo -all dump."""
    lines = dump.read_text(encoding='utf-8', errors='replace').splitlines()
    rows = []
    i = 0
    while i < len(lines):
        m = EXPORT_RE.match(lines[i])
        if not m:
            i += 1
            continue
        name = m.group(1)
        cls = None
        for j in range(i + 1, min(i + 6, len(lines))):
            c = CLASS_RE.search(lines[j])
            if c:
                cls = c.group(1)
                break
        if cls != 'StaticMesh':
            i += 1
            continue
        # Walk this export's block collecting material dependencies in order.
        mats = []
        j = i + 1
        while j < len(lines) and not EXPORT_RE.match(lines[j]):
            d = MIC_DEP_RE.match(lines[j])
            if d and d.group(1) not in mats:
                mats.append(d.group(1))
            j += 1
        if mats:
            rows.append((pkg, name, ';'.join(mats)))
        i = j

    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open('w', encoding='utf-8', newline='') as fh:
        for r in rows:
            fh.write('\t'.join(r) + '\n')
    print(f"{pkg}: {len(rows)} meshes with material refs")
    return 0


# ------------------------------------------------------------------ build
def parse_t3d(path: Path) -> dict:
    txt = path.read_text(errors='replace')
    tex = {}
    for pname, tref in TEXPARAM_RE.findall(txt):
        tex[pname.strip().lower()] = tref
    par = PARENT_RE.search(txt)
    return {'name': path.stem, 'textures': tex, 'parent': par.group(1) if par else ''}


def resolve_png(ref: str, library: Path, home_pkg: str) -> str:
    """'Pkg.Group.Name' or bare 'Name' -> a PNG path inside the library."""
    if not ref:
        return ''
    bits = ref.split('.')
    name = bits[-1]
    pkg = bits[0] if len(bits) > 1 else home_pkg
    p = library / pkg / f"{name}.png"
    if p.exists():
        return f"{pkg}/{name}.png"
    # Fall back to the mesh's own package, then anywhere in the library.
    p = library / home_pkg / f"{name}.png"
    if p.exists():
        return f"{home_pkg}/{name}.png"
    hits = list(library.glob(f"*/{name}.png"))
    return f"{hits[0].parent.name}/{name}.png" if hits else ''


def load_basemats(library: Path) -> dict[str, list[str]]:
    """material name -> its textures, from base-Material expression graphs.

    A MaterialInstanceConstant exposes TextureParameterValues, which is what
    the main path reads. A plain `Material` does not: its textures are wired
    into MaterialExpressionTextureSample nodes instead, so ~2500 meshes came
    out with a material reference but no texture. tools/xcom_fx.py `basemats`
    dumps those graphs; this is the fallback that consumes them.
    """
    out: dict[str, list[str]] = {}
    f = library / 'basematerials.csv'
    if not f.exists():
        return out
    for r in csv.DictReader(f.open(encoding='utf-8')):
        out.setdefault(r['material'].lower(), []).append(r['texture'])
    return out


def classify_texture(name: str) -> str:
    """Guess a texture's role from its name - graphs have no parameter names."""
    n = name.lower()
    if any(k in n for k in ('_nrm', '_norm', '_nml', 'normal')):
        return 'normal'
    if any(k in n for k in ('_msk', '_mask', '_opc', '_spec', '_emis')):
        return 'mask'
    return 'diffuse'


def build(library: Path, mats: Path, write_mtl: bool, patch_obj: bool):
    basemats = load_basemats(library)
    # mesh -> [material refs]
    deps: dict[tuple[str, str], list[str]] = {}
    for tsv in (mats / 'deps').glob('*.tsv'):
        for line in tsv.read_text(encoding='utf-8').splitlines():
            parts = line.split('\t')
            if len(parts) == 3:
                deps[(parts[0], parts[1])] = parts[2].split(';')

    # material -> textures, keyed both fully-qualified and by bare name
    micdefs: dict[str, dict] = {}
    for pkgdir in (mats / 't3d').iterdir() if (mats / 't3d').is_dir() else []:
        if not pkgdir.is_dir():
            continue
        for f in pkgdir.glob('*.T3D'):
            d = parse_t3d(f)
            d['package'] = pkgdir.name
            # Keys are lowercased; see the lookup below for why.
            micdefs.setdefault(f.stem.lower(), d)
            micdefs[f"{pkgdir.name}::{f.stem}".lower()] = d

    rows = []
    per_pkg_mtl: dict[str, dict[str, dict]] = defaultdict(dict)
    for obj in sorted(library.glob('*/*.obj')):
        pkg, mesh = obj.parent.name, obj.stem
        matrefs = deps.get((pkg, mesh), [])
        chosen = None
        for ref in matrefs:
            bits = ref.split('.')
            # Case-INSENSITIVE: XCOM's own references disagree with its file
            # names (`adventacunits.Materials.adventacunits` vs the on-disk
            # `AdventACUnits`), and an exact match silently loses ~1600 meshes.
            cand = (micdefs.get(f"{bits[0]}::{bits[-1]}".lower())
                    or micdefs.get(bits[-1].lower()))
            if cand:
                chosen = (ref, cand)
                break
        if not chosen:
            # No MIC for this mesh. Fall back to the base Material's expression
            # graph, whose textures carry no parameter names - so the role of
            # each is inferred from its suffix (_NRM, _MSK, ...).
            ref = matrefs[0] if matrefs else ''
            texes = basemats.get(ref.split('.')[-1].lower(), []) if ref else []
            byrole: dict[str, str] = {}
            for t in texes:
                byrole.setdefault(classify_texture(t.split('.')[-1]), t)
            dif = resolve_png(byrole.get('diffuse', ''), library, pkg)
            nrm = resolve_png(byrole.get('normal', ''), library, pkg)
            msk = resolve_png(byrole.get('mask', ''), library, pkg)
            rows.append({'package': pkg, 'mesh': mesh, 'material': ref,
                         'diffuse': dif, 'normal': nrm, 'mask': msk,
                         'parent': 'base-material' if texes else '',
                         'resolved': 'yes' if dif else ''})
            continue
        ref, d = chosen
        home = d.get('package', pkg)
        dif = resolve_png(pick(d['textures'], 'diffuse'), library, home)
        nrm = resolve_png(pick(d['textures'], 'normal'), library, home)
        msk = resolve_png(pick(d['textures'], 'mask'), library, home)
        rows.append({'package': pkg, 'mesh': mesh, 'material': ref, 'diffuse': dif,
                     'normal': nrm, 'mask': msk, 'parent': d['parent'],
                     'resolved': 'yes' if dif else ''})
        if dif:
            per_pkg_mtl[pkg][d['name']] = {'diffuse': dif, 'normal': nrm, 'mask': msk,
                                           'mesh': mesh}

    out = library / 'materials.csv'
    cols = ['package', 'mesh', 'material', 'diffuse', 'normal', 'mask', 'parent', 'resolved']
    with out.open('w', newline='', encoding='utf-8') as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)

    got = sum(1 for r in rows if r['resolved'])
    named = sum(1 for r in rows if r['material'])
    print(f"{len(rows)} meshes | {named} have a material ref | {got} resolved to a diffuse")
    print(f"-> {out}")

    if write_mtl or patch_obj:
        write_materials(library, rows, patch_obj)
    return 0


def write_materials(library: Path, rows: list[dict], patch_obj: bool):
    """One .mtl per package holding every material used by its meshes.

    Texture paths are written RELATIVE to the package folder, so a material in
    a sibling folder becomes ../Other/tex.png. That keeps every folder loadable
    in place without duplicating images across 594 directories.
    """
    by_pkg: dict[str, dict[str, dict]] = defaultdict(dict)
    for r in rows:
        if r['resolved']:
            by_pkg[r['package']][mtl_name(r['material'])] = r

    written = patched = 0
    for pkg, mats in by_pkg.items():
        lines = ["# generated by tools/xcom_materials.py"]
        for name, r in sorted(mats.items()):
            lines += [f"newmtl {name}", "Ka 1.000 1.000 1.000", "Kd 1.000 1.000 1.000",
                      "Ks 0.000 0.000 0.000", "d 1.0", "illum 2",
                      f"map_Kd {rel(pkg, r['diffuse'])}"]
            if r['normal']:
                lines.append(f"map_Bump {rel(pkg, r['normal'])}")
            if r['mask']:
                lines.append(f"# mask (cutout in BLUE channel): {rel(pkg, r['mask'])}")
            lines.append("")
        (library / pkg / f"{pkg}.mtl").write_text("\n".join(lines) + "\n")
        written += 1

    if patch_obj:
        for r in rows:
            if not r['resolved']:
                continue
            obj = library / r['package'] / f"{r['mesh']}.obj"
            if not obj.exists():
                continue
            txt = obj.read_text()
            if txt.startswith('mtllib') or '\nmtllib ' in txt:
                continue
            name = mtl_name(r['material'])
            head = f"mtllib {r['package']}.mtl\nusemtl {name}\n"
            # Put the references before the first vertex so any loader sees
            # them; OBJ allows usemtl anywhere before the faces it applies to.
            idx = txt.find('\nv ')
            txt = txt[:idx + 1] + head + txt[idx + 1:] if idx >= 0 else head + txt
            obj.write_text(txt)
            patched += 1
    print(f"wrote {written} .mtl files, patched {patched} OBJs")


def mtl_name(ref: str) -> str:
    return re.sub(r'[^A-Za-z0-9_]', '_', ref.split('.')[-1]) or 'material'


def rel(pkg: str, path: str) -> str:
    p = Path(path)
    return p.name if p.parent.name == pkg else f"../{path}"


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('scan')
    s.add_argument('--pkg', required=True)
    s.add_argument('--dump', required=True, type=Path)
    s.add_argument('--out', required=True, type=Path)
    b = sub.add_parser('build')
    b.add_argument('--library', required=True, type=Path)
    b.add_argument('--mats', required=True, type=Path)
    b.add_argument('--write-mtl', action='store_true')
    b.add_argument('--patch-obj', action='store_true')
    a = ap.parse_args(argv)
    if a.cmd == 'scan':
        return scan(a.dump, a.pkg, a.out)
    return build(a.library, a.mats, a.write_mtl, a.patch_obj)


if __name__ == '__main__':
    raise SystemExit(main())
