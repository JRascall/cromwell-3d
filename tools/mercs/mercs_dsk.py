"""mercs_dsk.py - read Mercenaries' .DSK archives and the ucfb chunk tree inside.

Mercenaries: Playground of Destruction (Pandemic / LucasArts, PS2, 2004) runs
on RedEngine - the ELF names it outright, in ../../RedEngine/Source/. All of
its content sits in three archives the ELF opens by name (dataps2/streamed.dsk,
assets.dsk, locked.dsk) plus english.dsk for localisation, and every payload in
them is a ucfb chunk tree, the same container Pandemic used in Star Wars
Battlefront.

THE .DSK CONTAINER

    u32 count
    u32 reserved            zero in all four archives
    count * {
        u32 size
        u32 nameHash        the key the game looks assets up by
        u32 groupHash       which streaming group this copy belongs to
    }
    payloads, back to back, no per-entry alignment
    2048 bytes of zero

Payload N starts at 8 + count*12 + sum(sizes[0..N-1]). That arithmetic lands
exactly on the end of the file, minus the fixed 2048-byte tail, for all four
archives - which is the check that the layout is right rather than plausible.

NAMES ARE NOT STORED. The table has a hash where a filename would be, so
assets come out keyed by hash. Real names are recovered from inside the chunks
instead: tex_/NAME, modl/NAME, anim/NAME and segm/NAME all carry them, and
--inventory reports them. Nothing here tries to reverse the hash.

THE GROUP FIELD IS THE ASSET TYPE

    A name hash is NOT unique in the table. A model and its geometry carry the
    SAME name hash and differ only in the group, because the group is the kind
    of asset rather than a streaming bucket:

        STREAMED.DSK   4 groups, and exactly 4 chunk kinds  (tex_ modl CSEG anim)
        LOCKED.DSK     2 groups, and exactly 657 modl + 657 CSEG
        ASSETS.DSK    17 groups, and roughly that many chunk kinds

    So (nameHash, groupHash) is the real key. Deduplicating on the name hash
    alone silently discards every CSEG in the archive - which is exactly the
    mistake that made the geometry look absent on a first pass through this
    data, and why --extract dedups on the content digest instead.

    Note for anyone measuring: real duplication between payloads is under
    0.1%. There is no store-once-per-level seek optimisation here, and the gap
    between a 4.37 GB disc and 2 GB of files is DVD padding plus 1.2 GB of
    .PSS movies, not redundancy.

WHAT IS IN THERE

    tex_   4643  122.8 MB   textures: INFO dims, raw palettes, compressed pixels
    CSEG   2941   29.4 MB   mesh geometry, compressed
    anim   1835   13.2 MB   animation, uncompressed
    wrld    824   13.5 MB   world/instance placement
    tern     14    9.3 MB   terrain
    modl   2954    1.8 MB   skeletons, node hierarchy, shadow volumes
    path    346    1.2 MB   paths and splines
    reg_     91    1.1 MB   localised text (ENGLISH.DSK)
    scr_/enc_/atbl/rgns     scripts, encounters, animation tables, regions

    tex_/INFO is {u16 width; u16 height; u16 bpp; u16 mipCount}. Palettes are
    raw: a count byte, then A,R,G,B quads - 16-entry tables are 1+64 bytes and
    256-entry ones 2+1024, which is how the split between header and data was
    settled.

WHAT IS NOT SOLVED. Texture pixels and CSEG geometry are both behind one
bespoke LZ compressor, headed {u32 decompressedSize; u32 compressedSize} with
compressedSize == 0xFFFFFFFF meaning stored. Two of the 2297 CSEGs are stored,
and those are what pinned the inner segm layout down. The compressor itself is
not deflate and not flag-word LZSS - both were tried and both contradict the
known output. Until it is read, this module extracts those chunks in their
compressed form and everything else in full.

Read-only research. Pandemic's art is not ours to redistribute, none of it is
committed or shipped, and this exists to learn the shape of the data - the same
rule this repo already applies to XCOM, Siege, UNIGINE and SimCity 3000.
"""
import argparse
import collections
import hashlib
import os
import struct
import sys

TAIL_PAD = 2048
STORED = 0xFFFFFFFF


# --------------------------------------------------------------- container
def read_table(data):
    """Return [(size, name_hash, group_hash, offset), ...] for a .DSK image."""
    count, reserved = struct.unpack_from('<II', data, 0)
    entries = []
    offset = 8 + count * 12
    for i in range(count):
        size, name_hash, group_hash = struct.unpack_from('<III', data, 8 + i * 12)
        entries.append((size, name_hash, group_hash, offset))
        offset += size
    return count, reserved, entries


def check(path, data, entries):
    """The layout proof: payloads must end exactly TAIL_PAD from the end."""
    if not entries:
        return True, 'empty'
    end = entries[-1][3] + entries[-1][0]
    slack = len(data) - end
    return slack == TAIL_PAD, 'slack=%d (expected %d)' % (slack, TAIL_PAD)


# ------------------------------------------------------------ ucfb walking
def children(buf):
    """Immediate child chunks of a ucfb payload, 4-byte aligned. No recursion."""
    out, p = [], 0
    while p + 8 <= len(buf):
        tag = buf[p:p + 4]
        size = struct.unpack_from('<I', buf, p + 4)[0]
        if p + 8 + size > len(buf):
            break
        out.append((tag.decode('ascii', 'replace'), buf[p + 8:p + 8 + size]))
        p = (p + 8 + size + 3) & ~3
    return out


def root_of(blob):
    """(tag, payload) of the single chunk inside a payload's ucfb wrapper."""
    if len(blob) < 16 or blob[:4] != b'ucfb':
        return None, b''
    size = struct.unpack_from('<I', blob, 4)[0]
    inner = blob[8:8 + size]
    kids = children(inner)
    return (kids[0] if kids else (None, b''))


