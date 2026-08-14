"""mercs_tex.py - decode Mercenaries' PS2 textures to .png.

Textures do NOT use the LZ in mercs_lzss.py. The texture loader at 0x0036BBD8
in SLUS_209.32 never calls it; its BODY handler at 0x0036C0B0 has its own
byte-oriented RLE, which is why every attempt to unpack textures with the mesh
decompressor failed instantly on a back-reference.

THE RLE, transcribed from 0x0036C114..0x0036C1D4:

    b = next byte
    if b & 0x80:   run     - read one more byte, repeat it (b - 127) times
    else:          literal - copy the next (b + 1) bytes

Both counts land in 1..128, so the worst case is one control byte per 128
literals: 0.78% expansion. That is the tell that identified it - the 4bpp
textures, whose palette indices are essentially incompressible, come out ~1%
LARGER than raw, which no LZ would do but a literal-run RLE does exactly.

MIP SIZES. The loader computes each level as

    (width * height * bpp) >> (2 * mip + 3)

which is width*height*bpp/8 for mip 0 and quarters thereafter. The tex_ INFO
chunk is {u16 width; u16 height; u16 bpp; u16 mipCount}, read as u16s by the
INFO handler at 0x0036BCA8.

PALETTES. pal_ holds its own INFO {u16 entries; u16 bitsPerEntry} and a BODY of
raw ABGR quads behind a short count prefix - 16-entry tables are 1+64 bytes,
256-entry ones 2+1024. PS2 stores a 256-entry CLUT with bits 3 and 4 of the
index swapped (the CSM1 layout), so 8bpp palettes are unswizzled on load; 4bpp
16-entry tables are not affected.

CHANNEL ORDER, and how it was settled. Byte 0 is alpha: over all 4,643 palettes
in the archive it is the byte with the fewest distinct values in 4,550 of them,
constant 0xFF on an opaque texture and bimodal 0x00/0xFF on a cutout, which no
colour channel is. The remaining three are B,G,R - byte 3 is red. Known-colour
textures say so with no room to argue: the Chinese HQ flag's palette is
`ff080c7d`-ish throughout, and a Chinese flag is red, not navy. Wood, skin and
autumn grass all agree. Read as A,R,G,B - which this file did at first - every
one of them comes out blue.

ALPHA. Full 0..255, despite PS2's 0..128 convention elsewhere: 0xFF accounts
for 59.9% of all palette entries and 0x00 for 37.3%, with 15,252 entries
between 129 and 254. Scaling by 2 (the right thing for a 0..128 alpha) drove
every one of those to opaque.

Read-only research - see the header of mercs_dsk.py.
"""
import argparse
import os
import struct
import sys
import zlib


def rle_decode(src, expected):
    """The BODY codec at 0x0036C114. Stops once `expected` bytes exist."""
    out = bytearray()
    p, n = 0, len(src)
    while len(out) < expected and p < n:
        b = src[p]
        p += 1
        if b & 0x80:
            if p >= n:
                break
            out.extend(bytes([src[p]]) * (b - 127))
            p += 1
        else:
            count = b + 1
            out.extend(src[p:p + count])
            p += count
    return bytes(out[:expected]), p


