> **Helldivers 2** has its own toolchain in this folder — `hd2_extract.ps1`,
> `hd2_unpack.py`, `hd2_dsar.py`, `hd2_index.py`. Different game, different
> engine (Stingray, not UE3), no shared code. See the end of this file.

# XCOM 2 SDK asset toolchain

Pull art and audio out of the SDK, convert it into something loadable, resolve
its materials, and index it so parcels can be assembled by query rather than by
hand. Every step is headless — no editor GUI session is ever needed.

Requires the XCOM 2 SDK (for `Binaries\Win64\XComGame.com`) and Python 3 with
`numpy` and `Pillow`. Pass `-SdkRoot` if your SDK is not at
`E:\SteamLibrary\steamapps\common\XCOM 2 SDK`. Audio additionally needs
[vgmstream](https://github.com/vgmstream/vgmstream/releases) — not vendored
here, pass `-VgmStream <path>`.

| script | job |
|---|---|
| `xcom_extract.ps1` | one package → raw OBJ + TGA |
| `xcom_convert.py` | raw → indexed OBJ with normals + PNG |
| `xcom_bulk.ps1` | sweep the whole SDK into `xcom_extracted/models/` |
| `xcom_materials.ps1` / `.py` | recover mesh → texture assignments, write `.mtl` |
| `xcom_index.py` | index/query the library |
| `xcom_parcel.ps1` / `.py` | a parcel's layout → placement CSV |
| `xcom_parcel_render.py` | **reference implementation: how to consume it all** |
| `xcom_audio.ps1` / `.py` | Wwise `.wem`/`.bnk` → named WAVs |
| `build_test_kit.ps1` | the curated, committed kit in `assets/models/` |
| `xcom_publish.ps1` | verify/relocate a finished library |

## The two asset tiers

**`assets/models/`** — the curated kit, committed. ~40 hand-picked assets, one
self-contained folder each, with hand-verified materials. Rebuilt by
`build_test_kit.ps1`; see `assets/models/README.md`.

**`xcom_extracted/`** — the full sweep, gitignored, ~70 GB. Everything the SDK
holds. Materials ARE resolved here (see below), so it is directly usable.

```
xcom_extracted/
    models/<Package>/            .obj + .png + <Package>.mtl
    models/index.csv             10107 meshes: size, cover class, footprint
    models/materials.csv         mesh -> material -> diffuse/normal/mask
    audio/_common/<Bank>/*.wav   SFX, weapons, footsteps, music, ambience
    audio/English_US_/<Bank>/    voice
    parcels/<Parcel>/            placements.csv
    wem_names.csv                audio id -> name
```

Current contents: **1759 packages, 10107 meshes, 976 packages with geometry,
290,657 WAVs**. 6570 meshes resolve to a diffuse and have `mtllib`/`usemtl`
patched into the OBJ. The unresolved remainder is mostly FX, UI and engine
materials — for environment art the rate is ~97%.

## Using the assets in the prototype

Start with **`xcom_parcel_render.py`**. It is a throwaway software rasteriser,
but its `place()` and `get_tex()` functions are the two things worth porting:
the exact transform order for placing a prop, and the correct way to find its
textures. Its header documents both, including the mirroring and alpha-cutout
traps. Run it on any extracted parcel to see the result:

```powershell
py -3 tools\xcom_parcel_render.py md_Forest_01  # -> workbench\parcel_<name>.png
```

Three rules that are easy to get wrong and expensive to debug:

1. **Meshes are Y-up and pre-scaled: 1.0 == one XCOM tile (96uu).** Placement
   coordinates in `placements.csv` are divided by 96 too, so a placement and a
   mesh vertex are directly comparable. `kTileSize` in
   `src/core/lattice/Constants.hpp` is the same 1.0.
2. **Never guess a texture from the mesh's folder** — use `materials.csv`.
   ~1/3 of packages ship no textures and paint from a shared library.
3. **Alpha cutout is the MSK's BLUE channel, foliage only.** A prop's diffuse
   alpha is packed data, not opacity; masking on it puts holes in solid walls.

## Rebuilding from scratch

```powershell
.\tools\xcom_bulk.ps1 -AllContent                    # models + textures
.\tools\xcom_materials.ps1 -WriteMtl -PatchObj       # pair them up
.\tools\xcom_audio.ps1 -VgmStream <path>             # loose .wem (voice)
.\tools\xcom_audio.ps1 -VgmStream <path> -Banks      # .bnk contents (all SFX)
```

Resumable — a package whose output folder exists is skipped, so an interrupted
run costs nothing. Raw TGA/OBJ is deleted per package after conversion, which
keeps staging at ~2 GB instead of ~100 GB.

**Parallelism.** One run uses one core. Split with `-Slice i -Of n` and launch
`n` copies; slices are disjoint and each package owns its output folder, so
workers never contend. Six workers took the sweep from ~2.9 h to ~45 min. Only
the unsliced run writes the index, so index once at the end with `-IndexOnly`.

`-AllContent` excludes only `Voices`, `Sound`, `Sounds` and `GameData` — audio
and config, which hold no meshes. **Characters, Weapons, FX, Cinematics and
Strategy are all included**: that is where the modular soldier parts
(`Central_Torso`, `Central_Legs`, `Central_Arms`) and weapon attachments come
from.

## Querying the library

`xcom_extracted/models/index.csv` has one row per mesh: package, name, tris,
tile dimensions, and **cover class / tile footprint / climb height parsed from
XCOM's own naming** — reliable because Firaxis' environment art encodes all
three.

```powershell
py -3 tools\xcom_index.py --library xcom_extracted\models --query "high 1x1"
py -3 tools\xcom_index.py --library xcom_extracted\models --query "ladder"
```

Of 10107 meshes: 415 high cover, 711 low, 1153 deco; the rest carry no cover
tag — usually structural or decorative. Footprints skew heavily to `1x1`, then
`x2`, `x1`, `1x2`, `2x2`. Ladders appear at 192/256/384/448/512 uu, all whole
z-cell multiples (64uu), so they drop straight onto the lattice.

## Parcels

```powershell
.\tools\xcom_parcel.ps1 -List
.\tools\xcom_parcel.ps1 -Parcel md_Advent_Security_03
.\tools\xcom_parcel.ps1 -Parcel lg_Museum_01 -Summary
```

Every placed prop in a parcel is an `XComLevelActor` (destructibles derive from
it), and BatchExport writes each to `.T3D` with its `StaticMesh` reference,
`Location`, `Rotation` and `DrawScale3D`. `xcom_parcel.py` turns that directory
into `placements.csv`.

Coordinates come out **divided by 96**, the same scale as the converted meshes,
so a placement and a mesh vertex are directly comparable. Unreal is Z-up and
the OBJs are Y-up, so the parser swaps axes and names columns for our
convention.

Three things the data shows, measured on `md_Advent_Security_03` (293 props):

* **Rotations are grid-aligned** — 288 of 293 yaws are exact multiples of 90°.
  Rotation is in UE angle units where 65536 = 360°.
* **Positions are grid-aligned** — 81% land on a half tile (127 on whole tile
  corners, the rest on tile centres).
* **Mirroring is used heavily** — 50 props have a negative `DrawScale3D`
  component. XCOM reuses one mesh for both handednesses. Preserve the sign or
  corner pieces face backwards, and remember a mirror **flips triangle
  winding**, so those instances need reversed cull order.

Budget ~30 s per parcel; the editor loads the map and everything it references.
201 parcels exist across 8 biomes, plus 122 plots and 454 PCPs.

## Audio

XCOM 2's sound is **not** in the `.upk` packages — the `SoundNodeWave` objects
there are stubs that "export" as zero bytes. The real audio is Wwise, in
`Content/WwiseAudio`, and it comes in two parts:

| | count | holds |
|---|---|---|
| loose `.wem` | 24,827 | almost entirely voice, one copy per language |
| **embedded in `.bnk`** | **286,704** | everything else — weapons, footsteps, environment, music |

Missing the second is easy and costs ~92% of the library. `xcom_audio.py`
parses the bank format directly (`DIDX` index + `DATA` blob), so no extra tool
is needed beyond vgmstream for the actual decode.

Names come from the `.txt` manifest beside each bank, so output is
`SF00_en_au_StunTarget_01.wav`, not `1001493842.wav`. 87% of loose files
recover a name; the rest land in `_unnamed/`.

Two traps, both of which silently lose most of the audio:
* The same ID exists **once per language**, so the language folder must be part
  of the output path or five locales overwrite each other. `-Language` filters
  early, before decoding.
* Bank IDs repeat across locales too, so a first-wins dedupe can drop English
  audio owned by a French bank. (It doesn't here — verified — but check if you
  change the ownership rule.)

## Known limitations

These are properties of UE3's exporters, not of these scripts:

* **Multi-material meshes collapse.** All material sections merge into a single
  `g UnrealEdObject` group, so a mesh with a trunk *and* a canopy gets whichever
  material `materials.csv` lists first. That file records every material a mesh
  references, so the ambiguous ones are identifiable.
* **No vertex colours.** Two consequences: road stripes lose their yellow (the
  texture is neutral grey, the colour was per-instance), and terrain materials
  parented to `Terrains_VP` are vertex-paint blends that cannot be reproduced —
  the tool takes the middle layer as a stand-in, so ground is approximate.
* **No skinned meshes.** The SDK's FBX exporter writes 0-byte files and there is
  no FBX SDK DLL in `Binaries`; PSK/OBJ/ASE export nothing. Character and weapon
  geometry is still available because XCOM ships a `StaticMesh` counterpart for
  each `SkeletalMesh` — but without bones or skin weights. Rigged meshes need
  [umodel](https://www.gildor.org/en/projects/umodel).
* **No normals.** Generated here with a 45° smoothing threshold.
* Textures in bulk mode are capped at 512 px with fast PNG encoding. The raw
  `.upk` is untouched, so anything worth more fidelity can be re-converted
  through `build_test_kit.ps1`.

---

# Helldivers 2 assets

Separate from everything above: different game, different engine, no shared
code.

## Start here: just run filediver

**Use [filediver](https://github.com/xypwn/filediver) directly against the game
install.** It already understands the current `bundles.NN.nxa` layout — its
`openDataDirSlim` reads `bundles.nxa`, parses the DSAA index and resolves files
straight out of the archives. It sees **76,912 extractable files in 13 s** and
writes glTF, PNG and ogg.

```powershell
$fd = "<path>\filediver.exe"
$gd = "E:\SteamLibrary\steamapps\common\Helldivers 2"

& $fd -g $gd -l -i "*"                                   # list everything
& $fd -g $gd -o out -i "content/fac_helldivers/**" -T unit
```

### Four things that will waste your afternoon

1. **A selector is mandatory.** With no `-i`/`-a`/`-m` it prints usage and
   exits 1. Pass `-i "*"` for everything.
2. **One type per invocation.** `-T unit,texture,material` silently matches
   **0 files**; `-T unit` matches 305. Loop over types instead.
3. **The `model` alias is broken.** `-T model` → 0, `-T unit` → 305. Use real
   type names: `unit`, `texture`, `material`, `shader_library`, `wwise_stream`,
   `wwise_bank`.
4. **Only 31.6% of names are known**; the rest are hashes. 24,958 assets carry
   real paths, 24,442 of them under `content/`.

Output is genuine: `arc_shotgun.unit.glb` is glTF 2.0 with 11,231 triangles,
26 materials, 46 embedded textures and a skin — it opens in Blender as-is.
`shader_library` extracts as raw DXBC with reflection names intact
(`exposure_dampening_up`, `atmosphere_enabled`), which is the rendering-study
material, not something you import.

## The local toolchain (a fallback, not the main path)

| script | job |
|---|---|
| `hd2_extract.ps1` | the driver — unpack, then index |
| `hd2_dsar.py` | DSAR/DSAA container readers |
| `hd2_unpack.py` | archives → loose bundle tree |
| `hd2_index.py` | index/query what the bundles contain |

These unpack the `.nxa` archives back into the pre-repack layout of loose
`<hash16>` / `.stream` / `.gpu_resources` files. **You do not need this to get
assets out** — filediver handles the packed form directly. It is worth keeping
for two narrower reasons: it makes the container format independently
documented and verifiable, and it gives any *other* Stingray tool that only
understands the old layout something to chew on.

Format spec, with the checks used to verify it:
[`study/helldivers2_formats.md`](../study/helldivers2_formats.md). Needs Python
3 and the `lz4` package (installed automatically).

```powershell
.\tools\hd2_extract.ps1 -List                # table of contents, extracts nothing
.\tools\hd2_extract.ps1 -Kind bundle         # 5.4 GB, start here
.\tools\hd2_extract.ps1 -Pilot 50            # time a sample first
.\tools\hd2_extract.ps1                      # everything, 127 GB
.\tools\hd2_extract.ps1 -IndexOnly
```

Resumable — a file already present at its expected size is skipped, so an
interrupted run costs nothing. Sliceable the same way as `xcom_bulk.ps1`:
`-Slice i -Of n`, disjoint by bundle, index once at the end with `-IndexOnly`.

## Mind the disk

The archives are 28 GB because chunks are **deduplicated** across bundles.
Unpacked, they expand to 127 GB:

| kind | files | size | holds |
|---|---|---|---|
| bundle | 3254 | 5.4 GB | metadata, materials, units |
| `.gpu_resources` | 2911 | 34.7 GB | vertex/index/texture payloads |
| `.stream` | 2650 | 86.7 GB | streamed audio, top mips |

`-Kind bundle` is enough to build `index.csv` and see what the game contains.
Pull payloads only for the bundles you want.

Dedup also means a single file is stitched from chunks scattered across many
archives — one 553 KB bundle uses 105 chunks from 14 of them. Extraction is
therefore random-access and cache-bound, not streaming.

## Querying

Bundles are named by hash, so `index.csv` records what each one *contains*,
read from its type table (one row per bundle, one column per resource type).

```powershell
py -3 tools\hd2_index.py --library hd2_extracted\data --query "unit"
py -3 tools\hd2_index.py --library hd2_extracted\data --query "texture>200"
```

Type names are recovered by hashing candidates with murmur64a and matching, the
same scheme Stingray uses everywhere. Unmatched hashes print as raw hex rather
than being guessed at — `0x46bc82aae9ae0565` is common and still unidentified.

A bundle is a mixed bag: a unit bundle carries its meshes, materials, textures,
physics and bones together. So the useful question is "which bundles hold
units", not "where is mesh X".

## Known limitations

* **No asset conversion.** By design — this is the bridge to filediver, not a
  replacement for it.
* **`data/game/` is not readable.** The 58 `generated_*.dl_bin` files are loose
  on disk and named enticingly (damage, destruction, weather, sky, planet data)
  but their contents are encrypted or compressed with an unidentified scheme.
* **The 80-byte per-file bundle entry is not decoded.** `index.csv` needs only
  the type table.
* **The layout will change again.** The DSAR invariants in the format doc are
  the cheapest way to check whether it has.
