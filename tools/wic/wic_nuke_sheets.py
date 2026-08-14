"""Contact sheets for the World in Conflict nuke's source art.

Run from the repo root; reads wic_extracted/raw and writes wic_extracted/_sheets.
Nothing here is committed -- Massive's art is read for reference only -- so this
script IS the record, and re-running it is how the pictures come back.

Why these exist. The flipbooks are NOT stored frame-major with a mip chain per
frame, which is what the first version of wic_tex.py assumed. Each is a single
grid atlas with exactly one mip level for the whole sheet. The size equation
cannot separate the two layouts -- 128*(128*128*4 + 64*64*4) and
2048*1024*4 + 1024*512*4 are the same 10,485,760 bytes -- and neither can the
mip-agreement test, because a stack of smoke frames also decodes smoothly. Only
the pixels can: read as a stack, every frame is a comb of interleaved rows.

So a contact sheet is not decoration here. It is the only check that caught it,
and it is why wic_tex.to_png now writes the whole grid rather than frame 0.

    py -3 tools/wic/wic_nuke_sheets.py [raw-dir] [out-dir]
"""
import os, sys, numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wic_tex

RAW = sys.argv[1] if len(sys.argv) > 1 else 'wic_extracted/raw'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'wic_extracted/_sheets'
os.makedirs(OUT, exist_ok=True)


