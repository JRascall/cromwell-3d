"""WiC texture decoding: recover (format, width, height, mips) for a headerless
payload, decode it, and write a PNG.

WHY THIS IS A SEARCH AND NOT A PARSE. The .sdf packer strips the 128-byte DDS
header and records the format nowhere. The table of contents was searched and
contains no 'DDS ' magic, no dimensions and no format field; neither does the
payload region. Every .dds in the archive decompresses to exactly its declared
size minus 128, so the header is dropped at pack time and the engine rebuilds it
from knowledge held elsewhere in the executable.

That leaves the payload SIZE as the only evidence, and size alone is ambiguous:
DXT1 512x512, A8L8 256x256 and R5G6B5 256x256 are all 174,760 bytes with the
same mip chain. So this enumerates candidates by size and then discriminates by
DECODING them, on two independent signals:

  smoothness      a correct interpretation yields a smooth image; a wrong one
                  reads adjacent texels out of the middle of unrelated blocks
                  and looks like noise.
  mip agreement   the second mip level should be the first one at half size.
                  A wrong (format, width, height) triple almost never produces
                  a consistent pyramid, which makes this the stronger of the
                  two and the reason it is weighted first.

DXT3 and DXT5 share an identical colour block and differ only in how alpha is
stored, so they are indistinguishable on colour and are reported as one class
unless the alpha decode separates them.

Every result carries a confidence margin (how much better the winner scored than
the runner-up). Files below the margin threshold are written to
_lowconfidence.txt rather than silently trusted -- an identification that is a
guess should say so.

    py -3 wic_tex.py <raw-dir> <out-dir>
    py -3 wic_tex.py --identify <file> [<file>...]
"""
import os, re, sys, struct, glob, collections, json
import numpy as np

FORMATS = [                       # name, block bytes (0 = linear), bytes/texel
    ('DXT1', 8, 0.5),
    ('DXT5', 16, 1.0),
    ('DXT3', 16, 1.0),
    ('A8R8G8B8', 0, 4.0),
    ('R8G8B8', 0, 3.0),
    ('A8L8', 0, 2.0),
    ('R5G6B5', 0, 2.0),
    ('A8', 0, 1.0),
]
FMT = {f[0]: f for f in FORMATS}
SIZES = [1 << i for i in range(1, 13)]      # 2 .. 4096
MARGIN_OK = 1.5     # measured: true readings win by 2-200x, false ones by <1.35


# ---- evidence that beats the search -------------------------------------
#
# Particle flipbooks are the search's hard case, and they matter because they
# are the textures worth looking at. The .pe effect files that reference them
# declare PARTICLEWIDTH, PARTICLEHEIGHT and NUMTEXTURES outright, so the frame
# geometry never has to be guessed.
#
# What the declaration does NOT say is how the frames are laid out, and this
# cost a round of wrong output. A stack of N frames each carrying its own small
# mip chain and a single (N/cols x rows) grid image carrying one mip satisfy
# **exactly the same size equation** -- 128*(128*128*4 + 64*64*4) and
# 2048*1024*4 + 1024*512*4 are the same 10,485,760 bytes -- so arithmetic
# cannot separate them and the first implementation here chose the stack.
#
# The decoded pixels settle it: WiC ships **grid atlases**. Read as a stack,
# every frame comes out as a comb of interleaved rows, because a 128-wide view
# of a 2048-wide image walks a sixteenth of a row at a time. Read as a grid,
# the frames are clean smoke. So the declaration gives the frame size and the
# count, and the grid shape is solved from the payload around it.
#
# The general lesson, which is the same one the .mrb reader learned: a size
# model can only ever rank hypotheses you already had, and "it decodes without
# raising" is not evidence. Look at the image.