def chunk_name(tag, payload):
    """The asset's real name, if this chunk carries one."""
    for k, v in children(payload):
        if k == 'NAME':
            return v.split(b'\0')[0].decode('ascii', 'replace')
    return ''


def tex_info(payload):
    """(width, height, bpp, mips) for a tex_ chunk, or None."""
    for k, v in children(payload):
        if k == 'INFO' and len(v) >= 8:
            return struct.unpack_from('<HHHH', v, 0)
    return None


# -------------------------------------------------------------- operations
def inventory(paths):
    for path in paths:
        with open(path, 'rb') as f:
            data = f.read()
        count, reserved, entries = read_table(data)
        ok, why = check(path, data, entries)
        print('=' * 72)
        print('%s  %s bytes  %d entries  %s'
              % (os.path.basename(path), format(len(data), ','), count,
                 'layout OK' if ok else 'LAYOUT MISMATCH: ' + why))

        by_tag = collections.Counter()
        bytes_by_tag = collections.Counter()
        groups = collections.Counter()
        seen, dup_bytes = set(), 0
        texshapes = collections.Counter()

        for size, nh, gh, off in entries:
            groups[gh] += 1
            blob = data[off:off + size]
            digest = hashlib.md5(blob).digest()
            if digest in seen:
                dup_bytes += size
                continue
            seen.add(digest)
            tag, payload = root_of(blob)
            if tag is None:
                by_tag['(raw text/data)'] += 1
                bytes_by_tag['(raw text/data)'] += size
                continue
            by_tag[tag] += 1
            bytes_by_tag[tag] += size
            if tag == 'tex_':
                ti = tex_info(payload)
                if ti:
                    texshapes[(ti[0], ti[1], ti[2])] += 1

        total = sum(e[0] for e in entries)
        print('  groups: %d    unique payloads: %d    duplicated: %s of %s (%.0f%%)'
              % (len(groups), len(seen), format(dup_bytes, ','),
                 format(total, ','), 100.0 * dup_bytes / max(1, total)))
        print('  %-18s %7s %12s' % ('CHUNK', 'COUNT', 'MB'))
        for tag, n in by_tag.most_common():
            print('  %-18s %7d %12.2f' % (tag, n, bytes_by_tag[tag] / 1048576.0))
        if texshapes:
            print('  texture shapes (w, h, bpp):')
            for shape, n in texshapes.most_common(10):
                print('     %-20s x%d' % (str(shape), n))


def extract(paths, out_dir, want=None):
    """Write one file per UNIQUE payload, named <tag>/<name or hash>.

    Dedup is on the content digest, not the name hash. That matters: a model
    and its geometry segment share a name, so deduplicating on the name hash
    silently discards every CSEG in the archive - which is exactly the mistake
    that made the geometry look absent on the first pass through this data.
    """
    os.makedirs(out_dir, exist_ok=True)
    written = collections.Counter()
    seen = set()
    for path in paths:
        with open(path, 'rb') as f:
            data = f.read()
        _, _, entries = read_table(data)
        for size, nh, gh, off in entries:
            blob = data[off:off + size]
            digest = hashlib.md5(blob).digest()
            if digest in seen:
                continue
            seen.add(digest)
            tag, payload = root_of(blob)
            if tag is None:
                tag, payload = 'raw', blob
            if want and tag not in want:
                continue
            name = chunk_name(tag, payload) if payload else ''
            safe = ''.join(c if c.isalnum() or c in '_-.' else '_' for c in name)
            stem = '%s_%08X' % (safe[:60], nh) if safe else '%08X' % nh
            sub = os.path.join(out_dir, tag.strip().replace('/', '_') or 'untagged')
            os.makedirs(sub, exist_ok=True)
            with open(os.path.join(sub, stem + '.chunk'), 'wb') as o:
                o.write(blob)
            written[tag] += 1
    for tag, n in written.most_common():
        print('  %-18s %6d files' % (tag, n))
    return sum(written.values())


def names(paths, out_file):
    """Dump every recovered asset name - the map from hash to something human."""
    rows = []
    seen = set()
    for path in paths:
        with open(path, 'rb') as f:
            data = f.read()
        _, _, entries = read_table(data)
        for size, nh, gh, off in entries:
            if nh in seen:
                continue
            blob = data[off:off + size]
            tag, payload = root_of(blob)
            if tag is None:
                continue
            n = chunk_name(tag, payload)
            if n:
                seen.add(nh)
                rows.append((n, tag, nh, size))
    rows.sort()
    with open(out_file, 'w', encoding='utf-8') as f:
        f.write('name\tchunk\tname_hash\tbytes\n')
        for n, tag, nh, size in rows:
            f.write('%s\t%s\t%08X\t%d\n' % (n, tag, nh, size))
    print('  %d named assets -> %s' % (len(rows), out_file))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dsk', nargs='+', help='one or more .DSK archives')
    ap.add_argument('--inventory', action='store_true',
                    help='report contents, verify the layout, write nothing')
    ap.add_argument('--extract', metavar='DIR', help='write unique payloads here')
    ap.add_argument('--names', metavar='TSV', help='dump recovered asset names')
    ap.add_argument('--only', metavar='TAG', action='append',
                    help='restrict --extract to these chunk tags (repeatable)')
    args = ap.parse_args()

    if args.inventory or not (args.extract or args.names):
        inventory(args.dsk)
    if args.names:
        names(args.dsk, args.names)
    if args.extract:
        n = extract(args.dsk, args.extract, set(args.only) if args.only else None)
        print('  %d unique payloads -> %s' % (n, args.extract))
    return 0


if __name__ == '__main__':
    sys.exit(main())
