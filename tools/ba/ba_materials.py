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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--ripped', default='D:/ba_extracted/_ripper_units')
    ap.add_argument('--out', default='D:/ba_extracted/ba_materials.csv')
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
