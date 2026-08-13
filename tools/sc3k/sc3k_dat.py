# -*- coding: utf-8 -*-
"""Reader for SimCity 3000's archive container and its four record kinds.

Written for ../../study/games/strategy/simcity3000.md.  Python 3, needs numpy and Pillow.

    py -3 sc3k_dat.py --list  <file.DAT>          # table of contents
    py -3 sc3k_dat.py --stats <dir>               # record kinds across a tree
    py -3 sc3k_dat.py --extract <file.DAT> --out <dir>

One container serves the whole game.  `.DAT` (sprites), `.IXF` (interface
art), `.bld` (Building Architect kits), `.sc3`/`.SNR` (saved cities and
scenarios) and `BAT.DAT` are all the same file format with different payloads,
which is why there is one reader rather than five.

# The container

    0x00  uint32   0x80C381D7 magic
    0x04  entry[]  20 bytes each, terminated by an all-zero entry
    ....  payload  referenced by absolute offset; NOT in index order

An entry is five little-endian uint32s:

    group, instance, type, offset, size

`group` and `instance` are exactly the two columns of the `.SII` text manifests
that sit beside the sprite archives, which is what makes those manifests
readable as names for this data — see `read_sii`.  For sprite archives `type`
is 0 for the pixels and 1 for an 8-byte registration record; elsewhere it is a
Maxis type hash (interface art is all type 0x62B9DA24).

**The index is not sorted and the payload is not contiguous with it.**  In
`00000002_Residential.DAT` the index ends at 0xD498 and the first *referenced*
byte is at 0xF004, with records scattered over 5 MB in no particular order.  A
reader that assumes "index, then data, in order" appears to work — it reads the
first few sprites correctly — and then walks off into the middle of a payload.

# Records

Every payload is RefPack-compressed (see sc3k_refpack.py).  The fixed header in
front of the compressed stream varies in length by kind, so `record_payload`
finds the stream by its `10 FB` signature and *validates* the find against the
uint32 in front of it, which must equal the record's remaining length.  That is
cheaper and far more robust than a per-kind offset table, because the check
fails loudly on a kind this reader has not seen instead of decoding garbage.

`type` in the index does not identify the payload; the first uint32 of the
record does:

| word0 | kind | payload |
|---|---|---|
| 0x0107 | span sprite | RGB565, run-length encoded by row |
| 0x0105 | span sprite | RGB555, same encoding |
| 0x0003 | alpha | one byte per pixel, 0-31, uncompressed raster |
| 0x0002 | raster | flat uncompressed image, format in word4 |

The low byte is the pixel format (7 = RGB565, 5 = RGB555, 3 = 8-bit) and 0x100
means "row-span encoded".  That reading is consistent across all 55,439 sprite
records and both interface formats, but it is inferred from those two bits
moving together, not from anything the game states.

# Why sprites are stored as spans and what that says

A span sprite's decompressed payload is:

    0x00  uint32  total length, including this header
    0x04  uint16  width
    0x06  uint16  height
    0x08  uint16  4          (constant everywhere; unidentified)
    0x0A  uint16  pixel format, matching the low byte of word0
    0x0C  uint16  colour key — 0xF81F in 565, 0x7C1F in 555, both magenta
    0x0E  uint16  0
    0x10  row[]   8 bytes each, exactly `height` of them
    ....  pixels  packed, 2 bytes each, no padding

    row = uint32 first pixel index, uint16 x, uint16 flag|length

Each row contributes **one** horizontal span: `length` pixels starting at
column `x`, taken from the pixel pool at `first pixel index`.  Everything
outside the span is transparent.  Rows are contiguous in the pool — row N+1
starts where row N ended — so `first pixel index` is redundant with the lengths
and exists to make a row directly addressable.  That is what lets the game
clip a sprite vertically to the dirty rectangle without walking the rows above
it, which for a full-screen scroll of a city is most of the work avoided.

**The top bit of `length` is set when the span contains no key-coloured pixel.**
Checked over every sprite in the game: 100% correlation, no exceptions.  So it
is a per-row "this run is fully opaque" hint, and the blitter reads it to
choose between a straight copy and a per-pixel key test for that row.  It is
worth pausing on, because it is the same rule CLAUDE.md states as *cull cheaply
before testing expensively*, paid for at bake time rather than per frame: the
expensive test is per-pixel and the flag that avoids it costs one bit per row.
Across the sprite library the flag is set on 81% of rows.

Interior transparency is what makes it necessary at all — 46,316 key-coloured
pixels sit *inside* spans in the residential set alone (windows, arches, gaps
between railings), so the spans cannot be assumed opaque and the key test
cannot be dropped outright.

# Alpha

Translucency is a *separate record*, one byte per pixel with values 0-31, and
it lives at `instance - 1` from the colour record it belongs to.  Smoke, water
and the disaster effects have them; ordinary buildings do not.  The `.SII`
manifests mark these rows `; Alpha`, which is how the pairing was confirmed
rather than guessed.

5 bits of alpha, not 8, because the blitter is 16-bit throughout: a 5-bit
weight multiplies a 5-bit colour channel into 10 bits, which was the widest
product worth having on a 1999 CPU doing this per pixel.
"""
import argparse
import collections
import csv
import os
import re
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sc3k_refpack

