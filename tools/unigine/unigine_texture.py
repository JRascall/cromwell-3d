"""
unigine_texture.py - reader for UNIGINE 2's `.texture` container ("tx10").

UNIGINE ships its core art as `.texture`, a thin container around raw GPU
texture data. It is not DDS and no third-party tool reads it, but the layout is
simple and fully recoverable:

    magic   'tx10'                                        4 bytes
    u32     is_3d           1 for a volume texture, else 0
    u32     format          Image::FORMAT_* enum (see FORMATS below)
    u32     width
    u32     height
    u32     depth           1 for 2D
    u32     num_mips
    u32 x2  unknown, always 1 1
    i32     unknown, always -1
    u32     unknown, always 0
    u32     unknown, always 4                            -> 48-byte header
    then, per mip, low to high:
    u64     stored_size
    bytes   payload         raw if stored_size == raw size, else an LZ4 block

The LZ4 is raw block format with no frame header, exactly as in the Helldivers 2
containers - the uncompressed size is known from the format and dimensions, so
it decodes directly. Compression is opportunistic and per mip: `foam_d.texture`
stores raw BC4 byte for byte, `clouds_shadows_*.texture` compresses 131072 bytes
of mostly-empty BC4 down to 17624.

Volume textures are a stack of independently block-compressed 2D slices, not 3D
blocks, so a BC4 256^3 volume is 256 slices of 256x256/2 bytes.

This is a *study* tool. Decoded output goes to unigine_extracted/, which is
gitignored - UNIGINE's art is licensed for use in UNIGINE projects and is not
ours to redistribute. Read the channels, then generate your own equivalents.

Format notes and what the cloud textures actually contain live in
study/games/rendering/unigine_clouds.md.

    py -3 tools\\unigine_texture.py <file-or-dir> [-o outdir] [--info] [--mip N]
"""

import argparse
import os
import struct
import sys

import numpy as np
from PIL import Image

try:
    import lz4.block as _lz4
except ImportError:
    sys.exit("this needs the lz4 package:  py -3 -m pip install lz4")

MAGIC = b"tx10"
HEADER = 48

# UNIGINE's Image::FORMAT_* enum. Values 3, 24 and 25 are confirmed byte-exact
# against the SDK's own files; the rest follow the enum's declaration order and
# are checked at decode time - a wrong guess fails the size assertion rather
# than silently producing garbage.
#   name, bytes-per-pixel (0 => block compressed), channels, block bytes
FORMATS = {
    0:  ("R8",      1, 1, 0),
    1:  ("RG8",     2, 2, 0),
    2:  ("RGB8",    3, 3, 0),
    3:  ("RGBA8",   4, 4, 0),
    4:  ("R16",     2, 1, 0),
    5:  ("RG16",    4, 2, 0),
    6:  ("RGB16",   6, 3, 0),
    7:  ("RGBA16",  8, 4, 0),
    8:  ("R16F",    2, 1, 0),
    9:  ("RG16F",   4, 2, 0),
    10: ("RGB16F",  6, 3, 0),
    11: ("RGBA16F", 8, 4, 0),
    12: ("R32F",    4, 1, 0),
    13: ("RG32F",   8, 2, 0),
    14: ("RGB32F", 12, 3, 0),
    15: ("RGBA32F",16, 4, 0),
    21: ("DXT1",    0, 4, 8),
    22: ("DXT3",    0, 4, 16),
    23: ("DXT5",    0, 4, 16),
    24: ("ATI1",    0, 1, 8),    # BC4, single channel
    25: ("ATI2",    0, 2, 16),   # BC5, two channels
}


