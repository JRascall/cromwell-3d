"""mercs_ini.py - the shipped text configuration, which is not a small thing.

WHY THIS EXISTS AT ALL

Forty INI files sit in plain text on the disc and were walked past for months
because nothing about a `.INI` suggests it holds anything the DSK archives do
not. They hold a great deal that the archives do not:

    [Atmosphere]     SunRGB SkyRGB HorizonRGB ZenithRGB AmbientRGB SunlightRGB
                     FogRGBA GlowRadius GlowDarken GlowAdd SkyTexture
                     SkyRotationsPerMinute RainChance SnowChance
    [TrafficZone]    327 zones with a density and their spawn objects
    [RadioMessage]   1,713 radio lines: id, sample, subtitle key, priority
    [Mail]           229 datapod emails with sender, subject, body, attachments
    [SoundAmbience]  ambience beds per environment per weather/combat state
    per level        layerDetail, Scrub, Dirt, detailType, terrainPhysicsType,
                     TerrainDust, ProcessRGBA
    CHARSTAT.INI     per-template armour and stealth multipliers

The atmosphere block is the global lighting rig - sun colour, ambient, fog,
sky gradient - which had been written off as absent. The traffic zones are the
callee side of the 395 Traffic_AddZoneSpawner calls in the mission Lua, and the
radio messages the callee side of the 201 Radio_QueueMessage calls. layerDetail
and Scrub are the terrain surface authoring the tern chunk does not carry.

THE FORMAT, AND WHY THE PARSER IS DELIBERATELY DUMB

There are two shapes mixed in one file and no way to tell them apart from a
line alone:

    [Section]                     a block, whose following Key value lines
    Density        800            belong to it
    Ambience  deserted TYPE_RAIN  ambient.dsrtd_rain      <- three columns
    Scrub     1  summer_env_shrubs02_night  0.8           <- three columns
    layerDetail    0  2                                   <- two columns

A directive is a keyword and then between one and four whitespace-separated
arguments, and the same keyword repeats. So the primary output is LONG - one
row per directive occurrence, every argument kept in its own column - which
cannot lose anything to a wrong guess about arity.

The wide per-section CSVs on top of that are a convenience for the blocks that
really are key/value records, and they are generated from whatever sections
turn out to be shaped that way rather than from a hard-coded list. A section
qualifies when its blocks are all single-argument Key value lines; anything
else stays long-form only.

Read-only research - see the header of mercs_dsk.py. These files are Pandemic's
configuration, not ours to redistribute; nothing here is committed.
"""
import argparse
import collections
import csv
import glob
import os
import sys

MAX_ARGS = 6


def parse(path):
    """-> [(section, directive, [args...]), ...] in file order."""
    out, section = [], ''
    with open(path, 'r', encoding='latin-1') as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith(';') or line.startswith('#'):
                continue
            if line.startswith('[') and line.endswith(']'):
                section = line[1:-1]
                out.append((section, '[SECTION]', []))
                continue
            parts = line.split()
            out.append((section, parts[0], parts[1:MAX_ARGS + 1]))
    return out


def blocks(rows, section):
    """Split one section's rows into records at each [SECTION] marker."""
    recs, cur = [], None
    for sec, key, args in rows:
        if sec != section:
            continue
        if key == '[SECTION]':
            if cur:
                recs.append(cur)
            cur = collections.OrderedDict()
        elif cur is not None:
            cur[key] = ' '.join(args)
    if cur:
        recs.append(cur)
    return recs


def widen(rows, section):
    """A section is widenable when every directive in it is single-valued.

    Checked rather than assumed: [SoundAmbience] looks like a block section and
    is really a list of three-column directives, and forcing it wide would
    silently keep the first column and drop the rest.
    """
    for sec, key, args in rows:
        if sec == section and key != '[SECTION]' and len(args) > 1:
            return None
    recs = blocks(rows, section)
    return recs if len(recs) > 1 else None


def dump(path, rows, keys=None):
    if not rows:
        return 0
    if keys is None:
        keys = []
        for r in rows:
            for k in r:
                if k not in keys:
                    keys.append(k)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        w = csv.DictWriter(f, fieldnames=keys, extrasaction='ignore')
        w.writeheader()
        w.writerows(rows)
    return len(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('root', help='the ripped DATAPS2 directory')
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    # Glob both cases for Linux, then dedupe by normalised path - on Windows
    # the two patterns match the SAME files and every record lands twice, which
    # shows up as exactly-doubled counts and nothing else.
    found = (glob.glob(os.path.join(args.root, '**', '*.INI'), recursive=True) +
             glob.glob(os.path.join(args.root, '**', '*.ini'), recursive=True))
    files, seen = [], set()
    for p in sorted(found):
        key = os.path.normcase(os.path.abspath(p))
        if key not in seen:
            seen.add(key)
            files.append(p)
    if not files:
        print('  no .INI found under %s' % args.root)
        return 1

    long_rows, per_section = [], collections.defaultdict(list)
    for path in files:
        rel = os.path.relpath(path, args.root).replace('\\', '/')
        rows = parse(path)
        for sec, key, a in rows:
            if key == '[SECTION]':
                continue
            row = {'file': rel, 'section': sec, 'directive': key}
            for i in range(MAX_ARGS):
                row['arg%d' % i] = a[i] if i < len(a) else ''
            long_rows.append(row)
        for sec in {s for s, _, _ in rows if s}:
            wide = widen(rows, sec)
            if wide:
                for r in wide:
                    r['file'] = rel
                per_section[sec].extend(wide)

    n = dump(os.path.join(args.out, 'ini_directives.csv'), long_rows)
    print('  %-26s %6d rows  (lossless, every directive)' % ('ini_directives.csv', n))
    for sec, recs in sorted(per_section.items(), key=lambda kv: -len(kv[1])):
        if len(recs) < 3:
            continue
        safe = ''.join(c if c.isalnum() or c in '_-' else '_' for c in sec).lower()
        n = dump(os.path.join(args.out, 'section_%s.csv' % safe), recs)
        print('  %-26s %6d records' % ('section_%s.csv' % safe, n))

    kinds = collections.Counter(r['directive'] for r in long_rows)
    print('  %d files, %d distinct directives' % (len(files), len(kinds)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
