# XCOM 2 test asset kit

Extracted from the XCOM 2 SDK. Rebuild the whole thing with:

```powershell
.\tools\xcom2\build_test_kit.ps1                 # extract + convert everything
.\tools\xcom2\build_test_kit.ps1 -SkipExtract    # reconvert from workbench\xcom_raw
.\tools\xcom2\build_test_kit.ps1 -Only cliff,rocks
```

The manifest at the top of that script is the source of truth; add a block
there to pull more. `tools/xcom2/xcom_extract.ps1` and `tools/xcom2/xcom_convert.py` do the
work and are documented in their headers.

**One folder per asset, and every folder is self-contained** — its meshes, its
textures and its `.mtl`, with nothing referenced across folders. Cliffs and
boulders share a rock atlas in the original and each keep their own copy; that
duplication is deliberate, so a folder can be dropped in without chasing
dependencies.

    cover/       shootable cover props, walls, fences, lamp posts
    vegetation/  plants and organic scatter
    ground/      man-made paving and road markings
    terrain/     natural ground, cliffs, rock
    interactive/ ladders
    vehicles/    cars

Everything is scaled so **one XCOM tile = 1.0**, matching `kTileSize` in
`src/core/lattice/Constants.hpp`. Meshes are Y-up, indexed, with generated
normals. 26 asset folders, 71 meshes, 105 textures.

---

## Read this before wiring up materials

Four things about XCOM's art will produce wrong-looking results if you assume
the usual conventions.

**1. The cutout mask is the MSK texture's BLUE channel — not the diffuse alpha.**
This is the big one; foliage renders as solid blobs without it. Verified across
maple, grass clumps, cattails, vines and trillium — blue std ≈ 85–120, red and
green ≈ 0.

| material | alpha-test source |
|---|---|
| `maple`, `grassclump`, `cattail`, `vines`, `forest_scatter` | blue channel of `*_msk.png` |
| `bush`, `ferns` | `bush_opacity.png` / `ferns_opacity.png` (any channel) |
| everything in `cover/`, `ground/`, `terrain/` | none — render opaque |

**2. Diffuse alpha is NOT opacity.** `jersey_dif.png` ranges the full 0–255 on
alpha, but the barrier is solid concrete — that channel is packed data.
Alpha-testing on it punches holes through a third of the mesh.

**3. `maple_*` is multi-material and cannot be split.** UnrealEd's OBJ exporter
merges every material section into a single `g UnrealEdObject` group, so the
trunk/canopy split is unrecoverable. The tree still reads well on the branches
atlas alone; `maple_bark_*.png` sits in the same folder for a hand-built trunk
material. Every other mesh in the kit is single-material.

**4. Pivots are not centred.** Meshes keep XCOM's authored origin: tile corner,
Y=0 at the floor. That is what makes them line up when placed at a tile origin.
Sizes below often exceed the nominal footprint because end posts deliberately
overhang so segments interlock.

