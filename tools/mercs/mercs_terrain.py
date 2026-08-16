"""mercs_terrain.py - decode Mercenaries' tern chunks to heightmaps and meshes.

Each level is one tern chunk in ASSETS.DSK. 14 of them, and they are large:
a 512 x 512 height grid at 8 metre spacing is a 4.1 km square map.

THE tern LAYOUT

    INFO   float grid_unit_size   8.0    metres between samples
           float height_scale     0.35
           float height_floor           lowest height in the map
           float height_ceiling         highest
           u16   grid_size        512    samples per side
           u16   height_patches   8
    HGT8   one byte per sample, RLE'd (the same codec as texture BODY)
    HEXP   per-patch value, SIGNED int16, 64 x 64
    LTEX   null-separated layer texture names
    DTEX   null-separated decal texture names
    SCAL   16 floats, per-layer scale        ROTN  16 floats, per-layer rotation
    AXIS   16 bytes, one per layer slot      DCAL  decal records, 24 bytes each
    PTCH   4 bytes per patch                 COLR / ALPH  see below

THE PATCH TABLE IS THE KEY TO COLR AND ALPH

    A map is divided into 8 x 8 sample patches - 64 x 64 of them on a 512 grid,
    16 x 16 on the 128-grid shell map. PTCH carries four bytes for each:

        u16 layerMask     which of the up to 16 LTEX layers blend in this patch
        u8  decalCount    how many DCAL records belong to this patch
        u8  reserved      zero on every patch of every map

    Neither COLR nor ALPH is a flat grid, which is why no amount of dividing
    their length by the map size ever produced a whole number. They are lists
    of variable-length per-patch blocks in patch order, each framed

        u16 packedLen; u16 unpackedLen;  then packedLen bytes

    stored verbatim when the two are equal and otherwise compressed with the
    SAME LZ as meshes and textures (mercs_lzss) - not the texture BODY RLE that
    an earlier version of this file assumed. ALPH holds a block only for
    patches whose layerMask has more than one bit set, because a patch drawing
    one layer needs no blend weights at all.

    unpackedLen is 162 for every COLR block in the game: 81 vertices - a patch
    is 8 x 8 cells and therefore 9 x 9 vertices, sharing its edge with the next
    patch - at two bytes each. For ALPH it is exactly 45 * popcount(layerMask),
    so 45 bytes per active layer.

WHY EACH OF THOSE IS BELIEVED, RATHER THAN MERELY FITTED

    Every claim above is a test that could have failed across all 14 maps:

      - walking COLR as (u16 len + 4) blocks consumes the chunk to the byte and
        yields exactly one block per patch; ALPH likewise for exactly the
        multi-layer patches, 53,504 and 20,989 blocks respectively
      - sum(decalCount) * 24 == len(DCAL), exactly, on all 14
      - no layerMask bit is ever set above the declared LTEX count
      - the three maps with no ALPH at all - blank, email, template - are
        precisely the three whose every patch has a single-bit mask
      - the LZ decodes all 51,231 compressed COLR blocks and all 20,989 ALPH
        blocks to their stated size, with no failures and nothing left over
      - decoded patch colours are edge-continuous: patch (x,y)'s last column
        equals patch (x+1,y)'s first on 100.00% of seams, read row-major, where
        a column-major read scores 6% and random pairs 2%

    That last one is the one worth keeping. It settles the 9 x 9 reading, the
    patch ordering and the two-bytes-per-vertex stride together, and it is not
    a metric that can be read backwards.

THE INSIDE OF AN ALPH PLANE, FROM THE CODE RATHER THAN FROM FITTING

    ps2RedTerrain.cpp walks it directly, and the loop settles every question
    the byte counts could not:

        for bit in 0..15:                 one plane per SET mask bit,
          if not (mask >> bit) & 1: skip  in ascending bit order
          for row in 0..8:                9 rows
            for col in 0..8:              9 columns - the shared-edge vertices
              if col is even: test *p & 0x04      low nibble
              else:           test *p & 0x40 ; p++   high nibble, then advance
            p++                           one extra byte at the END of each row

    Four advances inside the row plus one at the end is FIVE bytes a row, and
    nine rows is the 45 the header always states. One nibble per vertex, low
    then high, with the tenth nibble of each row unused padding.

    THE NIBBLE IS NOT A WEIGHT. The loop tests a single bit of it - 0x04 of the
    low nibble, 0x40 of the high - and when set writes the LAYER INDEX into a
    per-vertex byte grid. So this is a per-vertex layer selection, and whatever
    the other three bits do they are not consumed here.

    That also explains the thing that blocked this for a while: a patch-seam
    test scores COLR at a flat 100% and these nibbles at only 88%, which looked
    like a broken layout and is nothing of the kind. Selection flags have no
    reason to agree across a seam the way an interpolated colour does. The seam
    test was the right tool for COLR and the wrong one here, and reading the
    loop cost less than any of the fitting that preceded it.

HGT8 IS TILED 16 x 16. Read row-major it has a hard step every 16 columns and
none at all vertically, which is a layout artefact rather than terrain. Measured
over every plausible tile shape by total variation - a correctly de-tiled
heightmap is the smoothest one - 16x16 scores 3.55 against 18.51 for a
row-major read, a 5x improvement, and beats every other shape tried.

HEIGHTS. HGT8 covers exactly 0..255 across a map and INFO gives the floor and
ceiling, so a sample maps linearly onto that range:

    height = height_floor + h * (height_ceiling - height_floor) / 255

HEXP is read as SIGNED int16 - unsigned it appears to span the whole u16 range,
signed it spans -59..811 for aclubs, which is exactly (ceiling-floor)/height_scale
= 870. It is not needed to reconstruct the surface and is reported rather than
applied.

Read-only research - see the header of mercs_dsk.py.
"""
import argparse
import json
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mercs_lzss import decompress, STORED          # noqa: E402  (kept for parity)
from mercs_tex import rle_decode, write_png        # noqa: E402

