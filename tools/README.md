> **Helldivers 2** has its own toolchain in this folder — `hd2_extract.ps1`,
> `hd2_unpack.py`, `hd2_dsar.py`, `hd2_index.py`. Different game, different
> engine (Stingray, not UE3), no shared code. See the end of this file.
>
> **UNIGINE 2** has a small one too — `unigine_extract.ps1`,
> `unigine_texture.py`. Also unrelated to the above. See the end of this file.

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

### Use the patched binary in `workbench/hd2/`

Stock filediver **cannot extract the unit set at all**. It panics in
`geometry.LoadGLTF` on units whose `materialIndices` slice is empty — engine
placeholders such as `fallback_resources/missing_unit`. A Go panic kills the
process rather than skipping the file, so one bad asset costs all 7,487 units;
it died at file 238 every single time, deterministically.

The cause is a guard on the wrong slice. The comment above it says the author
was defending against exactly this, but checks `header.Materials` while the
thing that blows up is `materialIndices[0]`:

```go
if len(materialIndices) > 0 && int(group.MaterialIdx) < len(header.Materials) {
```

`workbench/hd2/` holds the built binary, `ffmpeg.exe` (needed for audio), and
`materialIndices-bounds-check.patch`. To rebuild: clone the repo, apply the
patch, `go build -o filediver-patched.exe ./cmd/filediver-cli`. Go 1.21 will
auto-fetch the 1.25 toolchain the module wants; no cgo, ~90 s. Worth checking
whether this has landed upstream before rebuilding.

### Five things that will waste your afternoon

1. **A selector is mandatory.** With no `-i`/`-a`/`-m` it prints usage and
   exits 1. Pass `-i "*"` for everything.
2. **One type per invocation.** `-T unit,texture,material` silently matches
   **0 files**; `-T unit` matches 305. Loop over types instead.
3. **The `model` alias is broken.** `-T model` → 0, `-T unit` → 305. Use real
   type names: `unit`, `texture`, `material`, `shader_library`, `wwise_stream`,
   `wwise_bank`.
4. **`-i` and `-x` take one PATTERN — but it can be an alternation.** A second
   `-i`/`-x` overrides the first rather than unioning, so `-x A -x B` excludes
   only B. `-x "{A,B,C}"` excludes all three. This is what makes a skip list or
   a resume-only-the-missing run possible at all; batch it under ~28k chars to
   stay inside the Windows command-line limit.
5. **Only 31.6% of names are known**; the rest are hashes. 24,958 assets carry
   real paths, 24,442 of them under `content/`. Hash forms work as globs too
   (`-i "0x0a7b*"`), which is how you shard a run.

### Resuming

filediver never skips existing output, so re-running redoes everything. To
resume after an interruption, diff the extracted `.glb` files against
`-l -i "*" -T unit` and feed the difference back as batched brace alternations.
Recovering 1,623 missing models that way took ~30 min instead of the ~2 h a
full re-run would have cost.

Validate before trusting a file count: check each `.glb` for the `glTF` magic
and that the header's declared length equals the actual file size.

Output is genuine: `arc_shotgun.unit.glb` is glTF 2.0 with 11,231 triangles,
26 materials, 46 embedded textures and a skin — it opens in Blender as-is.
`shader_library` extracts as raw DXBC with reflection names intact
(`exposure_dampening_up`, `atmosphere_enabled`), which is the rendering-study
material, not something you import.

### What a full extraction yields

Run 2026-08-09/10 against the then-current build, into `hd2_extracted/`:

| directory | files | size | format |
|---|---|---|---|
| `models/` | 7,487 | 55.7 GB | `.glb`, textures embedded |
| `audio/` | 102,260 | 49.0 GB | `.wav` (pass `--audio-format ogg` for ~10x less) |
| `textures/` | 16,434 | 11.8 GB | `.png` |
| `materials/` | 5,587 | 5.6 GB | |
| `shaders/` | 1,643 | 0.03 GB | DXBC |
| **total** | **133,496** | **122.5 GB** | |

All 7,487 models verified structurally valid. 85 contain zero meshes — the
placeholder units that used to trigger the panic; they extract cleanly now and
are simply empty.

Budget most of a night: the model pass alone is ~2 h at ~58 units/min, and
audio is far larger than it looks because it defaults to uncompressed WAV.

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

---

# UNIGINE 2 textures

Separate again: an engine SDK rather than a shipped game, and read for the
rendering studies rather than for assets.

| script | job |
|---|---|
| `unigine_extract.ps1` | the driver — decode the cloud and water texture sets |
| `unigine_texture.py` | the `.texture` (`tx10`) container reader and BC decoder |

```powershell
.\tools\unigine_extract.ps1 -Info              # header table, decodes nothing
.\tools\unigine_extract.ps1                    # clouds + water -> unigine_extracted/
.\tools\unigine_extract.ps1 -Set clouds -Slices
```