Two smaller notes: raylib's `LoadOBJ` applies `1.0f - v` itself
([rmodels.c:4399](../../builds/_cmake-win/_deps/raylib-src/src/rmodels.c#L4399)), so these
files stay in standard OBJ convention — do not flip V again. And normals here
are *generated* (45° smoothing threshold); UE3 exports none at all.

---

## cover/

XCOM encodes cover class and tile footprint in its own mesh names, and the
output names preserve it: `LoCov`/`LoCover` low, `HiCov`/`HiCover` full,
`1x1`/`x2` the footprint.

| mesh | tris | size (tiles) |
|---|---|---|
| **cinderblock_wall** | | |
| `wall_hi_x1` | 596 | 1.12 x 1.33 x 0.12 |
| `wall_lo_x1` | 360 | 1.12 x 0.67 x 0.12 |
| `wall_hi_x1_destroyed` | 156 | 1.00 x 0.11 x 0.13 |
| **sandbags** | | |
| `sandbags_lo_x1` | 1914 | 1.21 x 0.71 x 0.88 |
| `sandbags_lo_x2` | 3997 | 2.27 x 0.68 x 0.92 |
| `sandbags_lo_corner` | 1888 | 1.11 x 0.64 x 1.15 |
| **jersey_barrier** | | |
| `jersey_lo_x1` / `jersey_lo_x2` | 903 / 300 | 1.04 / 2.00 x 0.67 |
| **crates** | | |
| `crate_hi_1x1` | 1595 | 0.77 x 1.38 x 0.94 |
| `crate_lo_1x1` | 617 | 0.97 x 0.58 x 0.96 |
| `crate_debris` | 221 | 1.10 x 0.16 x 0.88 |
| **barrel** | `barrel_lo_1x1`, `_b` | 960 / 3264 |
| **guardrail** | `guardrail_lo_x2`, `_cap` | 334 / 940 |
| **hydrant** | `hydrant_lo_1x1` | 968 |
| **picnic_table** | `picnic_table`, `_destroyed` | 356 / 1126 |

### walls and fences

| mesh | tris | size (tiles) |
|---|---|---|
| **boundary_wall** `boundary_hi_x3` | 1728 | 3.02 x 2.00 x 0.92 |
| **boundary_wall** `boundary_lo_x2` / `_x3` | 230 / 232 | 2.00 / 3.02 x 0.66 |
| **stone_wall** `stone_lo_x1` / `_x2` | 3854 / 6558 | 1.19 / 2.11 x ~0.75 |
| **stone_wall** `stone_deco_x1` | 1258 | 1.05 x 0.26 x 0.45 |
| **brick_fence** `brick_lo_x1` / `_x2` | 236 / 346 | 1.02 / 2.02 x 0.67 |
| **brick_fence** `brick_post` | 86 | 0.23 x 0.82 x 0.23 |
| **wooden_fence** `wood_fence_lo_x2` / `_cap` | 986 / 1137 | ~2.1 x 0.77 |
| **wooden_fence** `wood_fence_destroyed` | 469 | 2.07 x 0.19 x 0.43 |
| **privacy_fence** `privacy_hi_x2` / `_cap` | 414 / 454 | ~2.1 x 1.40 x 0.17 |
| **privacy_fence** `privacy_hi_destroyed` | 104 | 2.24 x 0.08 x 0.63 |

`privacy_fence` (1.40) and `boundary_hi_x3` (2.00, an ADVENT wall) are the
full-height runs; everything else is waist-high, which is what most XCOM
fencing is. `stone_wall` is a dry-stone pile and by far the heaviest per tile
(6558 tris for two tiles) — useful as a stress case.

### lamp posts

| mesh | tris | size (tiles) |
|---|---|---|
| **street_light** `street_light_hi_1x1` | 3524 | 2.61 x 5.94 x 1.05 |
| **street_light** `street_light_destroyed` | 782 | 0.74 x 0.71 x 0.70 |
| **park_lamp** `park_lamp_lo_1x1` | 1188 | 0.89 x 4.01 x 1.27 |
| **light_post** `light_post_1x1` / `_c` | 1964 / 3490 | ~1.6 x 5.31 |
| **light_post** `wall_light_1x1` | 938 | 0.39 x 0.39 x 0.56 |

All three are destructible; one destroyed state is included for the street
light (the packages carry `Chunk` debris pieces too). `light_post` ships an
extra `light_post_emis.png` — an emissive lit-lamp mask, not a colour map, so
feed it to emission rather than diffuse.

Low cover clusters at ~0.67 tiles — exactly `kCellHeight`, XCOM's 64uu low
cover. `crate_hi_1x1` at 1.38 and `wall_hi_x1` at 1.33 are full cover.
`crate_debris`, `wall_hi_x1_destroyed`, `picnic_table_destroyed`,
`wood_fence_destroyed` and `privacy_hi_destroyed` are destroyed states.

## vegetation/

| mesh | tris | size (tiles) |
|---|---|---|
| **maple** `maple_small` / `maple_med` | 3956 / 7545 | 3.66 x 6.11 / 4.44 x 7.74 |
| **maple** `maple_small_destroyed` | 512 | 1.25 x 1.56 x 1.40 |
| **logs_stumps** `stump_hi_1x1` | 685 | 1.30 x 2.30 x 1.49 |
| **logs_stumps** `stump_lo_1x1` | 559 | 1.26 x 1.12 x 1.07 |
| **logs_stumps** `log_hi_2x2` | 2968 | 2.13 x 1.30 x 1.87 |
| **logs_stumps** `log_lo_1x2` / `log_lo_1x3` | 704 / 955 | 1.21 x 0.59 x 2.68 / 3.76 |
| **logs_stumps** `log_scatter` | 261 | 1.10 x 0.18 x 2.68 |
| **bush** `bush_1x1` / `bush_2x2` | 312 / 2427 | 0.50 / 2.67 |
| **ferns** `ferns_1x1` / `ferns_2x2` | 156 / 1120 | 1.29 / 3.08 |
| **grass_clump** `grass_clump_1x1` / `_2x2` / `_tall_1x1` | 394 / 958 / 394 | ~1.3 / ~2.4 |
| **grass_clump** `weeds_1x1` | 801 | 1.00 x 0.28 x 1.15 |
| **cattail** `cattail` / `cattail_b` | 59 / 346 | ~1.05 |
| **vines** `vines_1x1` / `_b` | 1337 / 1960 | 1.33 x 0.55 / 1.02 x 1.31 |
| **forest_scatter** `_a` `_c` `_e` `_g` | 171–494 | 1.6–3.2 x ~0.1 x 1.7–5.3 |

Grass clumps, weeds and forest scatter (fallen twigs and branches) are real
geometry, not textures on quads — scatter them across ground planes. Logs and
stumps are the forest's natural cover, and carry the same `LoCov`/`HiCov`
naming as the man-made props.

`TreeStumpCover` ships no textures of its own; it paints from
`Foliage_Temperate`'s maple bark, which is what the `bark_*.png` here are.

**Avoid the `CityCenterTree` package** if you go looking for more trees — all
six of its textures are flat 64×64 colour swatches, i.e. placeholders. The real
tree art is in `Foliage_Temperate`.

## ground/ — man-made paving

| mesh | tris | size (tiles) |
|---|---|---|
| **road** `road_straight_8x8` | 384 | 8.00 x 0.17 x 6.00 |
| **road** `road_corner_8x8` | 447 | 7.00 x 0.17 x 7.00 |
| **road** `road_intersection_8x8` | 552 | 8.00 x 0.17 x 8.00 |
| **sidewalk** `sidewalk_str_x2` / `_x4` | 108 / 172 | 2.04 / 4.04 x 0.19 x 1.34 |
| **sidewalk** `sidewalk_corner_2x2` | 189 | 2.19 x 0.19 x 2.19 |
| **sidewalk** `sidewalk_plate_4x4` | 268 | 4.04 x 0.04 x 4.04 |
| **sidewalk** `sidewalk_raised_x9` | 780 | 1.85 x 0.15 x 9.85 |
| **sidewalk** `curb_str_x4` | 48 | 4.00 x 0.19 x 0.17 |

Footprints are in the names and the converted sizes confirm them exactly.
Sidewalks sit 0.19 tiles proud — a curb height — so they drop straight onto a
ground plane. `sidewalk_raised_x9` is a raised median island.

`concrete/` is textures only: smooth, worn and cracked, plus a mask.

### road_markings — why the roads have no yellow lines

The road plates are bare asphalt **by design**. XCOM paints every line,
crosswalk and arrow with separate decal geometry laid over the plate, out of
`RoadDetails.upk`. `road_straight_8x8` has no centre line because it never had
one. Two ways to use what is here:

* `yellow_stripe_1x/4x/8x`, `white_stripe_8x`, `tar_strip_8x` — thin overlay
  strips (0.12 tiles wide) you lay along a plate;
* `road_yellow_straight_16x8` (8.00 x 0.02 x 9.06) and `road_yellow_corner_8x8`
  — pre-marked full-plate overlays sized to drop straight onto the road plates;
* `crosswalk`, `arrow`, `intersection_markings` (a full 16x16 set);
* `pothole`, `skidmark`, `cracked_road`, `asphalt_patch` — wear decals on a
  second material (`road_wear`).

**The stripe colour is not in the texture.** `YellowStripeDecal_*` and
`WhiteStripeDecal_*` share one `RoadStripes_MAT` instance, and both sample the
same perfectly neutral grey region of `stripes_dif.png` — saturation 0, with or
without the material's `TileU=2`. The yellow comes from per-instance data on
the `XComLevelActor` archetype (its `StaticMeshComponent`), which UE3's OBJ
exporter does not write; OBJ has no vertex-colour channel at all. So **tint the
stripe decals in your material** — the grey is a clean base to multiply. The
crosswalk is the exception: it carries real colour in the atlas.

These are decals, so they need the same masked treatment as foliage — alpha-cut
from `stripes_msk.png`'s blue channel — plus a depth bias against z-fighting
with the plate underneath. `roadwear_*` has no usable MSK (the source is empty),
so cut those from the diffuse alpha instead.

## terrain/ — natural

| mesh | tris | size (tiles) |
|---|---|---|
| **cliff** `cliff_a` | 18512 | 5.36 x 3.36 x 4.52 |
| **cliff** `cliff_c` | 23319 | 6.34 x 3.27 x 6.38 |
| **cliff** `cliff_f` | 27623 | 6.54 x 3.73 x 7.44 |
| **cliff** `cliff_lo_3x3` / `cliff_lo_6x6` | 4560 / 6395 | 3.18 / 6.35 x ~0.9 |
| **cliff** `cliff_bolton_hi_1x1` | 834 | 1.15 x 2.78 x 1.30 |
| **cliff** `cliff_bolton_lo_1x1` | 1262 | 1.35 x 0.86 x 1.10 |
| **cliff** `rock_ladder_256` | 4407 | 1.56 x 2.93 x 1.69 |
| **rocks** `rock_hi_1x1` / `rock_hi_2x2` | 3460 / 1095 | 0.98 x 1.42 / 1.78 x 1.54 |
| **rocks** `rock_deco_1x1` / `_2x2` / `_4x4` | 1300 / 2993 / 3500 | 0.73 / 2.16 / 3.67, all ~0.2 tall |
| **marsh** `marsh_a` | 3151 | 16.00 x 0.85 x 32.00 |
| **marsh** `lake_a` | 2268 | 16.00 x 0.67 x 16.00 |
| **groundplane** `groundplane_8x8` / `_8x16` | 512 / 1024 | 8x8 / 16x8, flat |
| **groundplane** `groundplane_open` / `_skirt_md` | 1008 / 512 | 16x16 open frames |

The cliffs are the interesting part for the lattice. `TemperatePlateaus`
carries **no textures of its own** — it paints with the `TemperateRocks`
atlases, which is why the cliff folder holds a copy. Note what the names say:

* `cliff_lo_*` are low-cover-height plateau slabs (~0.9 tiles, one z-cell-ish)
  meant to be stood on, and their `LowCover_3x3`/`6x6` names are XCOM's own.
* `cliff_bolton_*` are "bolt-on" pieces — standing rock spires you attach to a
  plateau edge to make it shootable cover, tagged `HiCover`/`LoCover`.
* `rock_ladder_256` is a climbable rock face. **256uu = 4 z-cells exactly**
  (`kCellHeight` is 64uu), so it is a ready-made test case for `LadderQuery`.

The big `cliff_a/c/f` forms are 18k–28k tris — organic, non-tile-aligned, and
the least convenient thing here for a voxel lattice. They are included
precisely so you can see how badly they fit.

`marsh_a` is a 16×32-tile terrain plate with a stream channel; `lake_a` is
16×16. Ground planes are flat grids that carry no material in the original
(each map blends several terrain libraries across them) and are pointed at
wildland dirt here so they load textured.

Tiling natural surfaces, textures only:

| folder | contents |
|---|---|
| `terrain/dirt` | `dirt_a/b/c` (WLD wildland dirt), `dirtroad_moss`, `dirtroad_stones`, `ground_msk` |
| `terrain/grass` | `grass_a/b` (WLD), `grass_c/d` |
| `terrain/forest_floor` | `leaves`, `roots`, `mud`, `mud_tracks`, `rocks` |

`WLD` is XCOM's wildland/temperate climate prefix; `ARD` is arid and `TND`
tundra, both present in the same source packages if you want other biomes.

## interactive/ — ladders

| mesh | tris | size (tiles) |
|---|---|---|
| `ladder_metal_192` | 632 | 0.31 x 2.28 x 0.64 |
| `ladder_metal_256` | 728 | 0.31 x 2.95 x 0.64 |
| `ladder_metal_512` | 1264 | 0.31 x 5.61 x 0.64 |
| `ladder_hatch_256` | 560 | 0.31 x 2.30 x 0.65 |
| `ladder_wood_192` | 456 | 0.31 x 2.25 x 0.66 |
| `ladder_wood_256` | 1208 | 0.14 x 2.92 x 0.68 |
| `ladder_wood_512` | 2344 | 0.14 x 5.58 x 0.68 |

**The number in each name is the climb height in unreal units**, and they land
on the lattice exactly: 192uu = `kStoreyHeight` (3 z-cells), 256uu = 4 cells,
512uu = 8 cells. Together with `terrain/cliff/rock_ladder_256` that is four
ready-made `LadderQuery` cases at three distinct heights. Measured heights run
a little over nominal because the meshes include mounting brackets and rails
above the top rung.

## vehicles/

| mesh | tris | size (tiles) |
|---|---|---|
| `sedan` | 13427 | 2.04 x 1.13 x 4.03 |
| `sedan_nodoors` / `sedan_burned` | 12904 / 7130 | 2.04 x ~1.1 x 4.03 |
| `sedan_door` | 396 | 0.24 x 0.56 x 1.08 |
| `minivan` | 11331 | 1.99 x 1.50 x 4.09 |
| `minivan_destroyed` | 10790 | 2.15 x 1.49 x 4.03 |
| `car_wreck` / `car_wreck_flipped` | 6632 / 6416 | 1.87 / 2.07 x ~1.1 x ~4 |

Cars occupy roughly **2 x 4 tiles** and stand ~1.1–1.5 tiles, so they are full
cover along the flank and their footprint spans several tiles — a good test for
multi-tile occupancy. The sedan and minivan ship their damage states as
separate meshes (XCOM swaps them on destruction, same pattern as the
cinderblock wall). `sedan_door` is a detachable panel. These are the heaviest
meshes in the kit at 11k–13k tris.

The sedan's diffuse is a near-neutral base — XCOM tints car paint per instance,
so expect to multiply a colour in rather than getting a red car for free.

---

## Resolution and rebuilding

Props and vegetation are capped at 512 px, terrain and paving at 1024 px.
Raise with `-PropSize 1024 -TerrainSize 2048`; drop with `-TerrainSize 512`,
which is the cheapest big saving since the tiling surface sets dominate.

Raw intermediates live in `workbench/xcom_raw/` (gitignored) and can be deleted
once converted — keeping them makes `-SkipExtract` instant.

Note `.gitignore` has `*.obj` for MSVC object files, with a
`!assets/models/**/*.obj` negation so these meshes are tracked.

## Licensing

Firaxis/2K assets from the XCOM 2 SDK. The SDK licenses them for XCOM 2 mods,
which does not extend to shipping them in a separate game. Fine as local
prototype and blockout reference; they need replacing before any public
release. Not redistributable on their own.
