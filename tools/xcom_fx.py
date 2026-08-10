"""xcom_fx.py - particle system definitions from the SDK.

`batchexport <pkg> ParticleSystem T3D` dumps a complete UE3 particle system:
every emitter, its LOD levels, and every module with its distributions. That
is the whole recipe - spawn rates, lifetimes, velocities, colour over life,
which material each emitter draws with - so the systems can be studied and
reimplemented rather than guessed at.

The textures those materials reference are already on disk: the model sweep
pulled 1,939 of them out of 476 FX_* packages.

STRUCTURE. A T3D is a nest of `Begin Object Class=X Name=Y ... End Object`
blocks, but containment is expressed by PROPERTY REFERENCE, not by nesting:

    Begin Object Class=ParticleSystem ...
       Emitters(0)=ParticleSpriteEmitter'ParticleSpriteEmitter_1'
    Begin Object Class=ParticleSpriteEmitter Name=ParticleSpriteEmitter_1
       LODLevels(0)=ParticleLODLevel'ParticleLODLevel_1'
    Begin Object Class=ParticleLODLevel Name=ParticleLODLevel_1
       RequiredModule=ParticleModuleRequired'...'
       Modules(0)=ParticleModuleLifetime'...'

so the parser flattens every block into a name -> node map and then follows
those references. Values live one level deeper again, in Distribution objects
(`Constant=`, or `Min=`/`Max=` for uniforms), which is why reading a "spawn
rate" means walking three hops.

    py -3 tools/xcom_fx.py scan  --pkg NAME --dump DIR --out TSV
    py -3 tools/xcom_fx.py build --scans DIR --out CSV
"""

from __future__ import annotations

import argparse
import csv
import re
from collections import Counter
from pathlib import Path

BEGIN_RE = re.compile(r'^\s*Begin Object Class=(\S+) Name=(\S+)')
END_RE = re.compile(r'^\s*End Object\s*$')
PROP_RE = re.compile(r'^\s*([A-Za-z_][\w()\[\]]*)\s*=\s*(.+?)\s*$')
REF_RE = re.compile(r"^[A-Za-z_]\w*'([^']+)'$")
ARRAY_RE = re.compile(r'^(\w+)\((\d+)\)$')


def parse_t3d(text: str) -> dict[str, dict]:
    """Flatten every Begin/End Object block into {name: {class, props}}."""
    nodes: dict[str, dict] = {}
    stack: list[dict] = []
    for line in text.splitlines():
        b = BEGIN_RE.match(line)
        if b:
            node = {'class': b.group(1), 'name': b.group(2), 'props': {}}
            nodes[b.group(2)] = node
            stack.append(node)
            continue
        if END_RE.match(line):
            if stack:
                stack.pop()
            continue
        if not stack:
            continue
        p = PROP_RE.match(line)
        if p:
            # Later duplicates are inner objects re-declaring defaults; the
            # first value seen on a node is the one that belongs to it.
            stack[-1]['props'].setdefault(p.group(1), p.group(2))
    return nodes


def deref(nodes, value):
    m = REF_RE.match(value or '')
    return nodes.get(m.group(1)) if m else None


def array_refs(node, key):
    """Collect Key(0), Key(1), ... in order."""
    out = []
    for k, v in node['props'].items():
        m = ARRAY_RE.match(k)
        if m and m.group(1) == key:
            out.append((int(m.group(2)), v))
    return [v for _, v in sorted(out)]


LOOKUP_RE = re.compile(r'LookupTable=\(([-\d.eE,\s]*)\)')


def distribution_value(node, key) -> str:
    """A module's value, read from the baked LookupTable.

    Values are structs, not plain references:

        Lifetime=(Distribution=DistributionFloatConstant'...',
                  LookupTable=(1.850000,1.850000,1.850000,1.850000))

    Chasing the Distribution object is both harder and unreliable - those inner
    objects reuse names heavily (43 copies of one name in a single file), so a
    flat name map collides. The LookupTable is UE3's own baked evaluation of
    the distribution and needs no lookup at all: constant when every entry
    matches, a range otherwise.
    """
    m = LOOKUP_RE.search(node['props'].get(key, '') if node else '')
    if not m:
        return ''
    try:
        vals = [float(x) for x in m.group(1).split(',') if x.strip()]
    except ValueError:
        return ''
    if not vals:
        return ''
    lo, hi = min(vals), max(vals)
    return f"{lo:g}" if abs(hi - lo) < 1e-6 else f"{lo:g}..{hi:g}"


