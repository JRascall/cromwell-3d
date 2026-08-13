# -*- coding: utf-8 -*-
"""EA RefPack (also called QFS) — the compressor behind every payload in
SimCity 3000's archives.

Written for ../../study/games/strategy/simcity3000.md.  Python 3.

RefPack is EA's in-house LZ77 variant, used across their catalogue from the
mid-90s on: SimCity 3000 and 4, The Sims, NBA Live, the FIFA series.  The
stream is a sequence of *commands*, each of which does two things in one go —
copy N literal bytes from the stream, then copy M bytes from earlier in the
output.  That fusion is the whole trick: a literal run and the match that
follows it share one control byte, so short alternations (which is what image
data is) do not pay a tag byte each.

Four command forms, distinguished by the top bits of the first byte:

    0xxxxxxx  2 bytes   0-3 literals, 3-10 match,   offset <= 1023
    10xxxxxx  3 bytes   0-3 literals, 4-67 match,   offset <= 16383
    110xxxxx  4 bytes   0-3 literals, 5-1028 match, offset <= 131071
    111xxxxx  1 byte    4-112 literals, no match          (0xE0..0xFB)
    111111xx  1 byte    0-3 literals, no match, END       (0xFC..0xFF)

The offset is stored as *distance back from the current output position*, minus
one, so the encoder can emit distance 1 — which is how runs of a repeated byte
are coded, and why the match copy has to be able to read bytes it is itself
writing.  A `memcpy` is therefore wrong here; see `_copy_match`.

The header is 5 bytes and is NOT part of the command stream:

    0x00  uint16  0xFB10, little-endian on disk, so the bytes read `10 FB`
    0x02  uint24  uncompressed length, BIG-endian

Big-endian in a little-endian file looks like a mistake and is not — RefPack
predates the x86-only era of EA's tools and the format was never re-endianed.
Reading it as little-endian yields a plausible-looking length for small assets
and garbage for large ones, which is the worst kind of wrong.

The decoded length is checked against the header on every call.  That check is
worth more than it looks: a desynchronised command stream does not throw, it
produces the wrong number of bytes, and without the check a corrupt sprite
reaches the decoder as a shape mismatch three functions away from the cause.
"""

MAGIC = 0xFB10


def is_packed(buf, off=0):
    """True if a RefPack header starts at `off`.

    Used to find the payload inside an archive record: the record's fixed
    header varies in length by record kind, so the reader locates the stream by
    its signature rather than by trusting a per-kind offset table.
    """
    return len(buf) >= off + 2 and buf[off] == 0x10 and buf[off + 1] == 0xFB


def unpacked_size(buf, off=0):
    """The declared output length, without decompressing anything."""
    if not is_packed(buf, off):
        raise ValueError('not a RefPack stream at offset %d' % off)
    return (buf[off + 2] << 16) | (buf[off + 3] << 8) | buf[off + 4]


def decompress(buf, off=0):
    """Decode the RefPack stream starting at `off`.  Returns bytes."""
    if not is_packed(buf, off):
        raise ValueError('not a RefPack stream at offset %d: %s'
                         % (off, buf[off:off + 4].hex(' ')))
    want = unpacked_size(buf, off)
    p = off + 5
    out = bytearray()
    n = len(buf)

    while p < n:
        cc = buf[p]
        p += 1

        if cc < 0x80:                       # 0xxxxxxx
            b1 = buf[p]; p += 1
            lit = cc & 3
            length = ((cc >> 2) & 7) + 3
            dist = ((cc & 0x60) << 3) + b1 + 1
        elif cc < 0xC0:                     # 10xxxxxx
            b1 = buf[p]; b2 = buf[p + 1]; p += 2
            lit = (b1 >> 6) & 3
            length = (cc & 0x3F) + 4
            dist = ((b1 & 0x3F) << 8) + b2 + 1
        elif cc < 0xE0:                     # 110xxxxx
            b1 = buf[p]; b2 = buf[p + 1]; b3 = buf[p + 2]; p += 3
            lit = cc & 3
            length = ((cc & 0x0C) << 6) + b3 + 5
            dist = ((cc & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif cc < 0xFC:                     # 111xxxxx, literal run
            lit = ((cc & 0x1F) << 2) + 4
            out += buf[p:p + lit]
            p += lit
            continue
        else:                               # 111111xx, terminator
            lit = cc & 3
            out += buf[p:p + lit]
            p += lit
            break

        out += buf[p:p + lit]
        p += lit
        _copy_match(out, dist, length)

    if len(out) != want:
        raise ValueError('RefPack length mismatch: decoded %d, header says %d'
                         % (len(out), want))
    return bytes(out)


def _copy_match(out, dist, length):
    """Copy `length` bytes from `dist` back in `out`, appending as it goes.

    The two cases are not an optimisation of each other.  When the match does
    not reach past the current end (`dist >= length`) the source is fully
    written already and one slice does it.  When it does overlap — `dist == 1`
    being the common case, an encoded run of one repeated byte — the copy must
    read bytes that this same call is producing, which a slice cannot express.
    Doing the whole thing bytewise would be correct but is roughly 4x slower
    over a full archive sweep, and the overlapping case is the minority.
    """
    start = len(out) - dist
    if start < 0:
        raise ValueError('RefPack match points before the start of the output')
    if dist >= length:
        out += out[start:start + length]
    else:
        for i in range(start, start + length):
            out.append(out[i])
