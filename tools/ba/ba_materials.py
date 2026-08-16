"""Build ba_materials.csv: material name -> its albedo / mask / normal PNG.

    py -3 tools/ba/ba_materials.py [--ripped D:/ba_extracted/_ripper_units] \
                                   [--out D:/ba_extracted/ba_materials.csv]

WHY A CSV AND NOT EMBEDDED TEXTURES. The .glb the browser reads carry geometry,
skin and animation but no pixels - see ba_glb.py for why. So something has to
say which texture belongs to which material, and the honest source is the
material assets themselves rather than a filename convention.

WHY NOT GUESS FROM NAMES. The obvious shortcut is to match `VDV.mat` against
`*_VDV_BaseMap.png` and be done. It falls over immediately: the maps for
`RU_VDV_rifle` are named `VDV_low_VDV_BaseMap`, `VDV_low_VDV_MaskMap` and
`VDV_low_VDV_Normal`, where `VDV_low` is the *mesh* and `VDV` the material, and
there is a separate `RU_VDVSpetsnaz.mat` and `VDV_Razvedka.mat` whose names are
prefixes and substrings of each other. Substring matching pairs the wrong maps
onto the wrong soldiers and does it silently.

The real link is exact. A `.mat` names its textures by GUID:

    m_TexEnvs:
      _Albedo:  {m_Texture: {guid: 5ad208d2efafeaf48bb161b28713553a}}
      _Mask:    {m_Texture: {guid: 60665fe043f8fe742bd5c3a0990a023c}}
      _Normal:  {m_Texture: {guid: e78c69be1fcc2db48ac4a6cb2b5734c1}}

and every exported texture has a `.png.meta` beside it carrying that same GUID.
So the join is GUID to GUID, which is what the engine itself did.

THE SLOT NAMES VARY BY SHADER. Broken Arrow's own shaders use _Albedo/_Mask/
_Normal; the HDRP-derived ones use _BaseMap/_MaskMap/_BumpMap, and a few use
_MainTex/_BumpMap. All three spellings are accepted and folded onto the same
three roles, because from the browser's point of view they are the same thing
and a material that used the other spelling would otherwise silently report no
textures at all.
"""
import argparse
import csv
import os
import re
from pathlib import Path

# Slot spellings seen across this game's shaders, folded onto three roles. Order
# matters within a role: the first spelling present wins, so a material that
# sets both _BaseMap and _MainTex resolves to the one the shader actually reads.
SLOTS = {
    'albedo': ('_Albedo', '_BaseMap', '_MainTex', '_BaseColorMap'),
    'mask':   ('_Mask', '_MaskMap', '_SpecGlossMap', '_MetallicGlossMap'),
    'normal': ('_Normal', '_BumpMap', '_NormalMap'),
}

GUID_RE = re.compile(r'guid:\s*([0-9a-f]{32})')


def texture_guids(ripped):
    """GUID -> PNG filename, from the .meta beside every exported texture."""
    out = {}
    tex_dir = Path(ripped) / 'ExportedProject' / 'Assets' / 'Texture2D'
    if not tex_dir.is_dir():
        return out
    for meta in tex_dir.glob('*.meta'):
        asset = meta.with_suffix('')          # strip .meta -> Foo.png
        if asset.suffix.lower() != '.png':
            continue
        try:
            head = meta.read_text(encoding='utf-8', errors='replace')[:400]
        except OSError:
            continue
        m = GUID_RE.search(head)
        if m:
            out[m.group(1)] = asset.name
    return out