Needs Python 3 with `numpy`, `Pillow` and `lz4` — the same set the XCOM and
Helldivers tools already want. Pass `-SdkRoot` if the SDK Browser put the SDK
somewhere other than
`%LOCALAPPDATA%\unigine\browser\sdks\community_windows_2.17.0.1_bin`; the
authoritative list is in `%APPDATA%\unigine\browser.json` under `sdk.installed`.

## Why this exists

UNIGINE ships its whole core library **unpacked and readable** — shaders as
source, textures as loose files — which makes it the best available reference
for two systems this project cares about. The shaders need no tool. The
textures do: `.texture` is UNIGINE's own container and nothing third-party
opens it.

`study/unigine_clouds.md` is the write-up that came out of it.

## Reading the output

24 files decode from the two sets, in five formats:

| set | files | formats seen |
|---|---|---|
| `clouds` | 19 | RGBA8, DXT1, DXT5, ATI1 (BC4) — 2D and 3D |
| `water` | 5 | R8, ATI1 (BC4), ATI2 (BC5) |

2D textures write `<name>.png` plus one image per channel. Volume textures
write a contact sheet of all Z slices (`<name>_sheet.png`), plus per-channel
sheets, plus every slice individually under `--slices`. Per-channel output is
the point — `cloud_noise` is four independent noise octaves packed into RGBA,
and the coverage textures pack coverage, storm mask and cloud-type height into
R, G and B.

The tool prints per-channel min/mean/max as it goes, which is the fastest way
to see what a channel actually holds. It is also the correctness check: BC5
normal maps must land on a mean of ~127 in both channels, and the storm mask
(G) is near-zero for `clouds_coverage_cumulus` but mean 174 for
`clouds_coverage_nimbostratus`.

### Two traps when looking at the output

**Do not judge a sparse texture from a downsampled contact sheet.**
`caustics.texture` is thin bright filaments on black (mean 9.4/255, median 0).
Resampling a 512 px slice down to a thumbnail averages those 1–2 px lines away
and it reads as a solid black square. View volume slices at 1:1, or boost them,
before concluding a decode is broken. The cheap check that it is *not* broken:
box-downsample mip 0 and correlate it against mip 1, which is a separate LZ4
block — they agree to 0.9999 here.

It is also genuinely darker than the caustics tiles you find on texture sites,
and deliberately so. `raytrace.frag:94` multiplies it by `caustic_fade` (≤ 1)
and `caustic_brightness` (**default 1**, max 2), then *adds* it to ground that
is already lit. It is a highlight overlay, not an illumination map.

**A volume's Z axis is not always depth.** For `caustics.texture` the 64 slices
are **animation frames** — the shader samples
`float3(uv.xy, water_time * s_caustics_animation_speed)`, default speed 0.3, and
`caustic_uv_transform` of `[0.05 0.05 0 0]` tiles it every 20 world units. For
`cloud_noise` and the `*_shape` volumes, Z *is* the third spatial axis. The
contact sheet looks the same either way.

## The container

48-byte header — `'tx10'`, an is-3D flag, an `Image::FORMAT_*` enum, w/h/d, mip
count — then per mip a `u64` size followed by either raw data or a **raw LZ4
block** (no frame header; the uncompressed size is known from the format and
dimensions). Compression is opportunistic per mip: `foam_d.texture` stores BC4
byte for byte, while a mostly-empty BC4 shadow map packs 131072 → 17624.

Volume textures are a stack of independently block-compressed **2D** slices,
not 3D blocks.

Only enum values 3 (RGBA8), 24 (ATI1) and 25 (ATI2) are confirmed byte-exact
against the SDK's own files; the rest follow the enum's declaration order.
A wrong guess fails a size assertion rather than silently decoding garbage,
which is why `--info` across a whole SDK is a cheap way to validate the table.

## Licensing — read, do not ship

UNIGINE's art is licensed for use in UNIGINE projects. `unigine_extracted/` is
gitignored and nothing from it is committed or shipped. The purpose is to read
what each channel holds — which the shaders in the same SDK document precisely —
and then generate our own equivalents, the same rule this repo already applies
to XCOM's `MovementBorder_Line` (see `study/README.md`).

## Known limitations

* **Only 8-bit formats decode.** The 16- and 32-bit entries in `FORMATS` are
  sized correctly, so headers and mip offsets are right, but `decode_slice`
  rejects them. Nothing in the cloud or water sets needs them.
* **No BC6H/BC7.** Not seen in either set; the enum values above 25 are not
  mapped at all.
* **Mip 0 only by default.** `--mip N` selects another; there is no
  whole-chain export.
* **Volume slices are decoded eagerly.** A 256³ BC4 volume is a million blocks
  — vectorised, so about a second, but it holds the whole decoded volume in
  memory (~16 MB at 8-bit) before writing.
