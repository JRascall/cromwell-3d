"""Index and query what the Siege archives contain.

Siege ships no asset names (see study/games/shooters/rainbow_six_formats.md), so the library has
to be navigable by property instead: what type an asset is, how big it is, and
which archive it came from. That last one carries more information than it looks
like -- the archive name says map, season or DLC (`pvp04_clubhouse`, `set01`,
`mtx`, `evn12_rengoku`), which is most of the context a name would have given.

This is the Siege equivalent of xcom_index.py, and the query language is
deliberately the same shape.

    py -3 tools/r6/r6_index.py --game "<install>" --list
    py -3 tools/r6/r6_index.py --game "<install>" --build --out r6_extracted/index.csv
    py -3 tools/r6/r6_index.py --library r6_extracted --query "mesh"
    py -3 tools/r6/r6_index.py --library r6_extracted --query "texture>500000"

--list reads only the 307 headers and is instant. --build has to walk every
archive: the v34 file allocation table is encrypted, so entries are found by
scanning, and typing each asset means inflating the first chunk of its payload.
Budget an hour and use --slice/--of to spread it over several processes.
"""
import argparse
import collections
import csv
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r6_forge as R

# Only the types actually seen in the shipped build are named here. The 2021-era
# table that RainbowForge carries lists ~840 class IDs, but most of the IDs this
# build uses are not in it -- Ubisoft has added and renumbered classes since -- so
# an unknown ID is recorded as its hex rather than guessed at.
TYPE_NAMES = {
    0xABEB2DFB: 'CompiledMeshObject',
    0x9231EE0F: 'CompiledMeshShapeDataObject',
    0xD7B5C478: 'TextureLow',
    0xF9C80707: 'TextureMedium',
    0x59CE4D13: 'TextureHigh',
    0x9F492D22: 'TextureUltra',
    0x3876CCDF: 'TextureFuture',
    0x9468B9E2: 'GuiTextureLow',
    0x05A61FAD: 'GuiTextureMedium',
    0x427411A3: 'CompiledSoundMedia',
    0x82688E42: 'CompiledSoundBank',
    0xD16E3EBE: 'GIStream',
    0x569859AA: 'WorldData',          # dominates the 40 map archives
}

KIND = {
    'CompiledMeshObject': 'mesh',
    'CompiledMeshShapeDataObject': 'meshshape',
    'TextureLow': 'texture', 'TextureMedium': 'texture',
    'TextureHigh': 'texture', 'TextureUltra': 'texture',
    'TextureFuture': 'texture',
    'GuiTextureLow': 'guitexture', 'GuiTextureMedium': 'guitexture',
    'CompiledSoundMedia': 'sound', 'CompiledSoundBank': 'soundbank',
    'GIStream': 'gi', 'WorldData': 'world',
}


def type_name(tid):
    return TYPE_NAMES.get(tid, f'0x{tid:08X}')


def archive_tag(name):
    """The map / season / content-drop hint carried by an archive's filename."""
    m = re.search(r'(pvp\d+_\w+?)(?:_v\d+)?(?:_mod)?$', name)
    if m:
        return m.group(1)
    for tag in ('onb00_shootingrange', 'tdm01_mtown', 'r6_menuworld', 'ui_playgo'):
        if tag in name:
            return tag
    m = re.search(r'(evn\d+_\w+|lgm\d+_\w+)', name)
    if m:
        return m.group(1)
    for tag in ('mtx', 'set01', 'set02', 'playgo', 'events', 'ondemand', 'dmtx'):
        if tag in name:
            return tag
    return 'core'


def list_archives(game):
    rows = []
    for p in sorted(glob.glob(os.path.join(game, '*.forge'))):
        try:
            h = R.read_header(p)
        except Exception as e:
            print(f'  !! {os.path.basename(p)}: {e}')
            continue
        if h is None:
            continue
        rows.append((os.path.basename(p)[:-6], h['version'], h['num_entries'],
                     os.path.getsize(p)))

    rows.sort(key=lambda r: -r[3])
    print(f'{"archive":58s} {"ver":>3s} {"entries":>9s} {"size":>10s}  tag')
    tot_e = tot_b = 0
    for name, ver, n, size in rows:
        tot_e += n
        tot_b += size
        print(f'{name[:58]:58s} {ver:3d} {n:9,d} {size / 2**20:9.1f}M  {archive_tag(name)}')
    print(f'\n{len(rows)} archives, {tot_e:,} entries, {tot_b / 2**30:.1f} GB')
    by_ver = collections.Counter(r[1] for r in rows)
    print('versions:', dict(by_ver))