def material_textures(mat_path):
    """{role: guid} for one .mat, reading only its m_TexEnvs block.

    Parsed with a scanner rather than a YAML library on purpose: Unity's
    serialised YAML uses tags and anchors that trip general parsers, and all
    that is needed here is "which GUID sits under which slot name".
    """
    try:
        text = mat_path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return {}
    body = text.split('m_TexEnvs:', 1)
    if len(body) < 2:
        return {}
    body = body[1].split('m_Ints:', 1)[0]

    # slot -> guid, for every slot the material declares
    found = {}
    current = None
    for line in body.splitlines():
        stripped = line.strip()
        if stripped.endswith(':') and stripped.startswith('_'):
            current = stripped[:-1]
            continue
        if current and 'm_Texture:' in stripped:
            m = GUID_RE.search(stripped)
            if m:
                found[current] = m.group(1)
            current = None

    out = {}
    for role, spellings in SLOTS.items():
        for s in spellings:
            if s in found:
                out[role] = found[s]
                break
    return out


def from_bundles(bundle_dir, verbose=True):
    """{material: {role: texture name}} read straight out of the addressables.

    WHY THIS EXISTS ALONGSIDE THE .mat ROUTE. AssetRipper was pointed at
    `units_assets_all` alone, and it said so at load time - two
    `Dependency 'archive:/CAB-...' wasn't found` lines - which means the
    materials living in the other bundles never reached the exported project.
    The symptom is 53 vehicles whose glTF names a material like `RU_BMP1_1`
    that simply is not in the csv, and therefore render untextured while
    everything around them works.

    Re-ripping the whole 10 GB set to fix that is a lot of machinery for a
    table of names. UnityPy reads Material objects out of any bundle directly,
    and a Material's m_TexEnvs holds a PPtr to its Texture2D, whose m_Name is
    the same name the sweep wrote its PNG under. So this walks the bundles and
    resolves the pointers - the same GUID-equivalent join, one layer lower.
    """
    import UnityPy

    out = {}
    files = sorted(p for p in Path(bundle_dir).glob('*.bundle')
                   if 'cutscene' not in p.name.lower())
    for i, f in enumerate(files):
        try:
            env = UnityPy.load(str(f))
        except Exception as e:
            if verbose:
                print(f'  {f.name}: {e}')
            continue
        for obj in env.objects:
            if obj.type.name != 'Material':
                continue
            try:
                d = obj.read_typetree()
            except Exception:
                continue
            envs = (d.get('m_SavedProperties') or {}).get('m_TexEnvs') or []
            # m_TexEnvs is a list of (slot name, {m_Texture: PPtr}) pairs.
            found = {}
            for entry in envs:
                if isinstance(entry, (list, tuple)) and len(entry) == 2:
                    slot, val = entry
                else:
                    continue
                ptr = (val or {}).get('m_Texture') or {}
                pid = ptr.get('m_PathID')
                if pid:
                    found[slot] = pid
            if not found:
                continue
            # EVERY slot is kept, not just the ones with recognised names. The
            # layered vehicle shaders name their properties by hash -
            # `Layer_A97CDC25` - so a lookup driven by slot spelling finds
            # nothing on exactly the assets that were missing textures. The
            # texture's own NAME still ends in BaseMap / MaskMap / Normal,
            # which is the convention the artists actually kept, so the role is
            # decided from that instead. Slot names are still consulted first
            # where they are meaningful, because they are the stronger signal.
            out[d.get('m_Name')] = found
        if verbose and (i + 1) % 10 == 0:
            print(f'  {i + 1}/{len(files)} bundles, {len(out)} materials', flush=True)

    # PathID -> texture name, gathered in the same walk would double the work,
    # so it is a second pass over the same files with a cheaper filter.
    names = {}
    for f in files:
        try:
            env = UnityPy.load(str(f))
        except Exception:
            continue
        for obj in env.objects:
            if obj.type.name == 'Texture2D':
                try:
                    names[obj.path_id] = obj.read().m_Name
                except Exception:
                    pass
    resolved = {}
    for mat, slots in out.items():
        row = {}
        # 1. Slot names, where the shader used the standard ones.
        for role, spellings in SLOTS.items():
            for s in spellings:
                if s in slots:
                    row[role] = names.get(slots[s], '')
                    break
        # 2. Texture names, for everything else. Only fills roles still empty,
        #    so a shader that named its slots properly is never overridden by a
        #    guess from a filename.
        for slot, pid in slots.items():
            tex = names.get(pid, '')
            role = role_from_texture_name(tex)
            if role and not row.get(role):
                row[role] = tex
        resolved[mat] = {k: v for k, v in row.items() if v}
    return resolved