def pe_hints(data_dir):
    """basename (no ext) -> (width, height, frames) from the .pe corpus.

    Two rules, both learned from wrong output:

    * **`TEXTURE3` is excluded.** It is the detail slot and is `surfaces/dummy.dds`
      in almost every effect in the game, so including it declares that the
      shared dummy is a 128-frame flipbook and poisons every other reading.
    * **Disagreement drops the hint.** A texture referenced by two effects with
      different `PARTICLEWIDTH` is one where at least one declaration is about a
      different slot, and a wrong hint is strictly worse than no hint — it
      bypasses the search *and* claims full confidence. First-writer-wins was
      the original behaviour and it silently mis-sized the 128x128 six-point
      atlas because some effect that also names it declares 64x64 frames.
    """
    seen = collections.defaultdict(set)
    for p in glob.glob(os.path.join(data_dir, '**', '*.pe'), recursive=True):
        try:
            txt = open(p, 'r', errors='replace').read()
        except OSError:
            continue
        def val(key):
            m = re.search(r'^%s\s+(\S+)' % key, txt, re.M)
            return m.group(1) if m else None
        w, h, n = val('PARTICLEWIDTH'), val('PARTICLEHEIGHT'), val('NUMTEXTURES')
        if not (w and h and n):
            continue
        try:
            w, h, n = int(w), int(h), int(n)
        except ValueError:
            continue
        if w < 1 or h < 1 or n < 1:
            continue
        for m in re.finditer(r'^TEXTURE2?\s+(.+)$', txt, re.M):
            name = os.path.splitext(os.path.basename(m.group(1).strip().replace('\\', '/')))[0]
            seen[name.lower()].add((w, h, n))
    return {k: next(iter(v)) for k, v in seen.items() if len(v) == 1}


def identify_atlas(data, w, h, frames):
    """Solve a declared w*h*frames flipbook laid out as one grid image.

    `width`/`height` in the result are the **atlas** dimensions, so decode() and
    to_png() need no special case; `frame_width`, `frame_height` and `cols` say
    how to slice it back into frames.

    Columns are enumerated over the divisors of `frames` that keep both atlas
    axes a power of two, which is what the shipped files use. The size equation
    then picks the format and mip count -- but it does *not* always pick the
    orientation: 128 frames of 128x128 is 8x16 or 16x8 and both are the same
    byte count. Where it is ambiguous, decode the survivors and take the
    smoothest, which is the same discriminator identify() uses and which
    separates them cleanly (a transposed read interleaves rows from unrelated
    frames and is visibly a comb).
    """
    size = len(data)
    if frames < 1 or w < 1 or h < 1:
        return None
    layouts = []
    for cols in range(1, frames + 1):
        if frames % cols:
            continue
        rows = frames // cols
        aw, ah = cols * w, rows * h
        if aw & (aw - 1) or ah & (ah - 1):
            continue                      # non-power-of-two: not what ships
        layouts.append((abs(aw - ah), cols, rows, aw, ah))
    layouts.sort()

    hits = []
    for _, cols, rows, aw, ah in layouts:
        for name, _blk, _bpp in FORMATS:
            for levels in (1, 2, 3, 4):
                if chain_bytes(name, aw, ah, levels) == size:
                    hits.append((name, aw, ah, levels, cols, rows))
    if not hits:
        return None
    if len(hits) > 1:
        hits.sort(key=lambda c: smoothness(decode(data, c[0], c[1], c[2])))
    name, aw, ah, levels, cols, rows = hits[0]
    return dict(format=name, width=aw, height=ah, levels=levels,
                frames=frames, cols=cols, rows=rows,
                frame_width=w, frame_height=h,
                atlas=True, margin=float('inf'), candidates=len(hits),
                confident=True, smooth=None, mip=None)


# ---- size model ---------------------------------------------------------

def level_bytes(name, w, h):
    _n, blk, bpp = FMT[name]
    if blk:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * blk
    return int(w * h * bpp)


def chain_bytes(name, w, h, levels):
    return sum(level_bytes(name, max(1, w >> i), max(1, h >> i)) for i in range(levels))


def candidates(size, max_aspect=8):
    out = []
    for name, _blk, _bpp in FORMATS:
        for w in SIZES:
            for h in SIZES:
                if w > h * max_aspect or h > w * max_aspect:
                    continue
                for lev in range(1, max(w.bit_length(), h.bit_length()) + 1):
                    if chain_bytes(name, w, h, lev) == size:
                        out.append((name, w, h, lev))
    return out