TILE = 16


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


def cstr_list(buf):
    return [s.decode('ascii', 'replace') for s in buf.split(b'\0') if s]


def detile(src, grid, tw=TILE, th=TILE):
    out = bytearray(grid * grid)
    p = 0
    for ty in range(grid // th):
        for tx in range(grid // tw):
            for dy in range(th):
                row = (ty * th + dy) * grid + tx * tw
                out[row:row + tw] = src[p:p + tw]
                p += tw
    return out


PATCH = 8          # samples per patch side; 9 x 9 vertices with the shared edge
VERTS = 81         # 9 * 9
PLANE = 45         # bytes of ALPH per active layer
DCAL_REC = 24      # bytes per decal record


def popcount(v):
    return bin(v).count('1')


def unpack_patches(buf):
    """PTCH -> [(layerMask, decalCount), ...] in row-major patch order."""
    return [(struct.unpack_from('<H', buf, i * 4)[0], buf[i * 4 + 2])
            for i in range(len(buf) // 4)]


def patch_blocks(buf, count):
    """Yield the `count` framed blocks of a COLR or ALPH chunk.

    The frame is {u16 packedLen; u16 unpackedLen} and the walk is a running
    cursor, exactly as the loader does it - there are no offsets anywhere in
    the chunk, so a block can only be found by having consumed every block
    before it. Raises rather than truncating: a short read means the framing is
    wrong, and returning a partial list would hide that.
    """
    p = 0
    for i in range(count):
        packed, unpacked = struct.unpack_from('<HH', buf, p)
        yield buf[p + 4:p + 4 + packed], unpacked
        p += packed + 4
    if p != len(buf):
        raise ValueError('block walk ended at %d of %d bytes' % (p, len(buf)))


def unpack_block(block):
    """A framed block -> its bytes, stored verbatim or LZ'd."""
    payload, unpacked = block
    if len(payload) == unpacked:
        return payload
    out = decompress(payload, unpacked)
    out = out[0] if isinstance(out, tuple) else out
    if len(out) != unpacked:
        raise ValueError('block unpacked to %d, header said %d'
                         % (len(out), unpacked))
    return out


def write_vertex_colour(path, patches_verts, side):
    """The per-vertex terrain colour, stitched into one image.

    Patches share their edge vertices, so 64 patches of 9 vertices span
    64 * 8 + 1 = 513 columns rather than 576. The seam test in the header is
    what says the overlap is real and which way round the patches go.

    FIVE-FIVE-FIVE-ONE IS PROVEN; THE CHANNEL ORDER IS NOT.

    Bit 15 is set on 100% of 331,776 vertices, so it is a constant alpha rather
    than the top of a channel - that rules out 565 and fixes the layout as
    1555. The three 5-bit fields then have means of 17.4, 16.8 and 15.8, which
    is what baked lighting on mostly-grey ground looks like and is exactly why
    it cannot settle the ORDER: a red/blue swap moves those means by nothing.

    Read as ABGR1555 on the strength of the palette entries elsewhere in this
    game being A,B,G,R rather than A,R,G,B. Check it against a surface whose
    colour is known independently before trusting it - the README records the
    afternoon lost to confirming a channel order on asphalt and concrete.
    """
    n = side * PATCH + 1
    rgba = bytearray(n * n * 4)
    for pi, verts in enumerate(patches_verts):
        px, py = pi % side, pi // side
        for v in range(VERTS):
            c = struct.unpack_from('<H', verts, v * 2)[0]
            x = px * PATCH + v % 9
            y = py * PATCH + v // 9
            o = (y * n + x) * 4
            rgba[o + 0] = (c & 0x1F) << 3
            rgba[o + 1] = (c >> 5 & 0x1F) << 3
            rgba[o + 2] = (c >> 10 & 0x1F) << 3
            rgba[o + 3] = 255 if c & 0x8000 else 255
    write_png(path, n, n, bytes(rgba))


def plane_nibbles(buf):
    """One 45-byte ALPH plane -> 81 nibbles in vertex order.

    Mirrors the console loop exactly: nine rows of nine, low nibble on even
    columns and high on odd, the cursor stepping once per pair and once more at
    the end of every row. Written as the same walk rather than as an index
    formula so it stays comparable with the code it came from.
    """
    out, p = [], 0
    for _row in range(9):
        for col in range(9):
            out.append(buf[p] & 0x0F if col % 2 == 0 else buf[p] >> 4)
            if col % 2:
                p += 1
        p += 1
    return out


def write_layer_map(path, patches, planes_by_patch, side):
    """The per-vertex layer selection, stitched into one image.

    This is the terrain splat map: the value at each vertex is the LTEX slot
    the console would stamp there. Single-layer patches have no ALPH block at
    all, so their one layer covers the whole patch - which is why they can be
    filled straight from the mask.

    Stored as an 8-bit index in all three channels so it opens as a greyscale
    image, with the count of distinct layers returned for the caller to report.
    Do not read it as a picture: 16 adjacent indices are 16 near-identical
    greys, and it is a lookup table that happens to be rectangular.
    """
    n = side * PATCH + 1
    rgba = bytearray(b'\x00\x00\x00\xff' * (n * n))
    used = set()
    for pi, (mask, _dec) in enumerate(patches):
        px, py = pi % side, pi // side
        bits = [b for b in range(16) if mask >> b & 1]
        planes = planes_by_patch.get(pi)
        for v in range(VERTS):
            x, y = px * PATCH + v % 9, py * PATCH + v // 9
            if planes is None:
                layer = bits[0] if bits else 0
            else:
                layer = bits[0] if bits else 0
                for bi, bit in enumerate(bits):
                    if planes[bi][v] & 0x04:
                        layer = bit
            used.add(layer)
            o = (y * n + x) * 4
            rgba[o] = rgba[o + 1] = rgba[o + 2] = layer
    write_png(path, n, n, bytes(rgba))
    return len(used)


def write_patch_csv(path, patches, side):
    with open(path, 'w', encoding='utf-8') as f:
        f.write('patch,px,py,layer_mask,layer_count,layers,decal_count\n')
        for i, (mask, dec) in enumerate(patches):
            layers = ' '.join(str(b) for b in range(16) if mask >> b & 1)
            f.write('%d,%d,%d,0x%04X,%d,%s,%d\n'
                    % (i, i % side, i // side, mask, popcount(mask), layers, dec))


def write_decals(path, dcal, patches, side):
    """DCAL records, tagged with the patch that owns them.

    The records are 24 bytes and run in patch order, with PTCH's decalCount
    giving the length of each run - exact on every map.

    WHAT THE LOADER SAYS THE 24 BYTES ARE. RedTerrain.cpp reads eight single
    bytes and then eight s16, and keeps almost none of the first eight: two
    become the nibbles of a packed byte, one becomes a second byte, and five
    are read and dropped. The eight s16 are four (x, y) pairs on alternating
    axes - the mins over the odd and even positions are taken separately - and
    are then rebased onto `min & ~0x7FF` and shifted right 4, so they are fixed
    point with FOUR FRACTIONAL BITS.

    Two of those survive contact with the data on all 14 maps: byte 3 is zero
    and bytes 4..7 are 0xFFFFFFFF, one distinct value across every record in
    the game. Byte 2 is a valid DTEX index on the ten real maps and out of
    range on the four placeholders (blank, email, template, veh), which is
    suggestive and not proof.

    WHAT IS NOT SETTLED, AND WHY THE OBVIOUS READING IS WRONG. ps2RedTerrain.cpp
    consumes the RUNTIME struct - eight bytes, stride 8 - indexing a per-vertex
    grid as `(b0 >> 4) * (stride + 1) + (b0 & 0xF)` with stride + 1 = 9, which
    independently confirms the 9-wide vertex row. But those are the nibbles of
    the BUILT struct, not of the file, and testing file bytes against them
    fails: the values run past 8. Reading the s16 as world position also fails
    - the centroid lands in the owning patch 0.3% of the time.

    So the records go out as hex against their patch. That is honest about what
    is known, and it is still enough to place a road by patch and to check a
    future field guess against a decal whose position is already known.
    """
    n = 0
    with open(path, 'w', encoding='utf-8') as f:
        f.write('patch,px,py,record,bytes\n')
        for i, (_, count) in enumerate(patches):
            for k in range(count):
                rec = dcal[n * DCAL_REC:(n + 1) * DCAL_REC]
                f.write('%d,%d,%d,%d,%s\n' % (i, i % side, i // side, k, rec.hex()))
                n += 1
    return n


def write_obj(path, heights, grid, unit, lo, hi, step):
    """A grid mesh. step decimates - 512x512 is 262k vertices per level."""
    span = (hi - lo) / 255.0
    n = 0
    with open(path, 'w', encoding='utf-8') as f:
        f.write('# Mercenaries terrain, %dx%d samples at %.1f m, step %d\n'
                % (grid, grid, unit, step))
        for y in range(0, grid, step):
            for x in range(0, grid, step):
                f.write('v %.3f %.3f %.3f\n'
                        % (x * unit, lo + heights[y * grid + x] * span, y * unit))
                n += 1
        w = len(range(0, grid, step))
        for j in range(w - 1):
            for i in range(w - 1):
                a = j * w + i + 1
                f.write('f %d %d %d\nf %d %d %d\n'
                        % (a, a + 1, a + w, a + 1, a + w + 1, a + w))
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dsk', nargs='+')
    ap.add_argument('--out', required=True)
    ap.add_argument('--step', type=int, default=2,
                    help='decimation for the .obj (1 = full 512x512)')
    ap.add_argument('--no-obj', action='store_true')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    done = 0
    for path in args.dsk:
        data = open(path, 'rb').read()
        count = struct.unpack_from('<I', data, 0)[0]
        ents = [struct.unpack_from('<III', data, 8 + i * 12) for i in range(count)]
        off = 8 + count * 12
        seen = set()
        for size, h, grp in ents:
            blob = data[off:off + size]
            off += size
            if blob[8:12] != b'tern' or h in seen:
                continue
            seen.add(h)
            inner = blob[16:16 + struct.unpack_from('<I', blob, 12)[0]]
            d = dict(children(inner))
            if 'INFO' not in d or 'HGT8' not in d:
                continue
            name = d.get('NAME', b'').split(b'\0')[0].decode('ascii', 'replace') or '%08X' % h
            unit, hscale, lo, hi = struct.unpack_from('<4f', d['INFO'], 0)
            grid, hpatch = struct.unpack_from('<2H', d['INFO'], 16)

            raw, _ = rle_decode(d['HGT8'], grid * grid)
            if len(raw) < grid * grid:
                continue
            heights = detile(raw, grid)

            safe = ''.join(c if c.isalnum() or c in '_-' else '_' for c in name)
            # 8-bit grey PNG of the raw samples - the JSON carries the floor and
            # ceiling needed to turn a pixel back into metres.
            rgba = bytearray(grid * grid * 4)
            for i, v in enumerate(heights):
                rgba[i * 4:i * 4 + 4] = bytes((v, v, v, 255))
            write_png(os.path.join(args.out, safe + '_height.png'), grid, grid, rgba)
            with open(os.path.join(args.out, safe + '_height.raw'), 'wb') as f:
                f.write(bytes(heights))

            hexp = struct.unpack_from('<%dh' % (len(d.get('HEXP', b'')) // 2),
                                      d.get('HEXP', b''), 0) if 'HEXP' in d else ()
            meta = {
                'name': name, 'grid': grid, 'grid_unit_metres': unit,
                'height_floor': lo, 'height_ceiling': hi, 'height_scale': hscale,
                'height_patches': hpatch, 'map_size_metres': grid * unit,
                'height_formula': 'floor + sample * (ceiling - floor) / 255',
                'heightmap_tiling': '16x16, already undone in the png/raw',
                'layer_textures': cstr_list(d.get('LTEX', b'')),
                'decal_textures': cstr_list(d.get('DTEX', b'')),
                'hexp_signed_range': [min(hexp), max(hexp)] if hexp else None,
            }
            for tag in ('SCAL', 'ROTN'):
                if tag in d:
                    meta[tag.lower()] = list(struct.unpack_from(
                        '<%df' % (len(d[tag]) // 4), d[tag], 0))
            # --- the patch table, and the two chunks it addresses -----------
            patches = unpack_patches(d.get('PTCH', b''))
            side = grid // PATCH        # 64 on a 512 map, 16 on the 128 one
            meta['patch_side'] = side
            meta['patch_count'] = len(patches)
            meta['layer_axis'] = list(d.get('AXIS', b''))

            if patches and len(patches) == side * side:
                if 'COLR' in d:
                    verts = [unpack_block(b) for b in
                             patch_blocks(d['COLR'], len(patches))]
                    write_vertex_colour(
                        os.path.join(args.out, safe + '_colour.png'), verts, side)
                    meta['vertex_colour_grid'] = side * PATCH + 1
                want = [i for i, p in enumerate(patches) if popcount(p[0]) > 1]
                by_patch = {}
                if 'ALPH' in d:
                    raw = [unpack_block(b) for b in
                           patch_blocks(d['ALPH'], len(want))]
                    for pi, blob in zip(want, raw):
                        k = popcount(patches[pi][0])
                        by_patch[pi] = [plane_nibbles(blob[j * PLANE:(j + 1) * PLANE])
                                        for j in range(k)]
                    meta['alph_patches'] = len(want)
                    meta['alph_plane_bytes'] = PLANE
                meta['layers_used'] = write_layer_map(
                    os.path.join(args.out, safe + '_layers.png'),
                    patches, by_patch, side)
                write_patch_csv(os.path.join(args.out, safe + '_patches.csv'),
                                patches, side)
                if 'DCAL' in d:
                    n = write_decals(os.path.join(args.out, safe + '_decals.csv'),
                                     d['DCAL'], patches, side)
                    meta['decal_count'] = n
            with open(os.path.join(args.out, safe + '.json'), 'w', encoding='utf-8') as f:
                json.dump(meta, f, indent=2)

            if not args.no_obj:
                write_obj(os.path.join(args.out, safe + '.obj'), heights, grid,
                          unit, lo, hi, max(1, args.step))
            done += 1
            print('  %-10s %dx%d  %.0f m across  height %.1f..%.1f  %d layer textures'
                  % (name, grid, grid, grid * unit, lo, hi,
                     len(meta['layer_textures'])))
    print('  %d terrains -> %s' % (done, args.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
