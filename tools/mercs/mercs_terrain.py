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
    COLR   vertex colour, RLE'd              ALPH  layer blend weights, RLE'd
    PTCH   per-patch flags, 64 x 64 u32      DCAL  decal placement

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
            for tag in ('COLR', 'ALPH'):
                if tag in d:
                    out, used = rle_decode(d[tag], 1 << 24)
                    with open(os.path.join(args.out, '%s_%s.bin' % (safe, tag.lower())),
                              'wb') as f:
                        f.write(out)
                    meta[tag.lower() + '_bytes'] = len(out)
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