def read_palette(pal_payload):
    """pal_ BODY is RLE'd over ENTRIES, not bytes - see 0x0036BA30.

    Same control byte as the pixel RLE, but the unit is one palette entry:
    the run branch reads a single entry and repeats it, the literal branch
    reads n entries. The sizes prove it with no ambiguity - a 16-entry table
    is 65 bytes, which is ONE control byte (0x0F, sixteen literal entries)
    plus 16*4, and a 256-entry table is 1026, which is TWO control bytes
    (0x7F, 128 entries each) plus 256*4.

    Treating it as a flat prefix plus raw quads happens to be right for 16
    entries, because there the single control byte looks exactly like a 1-byte
    prefix. It is wrong for 256, where the second control byte sits in the
    MIDDLE of the data and shifts everything after it - which is why 8bpp
    textures came out speckled while 4bpp ones were perfect.

    Fixing that speckling is NOT the same fix as the channel order below, and
    the two were confused afterwards. "Confirmed by eye" was the weak link: the
    textures looked at were asphalt, concrete and metal, which are grey, and
    grey survives a red/blue swap intact. The characters did not - their skin
    read as corpse-blue - and that is the whole tell. When checking a channel
    order, pick a texture whose colour is known and not neutral.
    """
    d = dict(children(pal_payload))
    if 'INFO' not in d or 'BODY' not in d:
        return None
    count, bits = struct.unpack_from('<HH', d['INFO'], 0)
    body = d['BODY']
    width = max(1, bits // 8)
    if width != 4:
        return None

    entries = []
    p, n = 0, len(body)
    while len(entries) < count and p < n:
        b = body[p]
        p += 1
        if b & 0x80:
            if p + width > n:
                break
            entries.extend([body[p:p + width]] * (b - 127))
            p += width
        else:
            for _ in range(b + 1):
                if p + width > n:
                    break
                entries.append(body[p:p + width])
                p += width
    if len(entries) < count:
        return None

    cols = []
    for e in entries[:count]:
        a, bl, g, r = e
        # Stored A,B,G,R - a little-endian 0xRRGGBBAA word - with alpha over
        # the FULL 0..255 range, not PS2's 0..128. Both halves of that were
        # wrong for a while and neither is visible on a grey texture, which is
        # why it survived: see the note in read_palette's docstring.
        cols.append((r, g, bl, a))
    return cols


def children(buf):
    out, p = [], 0
    while p + 8 <= len(buf):
        tag = buf[p:p + 4]
        size = struct.unpack_from('<I', buf, p + 4)[0]
        if p + 8 + size > len(buf):
            break
        out.append((tag.decode('ascii', 'replace'), buf[p + 8:p + 8 + size]))
        p = (p + 8 + size + 3) & ~3
    return out


def write_png(path, w, h, rgba):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw.extend(rgba[y * stride:(y + 1) * stride])

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data
                + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(bytes(raw), 6))
           + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)


def decode_texture(payload, want_mip=0):
    kids = children(payload)
    d = dict(kids)
    if 'INFO' not in d:
        return None
    w, h, bpp, mips = struct.unpack_from('<HHHH', d['INFO'], 0)
    name = d.get('NAME', b'').split(b'\0')[0].decode('ascii', 'replace')
    bodies = [v for k, v in kids if k == 'BODY']
    pals = [v for k, v in kids if k == 'pal_']
    if not bodies or not pals:
        return None
    pal = read_palette(pals[0])
    if not pal:
        return None

    mip = min(want_mip, len(bodies) - 1)
    mw, mh = max(1, w >> mip), max(1, h >> mip)
    expected = mw * mh * bpp // 8
    idx, _ = rle_decode(bodies[mip], expected)
    if len(idx) < expected:
        return None

    rgba = bytearray(mw * mh * 4)
    if bpp == 8:
        for i in range(mw * mh):
            c = pal[idx[i]] if idx[i] < len(pal) else (255, 0, 255, 255)
            rgba[i * 4:i * 4 + 4] = bytes(c)
    elif bpp == 4:
        for i in range(mw * mh):
            byte = idx[i >> 1]
            v = (byte & 0x0F) if (i & 1) == 0 else (byte >> 4)
            c = pal[v] if v < len(pal) else (255, 0, 255, 255)
            rgba[i * 4:i * 4 + 4] = bytes(c)
    else:
        return None
    return name, mw, mh, bpp, rgba


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dsk', nargs='+')
    ap.add_argument('--out', required=True)
    ap.add_argument('--mip', type=int, default=0)
    ap.add_argument('--limit', type=int, default=0)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    done = failed = 0
    seen = set()
    for path in args.dsk:
        data = open(path, 'rb').read()
        count = struct.unpack_from('<I', data, 0)[0]
        ents = [struct.unpack_from('<III', data, 8 + i * 12) for i in range(count)]
        off = 8 + count * 12
        for size, h, grp in ents:
            blob = data[off:off + size]
            off += size
            if blob[8:12] != b'tex_' or h in seen:
                continue
            seen.add(h)
            if args.limit and done >= args.limit:
                break
            payload = blob[16:16 + struct.unpack_from('<I', blob, 12)[0]]
            try:
                r = decode_texture(payload, args.mip)
            except Exception:
                r = None
            if not r:
                failed += 1
                continue
            name, mw, mh, bpp, rgba = r
            safe = ''.join(c if c.isalnum() or c in '_-' else '_' for c in name)
            write_png(os.path.join(args.out, '%s_%08X.png' % (safe[:60] or 'tex', h)),
                      mw, mh, rgba)
            done += 1
    print('  %d textures -> %s' % (done, args.out))
    if failed:
        print('  %d could not be decoded' % failed)
    return 0


if __name__ == '__main__':
    sys.exit(main())