def build(game, out, slice_i, slice_n, limit):
    files = sorted(glob.glob(os.path.join(game, '*.forge')))
    if slice_n > 1:
        files = [p for i, p in enumerate(files) if i % slice_n == slice_i]

    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    n_assets = 0
    with open(out, 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh)
        w.writerow(['archive', 'tag', 'version', 'uid', 'offset', 'size',
                    'type_id', 'type', 'kind'])
        for p in files:
            name = os.path.basename(p)[:-6]
            try:
                ents, h = R.entries(p, max_entries=limit)
            except Exception as e:
                print(f'  !! {name}: {e}')
                continue

            tag = archive_tag(name)
            f = open(p, 'rb')
            wrote = 0
            for uid, off, size in ents:
                f.seek(off)
                buf = f.read(min(size, 1 << 20))

                # A soundmedia archive holds bare .wem, not containers; type it
                # from the RIFF rather than failing to parse a container header.
                if buf[:4] == b'RIFF' and buf[8:12] == b'WAVE':
                    tid, tname = 0x427411A3, 'CompiledSoundMedia'
                else:
                    try:
                        _meta, payload = R.read_container(buf, want_bytes=256)
                        mm = R.asset_meta(payload)
                    except Exception:
                        mm = None
                    if not mm:
                        continue
                    tid = mm[0]
                    tname = type_name(tid)

                w.writerow([name, tag, h['version'], uid, off, size,
                            f'0x{tid:08X}', tname, KIND.get(tname, 'other')])
                wrote += 1
            f.close()
            n_assets += wrote
            print(f'  {name[:56]:56s} {wrote:7,d}')
    print(f'\n{len(files)} archives -> {n_assets:,} assets -> {out}')


def query(library, expr):
    path = library if library.endswith('.csv') else os.path.join(library, 'index.csv')
    rows = list(csv.DictReader(open(path, encoding='utf-8')))
    m = re.match(r'^\s*(\w[\w.]*)\s*(>|<|=)?\s*(\d+)?\s*$', expr)
    if not m:
        print('query looks like "mesh", "texture>500000", "pvp04_clubhouse"')
        return
    term, op, num = m.group(1).lower(), m.group(2), m.group(3)

    # An exact kind wins outright. Otherwise "mesh" also drags in every
    # meshshape archive by substring, which is not what anyone means by it.
    kinds = {k.lower() for k in KIND.values()}
    if term in kinds:
        sel = [r for r in rows if r['kind'].lower() == term]
    else:
        sel = [r for r in rows
               if term == r['tag'].lower()
               or term in r['type'].lower() or term in r['archive'].lower()]
    if op and num:
        n = int(num)
        sel = [r for r in sel
               if (int(r['size']) > n if op == '>' else
                   int(r['size']) < n if op == '<' else int(r['size']) == n)]

    print(f'{len(sel):,} of {len(rows):,} assets match "{expr}"')
    by_arch = collections.Counter(r['archive'] for r in sel)
    for a, c in by_arch.most_common(15):
        print(f'   {a[:58]:58s} {c:7,d}')
    if len(by_arch) > 15:
        print(f'   ... and {len(by_arch) - 15} more archives')
    total = sum(int(r['size']) for r in sel)
    print(f'   {total / 2**30:.2f} GB of packed payload')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=r"C:\Program Files (x86)\Steam\steamapps"
                                      r"\common\Tom Clancy's Rainbow Six Siege")
    ap.add_argument('--list', action='store_true', help='archive table; reads headers only')
    ap.add_argument('--build', action='store_true')
    ap.add_argument('--out', default='r6_extracted/index.csv')
    ap.add_argument('--library', help='directory holding index.csv, or the csv itself')
    ap.add_argument('--query')
    ap.add_argument('--slice', type=int, default=0)
    ap.add_argument('--of', type=int, default=1)
    ap.add_argument('--limit', type=int, default=None,
                    help='sample only: stops the scan early, so the index is partial')
    a = ap.parse_args()

    if a.list:
        list_archives(a.game)
    elif a.build:
        build(a.game, a.out, a.slice, a.of, a.limit)
    elif a.query:
        query(a.library or 'r6_extracted', a.query)
    else:
        ap.print_help()


if __name__ == '__main__':
    main()