MAGIC = 0x80C381D7

KIND_SPAN_565 = 0x0107
KIND_SPAN_555 = 0x0105
KIND_ALPHA = 0x0003
KIND_RASTER = 0x0002

KIND_NAMES = {
    KIND_SPAN_565: 'span565',
    KIND_SPAN_555: 'span555',
    KIND_ALPHA: 'alpha8',
    KIND_RASTER: 'raster',
}

Entry = collections.namedtuple('Entry', 'group instance type offset size')
Sprite = collections.namedtuple(
    'Sprite', 'group instance kind width height rgba opaque_rows total_rows')


# --------------------------------------------------------------------------
# container
# --------------------------------------------------------------------------

class Archive(object):
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as fh:
            self.buf = fh.read()
        if len(self.buf) < 4 or struct.unpack_from('<I', self.buf, 0)[0] != MAGIC:
            raise ValueError('%s: not a SimCity 3000 container' % path)
        self.entries = self._read_index()

    def _read_index(self):
        out = []
        pos = 4
        n = len(self.buf)
        while pos + 20 <= n:
            e = Entry(*struct.unpack_from('<5I', self.buf, pos))
            if e == (0, 0, 0, 0, 0):
                break
            # A truncated or misidentified file produces entries pointing
            # outside the file long before it produces a zero terminator.
            if e.offset + e.size > n:
                raise ValueError('%s: index entry %d runs past end of file'
                                 % (self.path, len(out)))
            out.append(e)
            pos += 20
        return out

    def record(self, entry):
        return self.buf[entry.offset:entry.offset + entry.size]

    def payload(self, entry):
        """Decompress a record.  Returns (word0, header_words, data)."""
        rec = self.record(entry)
        sig = _find_stream(rec)
        if sig is None:
            # Not every record is compressed: the 8-byte registration records
            # in sprite archives are stored raw, and so are a few tiny ones.
            return None, (), rec
        words = struct.unpack_from('<%dI' % ((sig - 4) // 4), rec, 0)
        return words[0], words, sc3k_refpack.decompress(rec, sig)


def _find_stream(rec):
    """Offset of the RefPack signature in `rec`, or None.

    Validated rather than merely located: the uint32 immediately before the
    signature is the compressed length *including itself*, so it must equal the
    number of bytes left in the record.  Two `10 FB` bytes can occur by chance
    inside a header, and without this check a chance hit decodes to a length
    mismatch far away from the real cause.
    """
    for off in range(8, min(len(rec), 64) - 1, 4):
        if sc3k_refpack.is_packed(rec, off):
            declared = struct.unpack_from('<I', rec, off - 4)[0]
            if declared == len(rec) - (off - 4):
                return off
    return None


# --------------------------------------------------------------------------
# pixel decoding
# --------------------------------------------------------------------------

def _rgb565(v):
    """uint16 array -> (h, w, 3) uint8.  Replicates high bits into low.

    `(x << 3) | (x >> 2)` rather than `x << 3`: a plain shift makes full white
    decode to 248, so every sprite comes out slightly grey and the error is
    invisible until two sources are composited.
    """
    r = ((v >> 11) & 0x1F).astype(np.uint16)
    g = ((v >> 5) & 0x3F).astype(np.uint16)
    b = (v & 0x1F).astype(np.uint16)
    return np.dstack([(r << 3) | (r >> 2),
                      (g << 2) | (g >> 4),
                      (b << 3) | (b >> 2)]).astype(np.uint8)


def _rgb555(v):
    r = ((v >> 10) & 0x1F).astype(np.uint16)
    g = ((v >> 5) & 0x1F).astype(np.uint16)
    b = (v & 0x1F).astype(np.uint16)
    return np.dstack([(r << 3) | (r >> 2),
                      (g << 3) | (g >> 2),
                      (b << 3) | (b >> 2)]).astype(np.uint8)


def decode_span(data):
    """Span-encoded sprite -> (rgba, opaque_rows, total_rows).

    rgba is (h, w, 4) uint8; alpha is 0 outside the spans and on key pixels.
    """
    total, w, h, const4, fmt, key, _pad = struct.unpack_from('<IHHHHHH', data, 0)
    if total != len(data):
        raise ValueError('span sprite declares %d bytes, decoded %d'
                         % (total, len(data)))
    pix_start = 16 + 8 * h
    if pix_start > len(data):
        raise ValueError('span sprite row table runs past the payload')

    rows = np.frombuffer(data, dtype='<u4,<u2,<u2', count=h, offset=16)
    index = rows['f0'].astype(np.int64)
    xs = rows['f1'].astype(np.int64)
    lens = (rows['f2'] & 0x7FFF).astype(np.int64)
    opaque = int((rows['f2'] >> 15).sum())

    npix = (len(data) - pix_start) // 2
    if int(lens.sum()) != npix:
        raise ValueError('span lengths total %d, pool holds %d pixels'
                         % (int(lens.sum()), npix))
    pool = np.frombuffer(data, dtype='<u2', count=npix, offset=pix_start)

    colour = _rgb565(pool) if fmt == 7 else _rgb555(pool)
    colour = colour.reshape(-1, 3)
    solid = (pool != key)

    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    for y in range(h):
        n = int(lens[y])
        if n == 0:
            continue
        x0, i0 = int(xs[y]), int(index[y])
        rgba[y, x0:x0 + n, :3] = colour[i0:i0 + n]
        rgba[y, x0:x0 + n, 3] = np.where(solid[i0:i0 + n], 255, 0)
    return rgba, opaque, h


def decode_raster(data, w, h, fmt):
    """Flat uncompressed image (interface art) -> (h, w, 4) uint8."""
    need = w * h
    pool = np.frombuffer(data, dtype='<u2', count=need, offset=0)
    colour = _rgb565(pool) if fmt == 7 else _rgb555(pool)
    key = 0xF81F if fmt == 7 else 0x7C1F
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    rgba[:, :, :3] = colour.reshape(h, w, 3)
    rgba[:, :, 3] = np.where(pool.reshape(h, w) != key, 255, 0)
    return rgba


def decode_alpha(data, w, h):
    """8-bit alpha record -> (h, w) uint8 scaled from the stored 0-31."""
    a = np.frombuffer(data, dtype=np.uint8, count=w * h).reshape(h, w)
    return ((a.astype(np.uint16) * 255) // 31).astype(np.uint8)


# --------------------------------------------------------------------------
# whole-archive decode
# --------------------------------------------------------------------------

def decode_archive(arc):
    """Decode every image in one archive.

    Returns (sprites, registrations, skipped) where `registrations` maps
    instance -> (left, top, right, bottom) spans around the sprite's origin and
    `skipped` counts records this reader does not decode.
    """
    raw = {}
    regs = {}
    skipped = collections.Counter()

    for e in arc.entries:
        if e.type == 1 and e.size == 8:
            regs[e.instance] = struct.unpack_from('<4H', arc.buf, e.offset)
            continue
        try:
            word0, words, data = arc.payload(e)
        except Exception as ex:
            skipped['unreadable: %s' % type(ex).__name__] += 1
            continue
        if word0 is None:
            skipped['uncompressed record'] += 1
            continue
        raw[e.instance] = (e, word0, words, data)

    sprites = []
    for inst, (e, word0, words, data) in sorted(raw.items()):
        try:
            if word0 in (KIND_SPAN_565, KIND_SPAN_555):
                rgba, opaque, nrows = decode_span(data)
            elif word0 == KIND_RASTER:
                w, h, fmt = words[2], words[3], words[4]
                rgba = decode_raster(data, w, h, fmt)
                opaque, nrows = h, h
            elif word0 == KIND_ALPHA:
                continue                       # emitted with its colour record
            else:
                skipped['unknown kind 0x%X' % word0] += 1
                continue
        except Exception as ex:
            skipped['decode failed: %s' % ex] += 1
            continue

        # Translucency lives in a separate record one instance below this one.
        mate = raw.get(inst - 1)
        if mate is not None and mate[1] == KIND_ALPHA:
            mw, mh = mate[2][2], mate[2][3]
            if (mh, mw) == rgba.shape[:2]:
                a = decode_alpha(mate[3], mw, mh)
                # The span mask still governs: a pixel outside every span is
                # not merely transparent, it has no colour at all, so the alpha
                # record must not resurrect it.
                rgba[:, :, 3] = np.minimum(rgba[:, :, 3], a)

        sprites.append(Sprite(e.group, inst, KIND_NAMES.get(word0, hex(word0)),
                              rgba.shape[1], rgba.shape[0], rgba,
                              opaque, nrows))
    return sprites, regs, skipped


# --------------------------------------------------------------------------
# the .SII manifests
# --------------------------------------------------------------------------

_SII_ROW = re.compile(
    r'^\s*([0-9A-Fa-f]{1,8})\s*,\s*(?:([0-9A-Fa-f]{1,8})\s*,\s*)?'
    r'(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*(?:;\s*(.*))?$')


def read_sii(path):
    """Parse a `.SII` image manifest -> {(group, instance): (spans, label)}.

    Two shapes exist and both appear in the shipped game: five columns
    (instance only — the group is the archive's) and six (group and instance).
    The trailing `;` comment is the only human-readable naming anywhere in the
    sprite data, so it is carried through to the index even though most rows
    have none.
    """
    out = {}
    with open(path, 'r', errors='replace') as fh:
        for line in fh:
            if line.lstrip().startswith(';') or ':' in line.split(';')[0]:
                continue
            m = _SII_ROW.match(line)
            if not m:
                continue
            g, i, l, t, r, b, label = m.groups()
            if i is None:
                group, inst = None, int(g, 16)
            else:
                group, inst = int(g, 16), int(i, 16)
            out[(group, inst)] = ((int(l), int(t), int(r), int(b)),
                                  (label or '').strip())
    return out


def sii_for(path):
    """The manifest beside an archive, if it ships one."""
    for ext in ('.sii', '.SII', '.Sii'):
        cand = os.path.splitext(path)[0] + ext
        if os.path.exists(cand):
            return read_sii(cand)
    return {}


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def containers_under(root):
    if os.path.isfile(root):
        return [root]
    found = []
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            p = os.path.join(dirpath, f)
            try:
                with open(p, 'rb') as fh:
                    if fh.read(4) == b'\xd7\x81\xc3\x80':
                        found.append(p)
            except OSError:
                pass
    return sorted(found)


def cmd_list(args):
    arc = Archive(args.path)
    print('%s: %d entries' % (os.path.basename(args.path), len(arc.entries)))
    kinds = collections.Counter()
    for e in arc.entries[:args.limit]:
        try:
            word0, words, data = arc.payload(e)
        except Exception as ex:
            print('  %08X/%08X type=%08X  ERROR %s' %
                  (e.group, e.instance, e.type, ex))
            continue
        if word0 is None:
            print('  %08X/%08X type=%08X  raw %d bytes' %
                  (e.group, e.instance, e.type, e.size))
            kinds['raw'] += 1
            continue
        name = KIND_NAMES.get(word0, 'kind 0x%X' % word0)
        kinds[name] += 1
        if word0 in (KIND_SPAN_565, KIND_SPAN_555):
            _t, w, h = struct.unpack_from('<IHH', data, 0)
        else:
            w, h = words[2], words[3]
        print('  %08X/%08X type=%08X  %-8s %4dx%-4d  %6d -> %6d bytes' %
              (e.group, e.instance, e.type, name, w, h, e.size, len(data)))
    print('kinds:', dict(kinds))


def cmd_stats(args):
    files = containers_under(args.path)
    print('%d containers under %s' % (len(files), args.path))
    kinds = collections.Counter()
    per_ext = collections.Counter()
    rows_opaque = rows_total = 0
    for p in files:
        try:
            arc = Archive(p)
        except Exception as ex:
            kinds['unreadable container'] += 1
            continue
        per_ext[os.path.splitext(p)[1].lower()] += 1
        for e in arc.entries:
            if e.type == 1 and e.size == 8:
                kinds['registration'] += 1
                continue
            try:
                word0, words, data = arc.payload(e)
            except Exception:
                kinds['payload error'] += 1
                continue
            if word0 is None:
                kinds['raw'] += 1
                continue
            kinds[KIND_NAMES.get(word0, 'kind 0x%X' % word0)] += 1
            if word0 in (KIND_SPAN_565, KIND_SPAN_555):
                h = struct.unpack_from('<H', data, 6)[0]
                flags = np.frombuffer(data, dtype='<u4,<u2,<u2', count=h,
                                      offset=16)['f2']
                rows_opaque += int((flags >> 15).sum())
                rows_total += h
    print('by extension:', dict(per_ext))
    for k, v in kinds.most_common():
        print('  %-24s %8d' % (k, v))
    if rows_total:
        print('span rows flagged fully opaque: %d of %d (%.1f%%)'
              % (rows_opaque, rows_total, 100.0 * rows_opaque / rows_total))


def cmd_extract(args):
    files = containers_under(args.path)
    os.makedirs(args.out, exist_ok=True)
    index_path = os.path.join(args.out, 'index.csv')
    written = 0
    skipped_all = collections.Counter()

    with open(index_path, 'w', newline='', encoding='utf-8') as fh:
        wr = csv.writer(fh)
        wr.writerow(['archive', 'group', 'instance', 'kind', 'width', 'height',
                     'reg_left', 'reg_top', 'reg_right', 'reg_bottom',
                     'opaque_rows', 'rows', 'label', 'file'])
        for p in files:
            rel = os.path.relpath(p, args.path if os.path.isdir(args.path)
                                  else os.path.dirname(args.path))
            stem = os.path.splitext(os.path.basename(p))[0]
            try:
                arc = Archive(p)
                sprites, regs, skipped = decode_archive(arc)
            except Exception as ex:
                print('  %-42s SKIPPED (%s)' % (rel, ex))
                skipped_all['container: %s' % type(ex).__name__] += 1
                continue
            skipped_all.update(skipped)
            if not sprites:
                continue
            sub = os.path.join(args.out, os.path.dirname(rel), stem)
            os.makedirs(sub, exist_ok=True)
            manifest = sii_for(p)
            for s in sprites:
                name = '%08X_%08X.png' % (s.group, s.instance)
                Image.fromarray(s.rgba, 'RGBA').save(os.path.join(sub, name))
                reg = regs.get(s.instance)
                meta = manifest.get((s.group, s.instance)) or \
                    manifest.get((None, s.instance))
                if reg is None and meta:
                    reg = meta[0]
                wr.writerow([rel, '%08X' % s.group, '%08X' % s.instance, s.kind,
                             s.width, s.height,
                             *(reg if reg else ('', '', '', '')),
                             s.opaque_rows, s.total_rows,
                             meta[1] if meta else '',
                             os.path.join(os.path.dirname(rel), stem, name)])
                written += 1
            print('  %-42s %5d images' % (rel, len(sprites)))

    print('\n%d images -> %s' % (written, args.out))
    if skipped_all:
        print('not decoded:')
        for k, v in skipped_all.most_common(10):
            print('  %-40s %6d' % (k, v))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('path', help='a container file, or a directory to walk')
    ap.add_argument('--list', action='store_true', help='table of contents')
    ap.add_argument('--stats', action='store_true', help='record kinds in a tree')
    ap.add_argument('--extract', action='store_true', help='decode to PNG')
    ap.add_argument('--out', default='sc3k_extracted', help='output directory')
    ap.add_argument('--limit', type=int, default=40, help='rows for --list')
    args = ap.parse_args()

    if args.list:
        cmd_list(args)
    elif args.stats:
        cmd_stats(args)
    elif args.extract:
        cmd_extract(args)
    else:
        ap.error('choose one of --list, --stats, --extract')


if __name__ == '__main__':
    main()