def dxt5_alpha(data, w, h, offset=0):
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    a = np.frombuffer(data[offset:offset + bw * bh * 16], np.uint8).reshape(bh * bw, 16)
    a0, a1 = a[:, 0].astype(np.int16), a[:, 1].astype(np.int16)
    bits = np.zeros(len(a), np.uint64)
    for i in range(6):
        bits |= a[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    pal = np.zeros((len(a), 8), np.int16)
    pal[:, 0], pal[:, 1] = a0, a1
    gt = a0 > a1
    for i in range(1, 7):
        pal[:, i + 1] = np.where(gt, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        pal[:, i + 1] = np.where(gt, pal[:, i + 1], ((5 - i) * a0 + i * a1) // 5)
    pal[:, 6] = np.where(gt, pal[:, 6], 0)
    pal[:, 7] = np.where(gt, pal[:, 7], 255)
    idx = np.zeros((len(a), 16), np.int64)
    for i in range(16):
        idx[:, i] = (bits >> np.uint64(3 * i)) & np.uint64(7)
    tex = np.take_along_axis(pal, idx, 1).astype(np.uint8)
    img = tex.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3)
    return img.reshape(bh * 4, bw * 4)[:h, :w]


def load(rel):
    return open(os.path.join(RAW, rel), 'rb').read()


def atlas(rel, fmt, W, H):
    """Level 0 of a grid atlas -> (rgb, alpha|None)."""
    d = load(rel)
    rgb = wic_tex.decode(d, fmt, W, H, 0)
    if fmt == 'A8R8G8B8':
        a = np.frombuffer(d[:W * H * 4], np.uint8).reshape(H, W, 4)[:, :, 3]
    elif fmt == 'DXT5':
        a = dxt5_alpha(d, W, H, 0)
    else:
        a = None
    return rgb, a


def tile(img, fw, fh, i, cols):
    r, c = divmod(i, cols)
    return img[r * fh:(r + 1) * fh, c * fw:(c + 1) * fw]


def gray3(a):
    return np.repeat(a[:, :, None], 3, 2)


def sheet(tiles, cols, px, labels, pad=6, lab=15, bg=(16, 16, 18)):
    rows = (len(tiles) + cols - 1) // cols
    im = Image.new('RGB', (cols * (px + pad) + pad, rows * (px + pad + lab) + pad), bg)
    dr = ImageDraw.Draw(im)
    for k, t in enumerate(tiles):
        r, c = divmod(k, cols)
        x, y = pad + c * (px + pad), pad + r * (px + pad + lab)
        im.paste(Image.fromarray(t).resize((px, px), Image.LANCZOS), (x, y))
        dr.text((x + 1, y + px + 2), labels[k], fill=(140, 140, 148))
    return im


# --- 1. the six-point pair -------------------------------------------------
p_rgb, p_a = atlas('hit_set_effects/effect_textures/smokeanimations/new_nmap/'
                   'rotatesmokefill_6p_0pos_128x128_128.dds', 'A8R8G8B8', 2048, 1024)
n_rgb, _ = atlas('hit_set_effects/effect_textures/smokeanimations/new_nmap/'
                 'rotatesmokefill_6p_1neg_128x128_128.dds', 'R8G8B8', 2048, 1024)
idx = [0, 16, 32, 48, 64, 80, 96, 112]
tiles, labels = [], []
for i in idx:
    tiles.append(tile(p_rgb, 128, 128, i, 16)); labels.append(f'_0pos rgb  f{i}')
for i in idx:
    tiles.append(gray3(tile(p_a, 128, 128, i, 16))); labels.append(f'_0pos alpha  f{i}')
for i in idx:
    tiles.append(tile(n_rgb, 128, 128, i, 16)); labels.append(f'_1neg rgb  f{i}')
sheet(tiles, 8, 128, labels).save(f'{OUT}/01_sixpoint_pair.png')

# --- 2. billowing smoke thick pair -----------------------------------------
b1_rgb, b1_a = atlas('hit_set_effects/effect_textures/smokeanimations/new_nmap/'
                     'billowingsmokethick_1_128x128_64f.dds', 'A8R8G8B8', 1024, 1024)
b2_rgb, _ = atlas('hit_set_effects/effect_textures/smokeanimations/new_nmap/'
                  'billowingsmokethick_2_128x128_64f.dds', 'R8G8B8', 1024, 1024)
idx2 = [0, 8, 16, 24, 32, 40, 48, 56]
tiles, labels = [], []
for i in idx2:
    tiles.append(tile(b1_rgb, 128, 128, i, 8)); labels.append(f'_1 rgb  f{i}')
for i in idx2:
    tiles.append(gray3(tile(b1_a, 128, 128, i, 8))); labels.append(f'_1 alpha  f{i}')
for i in idx2:
    tiles.append(tile(b2_rgb, 128, 128, i, 8)); labels.append(f'_2 rgb  f{i}')
sheet(tiles, 8, 128, labels).save(f'{OUT}/02_billowing_pair.png')

# --- 3. the fireball flipbooks ---------------------------------------------
ex_rgb, _ = atlas('hit_set_effects/effect_textures/explosions/'
                  'explosions_128x128_128f.dds', 'DXT1', 2048, 1024)
ef_rgb, ef_a = atlas('hit_set_effects/effect_textures/explosions/'
                     'explowithfireend_128x128_64f.dds', 'DXT5', 1024, 1024)
sr_rgb, sr_a = atlas('hit_set_effects/effect_textures/nukespecial/'
                     'smokerotate_nuke_64x64_64f.dds', 'DXT5', 512, 512)
tiles, labels = [], []
for i in [0, 8, 16, 24, 32, 48, 64, 96]:
    tiles.append(tile(ex_rgb, 128, 128, i, 16)); labels.append(f'Explosions f{i}')
for i in idx2:
    tiles.append(tile(ef_rgb, 128, 128, i, 8)); labels.append(f'ExploFireEnd f{i}')
for i in idx2:
    tiles.append(tile(sr_rgb, 64, 64, i, 8)); labels.append(f'SmokeRotate_Nuke f{i}')
sheet(tiles, 8, 128, labels).save(f'{OUT}/03_fireball_flipbooks.png')

# alpha of the two that have it, since they are drawn additive+alpha
tiles, labels = [], []
for i in idx2:
    tiles.append(gray3(tile(ef_a, 128, 128, i, 8))); labels.append(f'ExploFireEnd .a f{i}')
for i in idx2:
    tiles.append(gray3(tile(sr_a, 64, 64, i, 8))); labels.append(f'SmokeRotate .a f{i}')
sheet(tiles, 8, 128, labels).save(f'{OUT}/03b_fireball_alpha.png')

# --- 4. flares, rings, haze -------------------------------------------------
singles = [
    ('Flares/NukeFlare', 'flares/nukeflare.dds', 'DXT1', 256, 256),
    ('Flares/SmallBulletHit', 'flares/smallbullethit.dds', 'DXT1', 128, 128),
    ('Flares/Flare_01', 'flares/flare_01.dds', 'DXT1', 256, 256),
    ('Flares/ExplosionFlare_2', 'flares/explosionflare_2.dds', 'DXT1', 256, 256),
    ('Special/BlastRing_2', 'special/blastring_2.dds', 'DXT1', 128, 128),
    ('Waves/BlastWave_Alpha', 'waves/blastwave_alpha.dds', 'DXT5', 128, 256),
    ('Waves/BlastWave_DistortAlpha', 'waves/blastwave_distortalpha.dds', 'DXT5', 64, 128),
]
tiles, labels = [], []
for name, rel, fmt, w, h in singles:
    rgb, a = atlas('hit_set_effects/effect_textures/' + rel, fmt, w, h)
    sq = np.zeros((max(w, h), max(w, h), 3), np.uint8); sq[:h, :w] = rgb
    tiles.append(sq); labels.append(name)
    if a is not None:
        sa = np.zeros((max(w, h), max(w, h)), np.uint8); sa[:h, :w] = a
        tiles.append(gray3(sa)); labels.append(name + '  (alpha)')
dv = np.array(Image.open(os.path.join(RAW, '_dummy_objects/heathaze_testobject/dudvtest.tga')).convert('RGB'))
tiles.append(dv); labels.append('DudvTest  (heat haze)')
sheet(tiles, 5, 160, labels).save(f'{OUT}/04_flares_rings.png')

# --- 5. FireAni, the mesh atlas --------------------------------------------
fa_rgb, _ = atlas('hit_set_effects/effect_textures/fireanimations/'
                  'fireani_128x128_64f.dds', 'DXT1', 1024, 1024)
Image.fromarray(fa_rgb).resize((768, 768), Image.LANCZOS).save(f'{OUT}/05_fireani_atlas.png')

for f in sorted(os.listdir(OUT)):
    p = os.path.join(OUT, f)
    print(f, os.path.getsize(p), Image.open(p).size)