def role_from_texture_name(name):
    """albedo / mask / normal from a texture's own name, or None.

    Broken Arrow's art keeps the URP/HDRP suffixes even where the shader
    property does not - `Vilkas_MASE_BaseMap`, `..._MaskMap`, `..._Normal` -
    so the filename is a reliable last resort when the slot is called
    `Layer_A97CDC25`.
    """
    n = (name or '').lower()
    if not n:
        return None
    if 'normal' in n or n.endswith('_nrm'):
        return 'normal'
    if 'maskmap' in n or n.endswith('_mask'):
        return 'mask'
    if 'basemap' in n or 'albedo' in n or 'basecolor' in n or 'diffuse' in n:
        return 'albedo'
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--ripped', default='D:/ba_extracted/_ripper_units')
    ap.add_argument('--out', default='D:/ba_extracted/ba_materials.csv')
    ap.add_argument('--bundles', default='',
                    help='also scan this addressables folder with UnityPy, for '
                         'the materials the ripped project is missing')
    args = ap.parse_args()

    guids = texture_guids(args.ripped)
    print(f'{len(guids)} textures indexed by GUID')

    mat_dir = Path(args.ripped) / 'ExportedProject' / 'Assets' / 'Material'
    mats = sorted(mat_dir.glob('*.mat'))
    print(f'{len(mats)} materials')

    rows, resolved, dangling = [], 0, 0
    for m in mats:
        tex = material_textures(m)
        row = {'material': m.stem}
        for role in ('albedo', 'mask', 'normal'):
            g = tex.get(role)
            png = guids.get(g) if g else None
            if g and not png:
                dangling += 1
            row[role] = png or ''
        if any(row[r] for r in ('albedo', 'mask', 'normal')):
            resolved += 1
        rows.append(row)

    if args.bundles:
        print('scanning bundles for the materials the ripped project lacks...')
        extra = from_bundles(args.bundles)
        by_name = {r['material']: r for r in rows}
        added = filled = 0
        for mat, roles in extra.items():
            png = {r: (roles.get(r) + '.png' if roles.get(r) else '')
                   for r in ('albedo', 'mask', 'normal')}
            if not any(png.values()):
                continue
            row = by_name.get(mat)
            if row is None:
                row = {'material': mat, 'albedo': '', 'mask': '', 'normal': ''}
                rows.append(row)
                by_name[mat] = row
                added += 1
            # MERGE PER ROLE, never "skip if the material is already known".
            # The .mat route produces a row for every material including the
            # ones whose slots it could not recognise - the hash-named layered
            # shaders - so a whole-row skip means those rows stay empty forever
            # and the bundle data that would fill them is thrown away. That is
            # what left 49 vehicles untextured after the first bundle pass:
            # the rows existed, so nothing was "new".
            before = sum(1 for r in ('albedo', 'mask', 'normal') if row[r])
            for r in ('albedo', 'mask', 'normal'):
                if not row[r] and png[r]:
                    row[r] = png[r]
            after = sum(1 for r in ('albedo', 'mask', 'normal') if row[r])
            if after > before:
                filled += 1
        print(f'  {len(extra)} materials in bundles, {added} new rows, '
              f'{filled} rows gained a map')

    os.makedirs(os.path.dirname(args.out) or '.', exist_ok=True)
    with open(args.out, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=['material', 'albedo', 'mask', 'normal'])
        w.writeheader()
        w.writerows(rows)

    # Dangling GUIDs are worth printing rather than swallowing: they mean a
    # material points at a texture that is not in this export, which is a real
    # gap in coverage and not something to discover later as a blank preview.
    print(f'wrote {args.out}: {len(rows)} rows, {resolved} with at least one map, '
          f'{dangling} texture links unresolved')


if __name__ == '__main__':
    main()