class Header(object):
    def __init__(self, path):
        with open(path, "rb") as f:
            buf = f.read(HEADER)
        if len(buf) < HEADER or buf[:4] != MAGIC:
            raise ValueError("%s: not a tx10 texture" % os.path.basename(path))
        (self.is_3d, self.format, self.width, self.height, self.depth,
         self.mips) = struct.unpack_from("<6I", buf, 4)
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0]
        if self.format not in FORMATS:
            raise ValueError("%s: unknown format %d" % (self.name, self.format))
        self.fmt, self.bpp, self.channels, self.block = FORMATS[self.format]

    def mip_size(self, level):
        """Bytes one mip occupies uncompressed. Slices are compressed 2D."""
        w = max(1, self.width >> level)
        h = max(1, self.height >> level)
        d = max(1, self.depth >> level) if self.is_3d else 1
        if self.block:
            return ((w + 3) // 4) * ((h + 3) // 4) * self.block * d
        return w * h * self.bpp * d

    def dims(self, level):
        return (max(1, self.width >> level),
                max(1, self.height >> level),
                max(1, self.depth >> level) if self.is_3d else 1)

    def __str__(self):
        kind = "3D" if self.is_3d else "2D"
        size = "%dx%d" % (self.width, self.height)
        if self.is_3d:
            size += "x%d" % self.depth
        return "%-32s %-3s %-14s %-8s %2d mip" % (
            self.name, kind, size, self.fmt, self.mips)


def read_mip(hdr, level):
    """Return one mip's uncompressed bytes, decompressing if it was packed."""
    want = hdr.mip_size(level)
    with open(hdr.path, "rb") as f:
        f.seek(HEADER)
        for i in range(hdr.mips):
            stored = struct.unpack("<Q", f.read(8))[0]
            if i != level:
                f.seek(stored, os.SEEK_CUR)
                continue
            data = f.read(stored)
            if len(data) != stored:
                raise ValueError("%s: truncated at mip %d" % (hdr.name, i))
            if stored == hdr.mip_size(i):
                return data
            out = _lz4.decompress(data, uncompressed_size=hdr.mip_size(i))
            if len(out) != want:
                raise ValueError("%s mip %d: got %d bytes, expected %d - the "
                                 "format guess for enum %d is probably wrong"
                                 % (hdr.name, i, len(out), want, hdr.format))
            return out
    raise ValueError("%s: no mip %d" % (hdr.name, level))


# ---- block decoders ------------------------------------------------------
# All vectorised: a 256^3 BC4 volume is a million blocks and a per-block Python
# loop takes half a minute, where these take under a second.

def _bits48(blocks, first):
    """Six bytes starting at `first` as one uint64 per block."""
    v = np.zeros(len(blocks), dtype=np.uint64)
    for i in range(6):
        v |= blocks[:, first + i].astype(np.uint64) << np.uint64(8 * i)
    return v


def _bc4_channel(blocks, first):
    """Decode the 8-byte BC4 payload at `first` to (N, 16) uint8."""
    e0 = blocks[:, first].astype(np.uint16)
    e1 = blocks[:, first + 1].astype(np.uint16)
    pal = np.zeros((len(blocks), 8), dtype=np.uint16)
    pal[:, 0], pal[:, 1] = e0, e1
    six = e0 <= e1
    for i in range(1, 7):          # 8-value mode: 6 interpolants
        pal[:, i + 1] = ((7 - i) * e0 + i * e1) // 7
    for i in range(1, 5):          # 6-value mode: 4 interpolants, then 0 and 255
        pal[six, i + 1] = ((5 - i) * e0[six] + i * e1[six]) // 5
    pal[six, 6], pal[six, 7] = 0, 255

    bits = _bits48(blocks, first + 2)
    idx = np.empty((len(blocks), 16), dtype=np.uint8)
    for t in range(16):
        idx[:, t] = ((bits >> np.uint64(3 * t)) & np.uint64(7)).astype(np.uint8)
    return np.take_along_axis(pal, idx, axis=1).astype(np.uint8)


def _bc1_colour(blocks, first, opaque):
    """Decode the 8-byte BC1 colour payload at `first` to (N, 16, 4) uint8."""
    c0 = blocks[:, first].astype(np.uint16) | (blocks[:, first + 1].astype(np.uint16) << 8)
    c1 = blocks[:, first + 2].astype(np.uint16) | (blocks[:, first + 3].astype(np.uint16) << 8)

    def unpack(c):
        r = ((c >> 11) & 0x1F).astype(np.uint16)
        g = ((c >> 5) & 0x3F).astype(np.uint16)
        b = (c & 0x1F).astype(np.uint16)
        return np.stack([(r * 527 + 23) >> 6,        # 5/6-bit -> 8-bit, rounded
                         (g * 259 + 33) >> 6,
                         (b * 527 + 23) >> 6], axis=1)

    pal = np.zeros((len(blocks), 4, 4), dtype=np.uint16)
    pal[:, 0, :3], pal[:, 1, :3] = unpack(c0), unpack(c1)
    pal[:, :2, 3] = 255
    four = (c0 > c1) | opaque      # DXT3/5 are always in 4-colour mode
    pal[four, 2, :3] = (2 * pal[four, 0, :3] + pal[four, 1, :3]) // 3
    pal[four, 3, :3] = (pal[four, 0, :3] + 2 * pal[four, 1, :3]) // 3
    pal[four, 2:, 3] = 255
    three = ~four
    pal[three, 2, :3] = (pal[three, 0, :3] + pal[three, 1, :3]) // 2
    pal[three, 2, 3] = 255
    pal[three, 3, :] = 0           # the punch-through transparent index

    bits = np.zeros(len(blocks), dtype=np.uint32)
    for i in range(4):
        bits |= blocks[:, first + 4 + i].astype(np.uint32) << np.uint32(8 * i)
    idx = np.empty((len(blocks), 16), dtype=np.uint8)
    for t in range(16):
        idx[:, t] = ((bits >> np.uint32(2 * t)) & np.uint32(3)).astype(np.uint8)
    return np.take_along_axis(pal, idx[:, :, None].astype(np.intp),
                              axis=1).astype(np.uint8)


def _blocks_to_image(px, w, h, channels):
    """(N, 16, C) block-order pixels -> (h, w, C), undoing the 4x4 tiling."""
    bw, bh = (w + 3) // 4, (h + 3) // 4
    img = px.reshape(bh, bw, 4, 4, channels)
    img = img.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, channels)
    return img[:h, :w]