def scan(dump: Path, pkg: str, out: Path):
    systems, emitters = [], []
    for f in sorted(dump.glob('*.T3D')):
        nodes = parse_t3d(f.read_text(errors='replace'))
        roots = [n for n in nodes.values() if n['class'] == 'ParticleSystem']
        for sysn in roots:
            sysname = f.stem
            erefs = array_refs(sysn, 'Emitters')
            nmod = 0
            for ei, eref in enumerate(erefs):
                em = deref(nodes, eref)
                if not em:
                    continue
                lods = array_refs(em, 'LODLevels')
                lod = deref(nodes, lods[0]) if lods else None
                if not lod:
                    continue
                req = deref(nodes, lod['props'].get('RequiredModule', ''))
                spawn = deref(nodes, lod['props'].get('SpawnModule', ''))
                td = deref(nodes, lod['props'].get('TypeDataModule', ''))
                mods = [deref(nodes, r) for r in array_refs(lod, 'Modules')]
                mods = [m for m in mods if m]
                nmod += len(mods)

                material = ''
                if req:
                    mm = REF_RE.match(req['props'].get('Material', ''))
                    material = mm.group(1) if mm else req['props'].get('Material', '')

                by_class = {m['class']: m for m in mods}
                lt = by_class.get('ParticleModuleLifetime')
                sz = by_class.get('ParticleModuleSize')
                vel = by_class.get('ParticleModuleVelocity')

                emitters.append({
                    'package': pkg, 'system': sysname,
                    'emitter': em['props'].get('EmitterName', f'Emitter{ei}').strip('"'),
                    'type': (td['class'].replace('ParticleModuleTypeData', '')
                             if td else 'Sprite'),
                    'material': material,
                    'spawn_rate': distribution_value(spawn, 'Rate'),
                    'lifetime': distribution_value(lt, 'Lifetime'),
                    'start_size': distribution_value(sz, 'StartSize'),
                    'start_velocity': distribution_value(vel, 'StartVelocity'),
                    'lods': len(lods),
                    'modules': len(mods),
                    'module_list': ';'.join(
                        m['class'].replace('ParticleModule', '') for m in mods),
                })
            systems.append({'package': pkg, 'system': sysname,
                            'emitters': len(erefs), 'modules': nmod})

    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open('w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh, delimiter='\t')
        for s in systems:
            w.writerow(['S', s['package'], s['system'], s['emitters'], s['modules']])
        for e in emitters:
            w.writerow(['E', e['package'], e['system'], e['emitter'], e['type'],
                        e['material'], e['spawn_rate'], e['lifetime'],
                        e['start_size'], e['start_velocity'],
                        e['lods'], e['modules'], e['module_list']])
    print(f"{pkg}: {len(systems)} systems, {len(emitters)} emitters")
    return 0


def build(scans: Path, out: Path):
    systems, emitters = [], []
    for tsv in sorted(scans.glob('*.tsv')):
        for row in csv.reader(tsv.open(encoding='utf-8'), delimiter='\t'):
            if not row:
                continue
            if row[0] == 'S' and len(row) >= 5:
                systems.append(dict(zip(['package', 'system', 'emitters', 'modules'], row[1:5])))
            elif row[0] == 'E' and len(row) >= 13:
                emitters.append(dict(zip(
                    ['package', 'system', 'emitter', 'type', 'material', 'spawn_rate',
                     'lifetime', 'start_size', 'start_velocity', 'lods', 'modules', 'module_list'], row[1:13])))
    if not systems:
        print(f"No scans in {scans}")
        return 1
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open('w', newline='', encoding='utf-8') as fh:
        w = csv.DictWriter(fh, fieldnames=['package', 'system', 'emitters', 'modules'])
        w.writeheader(); w.writerows(systems)
    em = out.with_name(out.stem + '_emitters.csv')
    with em.open('w', newline='', encoding='utf-8') as fh:
        w = csv.DictWriter(fh, fieldnames=['package', 'system', 'emitter', 'type',
                                           'material', 'spawn_rate', 'lifetime',
                                           'start_size', 'start_velocity',
                                           'lods', 'modules', 'module_list'])
        w.writeheader(); w.writerows(emitters)
    print(f"{len(systems)} particle systems from {len({s['package'] for s in systems})} packages -> {out}")
    print(f"{len(emitters)} emitters -> {em}")
    print("  emitter types: " + ", ".join(
        f"{k}={v}" for k, v in Counter(e['type'] for e in emitters).most_common(6)))
    mods = Counter(m for e in emitters for m in e['module_list'].split(';') if m)
    print("  commonest modules: " + ", ".join(f"{k}={v}" for k, v in mods.most_common(8)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('scan')
    s.add_argument('--pkg', required=True)
    s.add_argument('--dump', required=True, type=Path)
    s.add_argument('--out', required=True, type=Path)
    b = sub.add_parser('build')
    b.add_argument('--scans', required=True, type=Path)
    b.add_argument('--out', required=True, type=Path)
    a = ap.parse_args(argv)
    return scan(a.dump, a.pkg, a.out) if a.cmd == 'scan' else build(a.scans, a.out)


if __name__ == '__main__':
    raise SystemExit(main())