# ---- decoding -----------------------------------------------------------

def _c565(v):
    return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def _expand565(v):
    """uint16 array -> (N,3) uint8 RGB."""
    r = ((v >> 11) & 31).astype(np.uint16) * 255 // 31
    g = ((v >> 5) & 63).astype(np.uint16) * 255 // 63
    b = (v & 31).astype(np.uint16) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.uint8)


def decode(data, name, w, h, offset=0):
    """One mip level -> (h, w, 3) uint8.

    Vectorised: a pure-Python block loop costs ~5.6 s per texture, which is
    15 hours over the 10,029-texture library and is why this is numpy.
    """
    if name in ('DXT1', 'DXT3', 'DXT5'):
        blk = FMT[name][1]
        bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
        need = bw * bh * blk
        buf = data[offset:offset + need]
        if len(buf) < need:
            buf = buf + b'\0' * (need - len(buf))
        a = np.frombuffer(buf, np.uint8).reshape(bh * bw, blk)
        cb = a[:, 8:16] if blk == 16 else a[:, 0:8]
        c0 = cb[:, 0].astype(np.uint16) | (cb[:, 1].astype(np.uint16) << 8)
        c1 = cb[:, 2].astype(np.uint16) | (cb[:, 3].astype(np.uint16) << 8)
        bits = (cb[:, 4].astype(np.uint32) | (cb[:, 5].astype(np.uint32) << 8)
                | (cb[:, 6].astype(np.uint32) << 16) | (cb[:, 7].astype(np.uint32) << 24))
        p0, p1 = _expand565(c0), _expand565(c1)
        f0, f1 = p0.astype(np.uint16), p1.astype(np.uint16)
        third = ((2 * f0 + f1) // 3).astype(np.uint8)
        twoth = ((f0 + 2 * f1) // 3).astype(np.uint8)
        half = ((f0 + f1) // 2).astype(np.uint8)
        zero = np.zeros_like(p0)
        if name == 'DXT1':
            opaque = (c0 > c1)[:, None]
            p2 = np.where(opaque, third, half)
            p3 = np.where(opaque, twoth, zero)
        else:
            p2, p3 = third, twoth
        pal = np.stack([p0, p1, p2, p3], 1)                      # (N,4,3)
        shifts = (2 * np.arange(16)).astype(np.uint32)
        idx = (bits[:, None] >> shifts[None, :]) & 3             # (N,16)
        texels = np.take_along_axis(pal, idx[:, :, None].astype(np.intp), 1)  # (N,16,3)
        img = texels.reshape(bh, bw, 4, 4, 3).transpose(0, 2, 1, 3, 4)
        return img.reshape(bh * 4, bw * 4, 3)[:h, :w]

    n = w * h
    if name == 'A8R8G8B8':
        a = np.frombuffer(data[offset:offset + n * 4], np.uint8).reshape(h, w, 4)
        return a[:, :, 2::-1]
    if name == 'R8G8B8':
        a = np.frombuffer(data[offset:offset + n * 3], np.uint8).reshape(h, w, 3)
        return a[:, :, ::-1]
    if name == 'A8L8':
        a = np.frombuffer(data[offset:offset + n * 2], np.uint8).reshape(h, w, 2)
        return np.repeat(a[:, :, 0:1], 3, 2)
    if name == 'R5G6B5':
        v = np.frombuffer(data[offset:offset + n * 2], '<u2').reshape(h, w)
        return _expand565(v)
    a = np.frombuffer(data[offset:offset + n], np.uint8).reshape(h, w)
    return np.repeat(a[:, :, None], 3, 2)


# ---- discriminators -----------------------------------------------------

def smoothness(px):
    """Mean |difference| between adjacent texels."""
    a = px.astype(np.int16)
    dx = np.abs(a[:, 1:] - a[:, :-1]).mean() if px.shape[1] > 1 else 0.0
    dy = np.abs(a[1:, :] - a[:-1, :]).mean() if px.shape[0] > 1 else 0.0
    return float((dx + dy) / 2)


def mip_disagreement(data, name, w, h, levels):
    """Compare mip 1 against mip 0 point-sampled at half rate.

    This is the strong signal: a wrong (format, width, height) triple almost
    never yields a pyramid whose second level matches the first downscaled.
    """
    if levels < 2 or w < 8 or h < 8:
        return None
    lv0 = decode(data, name, w, h, 0)
    hw, hh = max(1, w // 2), max(1, h // 2)
    lv1 = decode(data, name, hw, hh, level_bytes(name, w, h))
    ref = lv0[::2, ::2][:hh, :hw].astype(np.int16)
    return float(np.abs(ref - lv1[:ref.shape[0], :ref.shape[1]].astype(np.int16)).mean())


def identify(data):
    cands = candidates(len(data))
    scored = []
    for name, w, h, lev in cands:
        if level_bytes(name, w, h) > len(data):
            continue
        try:
            px = decode(data, name, w, h)
            s = smoothness(px)
            m = mip_disagreement(data, name, w, h, lev)
        except Exception:
            continue
        # mip agreement dominates when available; smoothness breaks ties.
        key = (m if m is not None else s * 2) * 4 + s
        scored.append((key, name, w, h, lev, s, m))
    if not scored:
        return None
    scored.sort(key=lambda t: t[0])
    best = scored[0]

    # DXT3 and DXT5 decode to identical colour, so they always tie exactly.
    # Counting that as ambiguity would mark every alpha texture unconfident and
    # hide the cases where the geometry is genuinely undecided, which is the
    # thing the margin exists to flag. Measure against the best interpretation
    # that actually differs in shape or in colour handling.
    def same_reading(a, b):
        return (a[2], a[3]) == (b[2], b[3]) and {a[1], b[1]} <= {'DXT3', 'DXT5'}

    rival = next((s for s in scored[1:] if not same_reading(best, s)), None)
    margin = (rival[0] / best[0]) if rival and best[0] > 0 else float('inf')
    return dict(format=best[1], width=best[2], height=best[3], levels=best[4],
                smooth=round(best[5], 2),
                mip=None if best[6] is None else round(best[6], 2),
                margin=round(margin, 3), candidates=len(scored),
                confident=margin >= MARGIN_OK)


def to_png(data, info, path):
    """Top mip. For a flipbook that is the whole grid, frames and all, which is
    what makes the output inspectable — a single frame tells you nothing about
    whether the layout was read correctly, and the grid tells you immediately."""
    from PIL import Image
    px = decode(data, info['format'], info['width'], info['height'])
    Image.fromarray(px, 'RGB').save(path)


# ---- CLI ----------------------------------------------------------------

def _one(job):
    """Worker: (raw_dir, out_dir, path, hint) -> (rel, info|None, error|None)."""
    raw_dir, out_dir, p, hint = job
    rel = os.path.relpath(p, raw_dir)
    out = os.path.join(out_dir, rel + '.png')
    if os.path.exists(out):
        return (rel, None, 'skip')
    try:
        data = open(p, 'rb').read()
        info = identify_atlas(data, *hint) if hint else None
        if info is None:
            info = identify(data)
        if not info:
            return (rel, None, 'unsolved size=%d' % len(data))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        to_png(data, info, out)
        return (rel, info, None)
    except Exception as e:
        return (rel, None, '%s: %s' % (type(e).__name__, e))


def main_parallel(raw_dir, out_dir, jobs=0):
    """Identification is ~1 s of pure CPU per texture and the library is 10k
    files, so this is embarrassingly parallel and single-threaded is 2.6 hours."""
    import multiprocessing as mp
    files = glob.glob(os.path.join(raw_dir, '**', '*.dds'), recursive=True)
    if not files:
        sys.exit('no .dds under %s' % raw_dir)
    os.makedirs(out_dir, exist_ok=True)
    hints = pe_hints(raw_dir)
    print('%d textures, %d geometries declared by the .pe corpus' % (len(files), len(hints)))
    jobs = jobs or max(1, (os.cpu_count() or 4) - 2)
    work = [(raw_dir, out_dir,
             p, hints.get(os.path.splitext(os.path.basename(p))[0].lower()))
            for p in files]
    fmts, res, low, manifest = collections.Counter(), collections.Counter(), [], {}
    done = skipped = failed = 0
    with mp.Pool(jobs) as pool:
        for i, (rel, info, err) in enumerate(pool.imap_unordered(_one, work, chunksize=8)):
            if err == 'skip':
                skipped += 1
            elif err:
                failed += 1
                low.append('%-9s %s' % (err.split(':')[0], rel))
            else:
                done += 1
                fmts[('atlas ' if info.get('atlas') else '') + info['format']] += 1
                res['%dx%d' % (info['width'], info['height'])] += 1
                manifest[rel.replace(os.sep, '/')] = info
                if not info['confident']:
                    low.append('MARGIN %5.2f %-8s %4dx%-4d %s'
                               % (info['margin'], info['format'],
                                  info['width'], info['height'], rel))
            if (i + 1) % 500 == 0:
                print('  %d/%d  (%d written, %d skipped, %d failed)'
                      % (i + 1, len(work), done, skipped, failed))
    with open(os.path.join(out_dir, '_manifest.json'), 'w') as fh:
        json.dump(manifest, fh, indent=1, sort_keys=True, default=str)
    with open(os.path.join(out_dir, '_lowconfidence.txt'), 'w') as fh:
        fh.write('\n'.join(low))
    print('wrote %d, skipped %d, failed %d, %d flagged' % (done, skipped, failed, len(low)))
    print('formats:', fmts.most_common())
    print('resolutions:', res.most_common(12))


def main(raw_dir, out_dir):
    files = [p for p in glob.glob(os.path.join(raw_dir, '**', '*.dds'), recursive=True)]
    if not files:
        sys.exit('no .dds under %s' % raw_dir)
    os.makedirs(out_dir, exist_ok=True)
    hints = pe_hints(raw_dir)
    print('%d texture geometries declared by the .pe corpus' % len(hints))
    fmts, res = collections.Counter(), collections.Counter()
    low, manifest, done, failed = [], {}, 0, 0
    for p in files:
        rel = os.path.relpath(p, raw_dir)
        out = os.path.join(out_dir, rel + '.png')
        if os.path.exists(out):
            done += 1
            continue
        data = open(p, 'rb').read()
        hint = hints.get(os.path.splitext(os.path.basename(p))[0].lower())
        info = identify_atlas(data, *hint) if hint else None
        if info is None:
            info = identify(data)
        if not info:
            failed += 1
            low.append('UNSOLVED %9d %s' % (len(data), rel))
            continue
        fmts[info['format']] += 1
        res['%dx%d' % (info['width'], info['height'])] += 1
        manifest[rel.replace(os.sep, '/')] = info
        if not info['confident']:
            low.append('MARGIN %5.2f %-8s %4dx%-4d %s'
                       % (info['margin'], info['format'], info['width'], info['height'], rel))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        try:
            to_png(data, info, out)
            done += 1
        except Exception as e:
            failed += 1
            low.append('PNGFAIL %s: %s' % (rel, e))
    with open(os.path.join(out_dir, '_manifest.json'), 'w') as fh:
        json.dump(manifest, fh, indent=1, sort_keys=True)
    with open(os.path.join(out_dir, '_lowconfidence.txt'), 'w') as fh:
        fh.write('\n'.join(low))
    print('wrote %d PNGs, %d failed, %d low-confidence' % (done, failed, len(low)))
    print('formats:', fmts.most_common())
    print('resolutions:', res.most_common(12))


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == '--identify':
        for p in sys.argv[2:]:
            d = open(p, 'rb').read()
            print('%-60s %s' % (os.path.basename(p), identify(d)))
    elif len(sys.argv) in (3, 4):
        main_parallel(sys.argv[1], sys.argv[2],
                      int(sys.argv[3]) if len(sys.argv) == 4 else 0)
    else:
        sys.exit(__doc__)