def decode_slice(hdr, raw, w, h):
    """One 2D slice of uncompressed-container bytes -> (h, w, C) uint8."""
    if not hdr.block:
        if hdr.bpp // hdr.channels != 1:
            raise ValueError("%s: %s is not 8-bit, no converter yet"
                             % (hdr.name, hdr.fmt))
        return np.frombuffer(raw, dtype=np.uint8).reshape(h, w, hdr.channels)

    blocks = np.frombuffer(raw, dtype=np.uint8).reshape(-1, hdr.block)
    if hdr.fmt == "ATI1":
        px = _bc4_channel(blocks, 0)[:, :, None]
    elif hdr.fmt == "ATI2":
        px = np.stack([_bc4_channel(blocks, 0), _bc4_channel(blocks, 8)], axis=2)
    elif hdr.fmt == "DXT1":
        px = _bc1_colour(blocks, 0, opaque=False)
    elif hdr.fmt == "DXT5":
        px = _bc1_colour(blocks, 8, opaque=True)
        px[:, :, 3] = _bc4_channel(blocks, 0)
    elif hdr.fmt == "DXT3":
        px = _bc1_colour(blocks, 8, opaque=True)
        a = blocks[:, :8]
        alpha = np.empty((len(blocks), 16), dtype=np.uint8)
        for t in range(16):
            nib = (a[:, t // 2] >> (4 * (t % 2))) & 0xF
            alpha[:, t] = nib * 17
        px[:, :, 3] = alpha
    else:
        raise ValueError("%s: no decoder for %s" % (hdr.name, hdr.fmt))
    return _blocks_to_image(px, w, h, px.shape[2])


def decode(hdr, level=0):
    """Whole mip -> (depth, h, w, C) uint8."""
    raw = read_mip(hdr, level)
    w, h, d = hdr.dims(level)
    per = len(raw) // d
    return np.stack([decode_slice(hdr, raw[i * per:(i + 1) * per], w, h)
                     for i in range(d)])


# ---- output --------------------------------------------------------------

MODES = {1: "L", 2: "RGB", 3: "RGB", 4: "RGBA"}


def to_image(a):
    """(h, w, C) -> PIL. Two-channel BC5 becomes RG_ so it is viewable."""
    if a.shape[2] == 2:
        a = np.dstack([a, np.zeros(a.shape[:2], dtype=np.uint8)])
    mode = MODES[a.shape[2]]
    if a.shape[2] == 1:            # PIL wants 2D for L, not (h, w, 1)
        a = a[:, :, 0]
    return Image.fromarray(np.ascontiguousarray(a), mode)


def contact_sheet(vol):
    """(d, h, w, C) -> one image, slices tiled left to right, top to bottom."""
    d, h, w, c = vol.shape
    cols = int(np.ceil(np.sqrt(d)))
    rows = int(np.ceil(d / cols))
    sheet = np.zeros((rows * h, cols * w, c), dtype=np.uint8)
    for i in range(d):
        r, q = divmod(i, cols)
        sheet[r * h:(r + 1) * h, q * w:(q + 1) * w] = vol[i]
    return sheet


def write(hdr, outdir, level=0, split=True, slices=False):
    vol = decode(hdr, level)
    os.makedirs(outdir, exist_ok=True)
    written = []

    def save(img, suffix):
        p = os.path.join(outdir, "%s%s.png" % (hdr.name, suffix))
        to_image(img).save(p)
        written.append(p)

    if not hdr.is_3d:
        save(vol[0], "")
        if split and vol.shape[3] > 1:
            for i in range(vol.shape[3]):
                save(vol[0, :, :, i:i + 1], "_%s" % "rgba"[i])
    else:
        save(contact_sheet(vol), "_sheet")
        if split and vol.shape[3] > 1:
            for i in range(vol.shape[3]):
                save(contact_sheet(vol[:, :, :, i:i + 1]), "_sheet_%s" % "rgba"[i])
        if slices:
            sub = os.path.join(outdir, hdr.name)
            os.makedirs(sub, exist_ok=True)
            for z in range(vol.shape[0]):
                p = os.path.join(sub, "z%03d.png" % z)
                to_image(vol[z]).save(p)
                written.append(p)
    return vol, written


def stats(vol):
    """Per-channel min/mean/max - the fastest way to see what a channel holds."""
    out = []
    for i in range(vol.shape[3]):
        c = vol[:, :, :, i]
        out.append("%s %3d/%3d/%3d" % ("rgba"[i], c.min(), c.mean(), c.max()))
    return "  ".join(out)


def gather(target):
    if os.path.isfile(target):
        return [target]
    found = []
    for root, _, files in os.walk(target):
        found += [os.path.join(root, f) for f in sorted(files)
                  if f.endswith(".texture")]
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", help="a .texture file, or a directory to walk")
    ap.add_argument("-o", "--out", default="unigine_extracted",
                    help="output directory (default: unigine_extracted)")
    ap.add_argument("--info", action="store_true",
                    help="print the header table only, decode nothing")
    ap.add_argument("--mip", type=int, default=0, help="mip level (default 0)")
    ap.add_argument("--slices", action="store_true",
                    help="also write every z slice of a volume as its own PNG")
    ap.add_argument("--no-split", dest="split", action="store_false",
                    help="skip the per-channel images")
    args = ap.parse_args()

    files = gather(args.target)
    if not files:
        sys.exit("no .texture files under %s" % args.target)

    ok = fail = 0
    for path in files:
        try:
            hdr = Header(path)
        except ValueError as e:
            print("  !! %s" % e)
            fail += 1
            continue
        if args.info:
            print(hdr)
            ok += 1
            continue
        try:
            vol, written = write(hdr, args.out, args.mip, args.split, args.slices)
            print("%s  %s  -> %d file%s" % (hdr, stats(vol), len(written),
                                            "" if len(written) == 1 else "s"))
            ok += 1
        except Exception as e:
            print("  !! %s: %s" % (hdr.name, e))
            fail += 1

    print("\n%d ok, %d failed" % (ok, fail))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
