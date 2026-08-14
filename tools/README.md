# Code tools

One folder per toolchain, one script at the root about this repository's own
source. The layout:

| entry | job |
|---|---|
| `tidy.sh` | clang-tidy over the tree — the mechanical half of CLAUDE.md's performance rules |
| [`xcb/`](xcb/) | the project's cockpit (Go TUI) — build targets, run suites, per-project build timing and history; see the header comment in `xcb/main.go` |
| [`xcom2/`](xcom2/) | the XCOM 2 SDK asset toolchain — extract, convert, materials, audio, FX, parcels, publish |
| [`hd2/`](hd2/) | Helldivers 2 (Stingray) extraction |
| [`r6/`](r6/) | Rainbow Six: Siege (AnvilNext 2) extraction |
| [`ruse/`](ruse/) | R.U.S.E. (IRISZOOM) readers — the only Python 2 in the tree |
| [`wic/`](wic/) | World in Conflict (MassTech) — `RYS` archives, headerless textures, Python 2.3 gameplay bytecode |
| [`ba/`](ba/) | Broken Arrow (Unity 2022 / IL2CPP) — addressable bundles, encrypted FMOD banks, Granite virtual textures |
| [`unigine/`](unigine/) | UNIGINE 2 texture decoding |
| [`asset_browser/`](asset_browser/) | search, preview and channel-split across *every* extracted library |
| [`fonts/`](fonts/) | icon header/CSS generation and MSDF atlas baking from the licensed font packs |

The per-game toolchains share no code with each other — different games,
different engines. Each is documented in its own section below.

```bash
./tools/tidy.sh                          # everything; "clean" when it finds nothing
./tools/tidy.sh src/game/los/RayCaster.cpp
./tools/tidy.sh --fix                    # apply what clang-tidy is confident about
```

Nothing to install: clang-tidy ships with Visual Studio 2022. It needs no
compile database either — the simulation is portable C++20, so
`-std=c++20 -Isrc` is the whole flag set, and raylib's include path is picked up
from `builds/_cmake-win/_deps` when the project has been configured once.

**What it cannot tell you.** clang-tidy sees *local* patterns — a needless copy,
a vector that should have reserved. The expensive mistakes here were contextual:
correct, idiomatic code that was ruinous only because a caller four levels up sat
inside a per-ray-step loop. Nothing local sees that.

**For where the time actually goes, use Tracy** — build with `-DXC_TRACY=ON` and
see the profiling section in `CLAUDE.md`. It gives CPU and OpenGL GPU zones on
one aligned timeline, which is the only way to tell a GPU-bound frame from a
CPU-bound one.

---

> **Helldivers 2** has its own toolchain in [`hd2/`](hd2/) — `hd2_extract.ps1`,
> `hd2_unpack.py`, `hd2_dsar.py`, `hd2_index.py`. Different game, different
> engine (Stingray, not UE3), no shared code. See the end of this file.
>
> **UNIGINE 2** has a small one too, in [`unigine/`](unigine/) —
> `unigine_extract.ps1`, `unigine_texture.py`. Also unrelated to the above. See
> the end of this file.
>
> **Rainbow Six: Siege** has one as well, in [`r6/`](r6/) — `r6_extract.ps1`,
> `r6_forge.py`, `r6_index.py`. AnvilNext 2, unrelated to any of the above. See
> the end of this file, and `study/games/shooters/rainbow_six_formats.md` for
> the container format.
>
> **R.U.S.E. / IRISZOOM** has three small readers in [`ruse/`](ruse/) —
> `ruse_edat.py`, `ruse_ndf.py`, `ruse_python.py`. Eugen Systems' own engine,
> unrelated to any of the above, and **the only Python 2 scripts in the tree**.
> See the end of this file, and `study/games/strategy/ruse.md` for what they
> were written to find out.
>
> **[`asset_browser/`](asset_browser/)** sits across all of them — search every
> extracted library, preview meshes, split textures into channels. See below.

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
| `xcom_anim.ps1` / `.py` | skinned meshes + animations (needs umodel), notify events |
| `xcom_fx.ps1` / `.py` | particle systems; also resolves base-Material textures |
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

Current contents: **1760 packages, 10107 meshes, 976 packages with geometry,
290,657 WAVs, 1005 maps with 114,642 prop placements, 5273 animation clips**.
**8586 meshes (85%)** resolve to a diffuse and have `mtllib`/`usemtl` patched
into the OBJ — see "Materials: how the 85% is reached" below for the four
passes that took it there. The remainder is engine defaults, dev scratch and
master materials; for environment art the rate is ~97%.

## Using the assets in the prototype

Start with **`xcom_parcel_render.py`**. It is a throwaway software rasteriser,
but its `place()` and `get_tex()` functions are the two things worth porting:
the exact transform order for placing a prop, and the correct way to find its
textures. Its header documents both, including the mirroring and alpha-cutout
traps. Run it on any extracted parcel to see the result:

```powershell
py -3 tools\xcom2\xcom_parcel_render.py md_Forest_01  # -> workbench\parcel_<name>.png
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
.\tools\xcom2\xcom_bulk.ps1 -AllContent                    # models + textures
.\tools\xcom2\xcom_materials.ps1 -WriteMtl -PatchObj       # pair them up
.\tools\xcom2\xcom_audio.ps1 -VgmStream <path>             # loose .wem (voice)
.\tools\xcom2\xcom_audio.ps1 -VgmStream <path> -Banks      # .bnk contents (all SFX)
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
py -3 tools\xcom2\xcom_index.py --library xcom_extracted\models --query "high 1x1"
py -3 tools\xcom2\xcom_index.py --library xcom_extracted\models --query "ladder"
```

Of 10107 meshes: 415 high cover, 711 low, 1153 deco; the rest carry no cover
tag — usually structural or decorative. Footprints skew heavily to `1x1`, then
`x2`, `x1`, `1x2`, `2x2`. Ladders appear at 192/256/384/448/512 uu, all whole
z-cell multiples (64uu), so they drop straight onto the lattice.

## Parcels

```powershell
.\tools\xcom2\xcom_parcel.ps1 -List
.\tools\xcom2\xcom_parcel.ps1 -Parcel md_Advent_Security_03
.\tools\xcom2\xcom_parcel.ps1 -Parcel lg_Museum_01 -Summary
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

## Animations and skinned meshes

The SDK **cannot** export these. Its FBX exporter writes 0-byte files (there is
no FBX SDK DLL in `Binaries`) and PSK/PSA/ASE export nothing, so `SkeletalMesh`
and `AnimSequence` are unreachable through BatchExport. umodel reads the `.upk`
directly and does not care.

```powershell
.\tools\xcom2\xcom_anim.ps1 -UModel C:\tools\umodel\umodel_64.exe
.\tools\xcom2\xcom_anim.ps1 -Notifies                     # second pass, no umodel needed
```

umodel is third party and NOT vendored: <https://www.gildor.org/en/projects/umodel>.
The site blocks hotlinking, so a programmatic fetch needs a browser `Referer`
header.

**Three things that will waste hours if you do not know them:**

1. **Use `umodel_64.exe`.** The 32-bit build in the same zip crashes on these
   packages with exit 255 and no output.
2. **`-nostat` is not optional.** umodel 1590 cannot deserialise XCOM's
   StaticMesh variant - it dies with `RawArray item size mismatch` and takes
   the whole package with it, animations included. Without it **128 of 473
   packages fail**; with it, 2. Static geometry is already covered by
   `xcom_bulk.ps1`, so nothing is lost. This is the default here.
3. **`.psa` carries no notify events.** Clip names and lengths come from the
   `.psa` ANIMINFO chunk (complete and authoritative); footstep, weapon-fire
   and IK timings exist only in the SDK, via the separate `-Notifies` pass.

Output: `xcom_extracted/anim/<Package>/{AnimSet/*.psa, SkeletalMesh/*.psk}`,
plus `anim_clips.csv` (5273 clips, 758 minutes) and `anim_notifies_events.csv`
(13,469 events - 6141 AkEvent sound triggers, 2785 footsteps, 648 IK blends).
AkEvent payloads name Wwise events that cross-reference into the WAVs.

Notify coverage is ~73% of clips, not 100%: BatchExport names each T3D after
the object, and most AnimSequences are anonymous (`AnimSequence_0`, ...) with
numbering restarting per AnimSet, so files overwrite each other. There is no
BatchExport option to disambiguate.

## Particle systems

```powershell
.\tools\xcom2\xcom_fx.ps1 -PackageList workbench\fx_packages.txt -Slice 0 -Of 4
```

`batchexport <pkg> ParticleSystem T3D` dumps every emitter, LOD level and
module with its baked distribution values. Yields `fx_systems.csv` (2844
systems) and `fx_systems_emitters.csv` (17,293 emitters: type, material, spawn
rate, lifetime, start size/velocity, module chain).

**Filter the package list first.** Loading a package costs ~80s and most hold
no particles, so a full 1759-package sweep is ~5 hours. Particles live almost
entirely in `FX_*`. Grepping each `.upk` name table for `ParticleSystem` looks
tempting and is *wrong* - it matches packages that merely reference one
(`AbstractSculptures` matched and exports zero).

Values are read from each property's baked `LookupTable`, not by chasing the
Distribution object it names - those inner objects reuse names heavily (43
copies of one name in a single file), so a flat name map collides.

## Materials: how the 85% is reached

`materials.csv` resolves 8586 of 10107 meshes to a diffuse. It took four
passes, and each is a trap worth knowing:

| pass | mechanism | running total |
|---|---|---|
| 1 | `pkginfo -all` DependsMap → MIC → `TextureParameterValues` | 6570 (65%) |
| 2 | **case-insensitive** MIC lookup | 7369 (73%) |
| 3 | base `Material` expression graphs | 8184 (81%) |
| 4 | MIC → `Parent=` chain into the base graph | 8586 (85%) |

* **Case sensitivity is the single biggest trap here.** XCOM's own references
  disagree with its file names - `adventacunits.Materials.adventacunits` points
  at a package stored as `AdventACUnits`. Exact-case matching silently loses
  ~800 meshes, and made 197 packages look absent from the SDK elsewhere.
  Lowercase both sides of every name comparison.
* **A MIC that overrides nothing declares no textures at all** - it inherits
  them from `Parent=`. Following that chain into the base Material's graph is
  worth another 400 meshes.
* **Base `Material` objects are not MICs.** Their textures live in
  `MaterialExpressionTextureSample` nodes, exposed only by
  `batchexport <pkg> Material T3D` (`xcom_fx.py basemats`, then
  `buildmats` → `basematerials.csv`). Expensive: ~280s for a texture-heavy
  package.
* Graph textures carry no parameter names, so role is inferred from the suffix
  (`_NRM`, `_MSK`, `_OPC`). Imperfect, but better than untextured.

The remaining 539 are `EngineMaterials`, `jeremy_dev` and
`Strat_Ship_Int_MasterMaterials` - engine defaults, dev scratch and master
materials, mostly without a conventional diffuse. A further 982 meshes
reference no material at all (collision proxies, LODs). 85% is close to the
automated ceiling.

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
[`study/games/shooters/helldivers2/helldivers2_formats.md`](../study/games/shooters/helldivers2/helldivers2_formats.md). Needs Python
3 and the `lz4` package (installed automatically).

```powershell
.\tools\hd2\hd2_extract.ps1 -List                # table of contents, extracts nothing
.\tools\hd2\hd2_extract.ps1 -Kind bundle         # 5.4 GB, start here
.\tools\hd2\hd2_extract.ps1 -Pilot 50            # time a sample first
.\tools\hd2\hd2_extract.ps1                      # everything, 127 GB
.\tools\hd2\hd2_extract.ps1 -IndexOnly
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
py -3 tools\hd2\hd2_index.py --library hd2_extracted\data --query "unit"
py -3 tools\hd2\hd2_index.py --library hd2_extracted\data --query "texture>200"
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
.\tools\unigine\unigine_extract.ps1 -Info              # header table, decodes nothing
.\tools\unigine\unigine_extract.ps1                    # clouds + water -> unigine_extracted/
.\tools\unigine\unigine_extract.ps1 -Set clouds -Slices
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

`study/games/rendering/unigine_clouds.md` is the write-up that came out of it.

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

---

# Asset browser — across every library at once

| script | job |
|---|---|
| `asset_browser.py` | catalogue, search, mesh render (GPU or software), animation playback, texture channel split |
| `asset_browser_ui.py` | the tkinter front end; import it from the above, not directly |

```powershell
py -3 toolssset_browser.py                       # the GUI
py -3 toolssset_browser.py --list                # per-library counts
py -3 toolssset_browser.py --find rifle --kind mesh
py -3 toolssset_browser.py --render <mesh> --out preview.png --mode normals
py -3 toolssset_browser.py --sheet siege:mesh --out sheet.png -n 24
```

Needs only numpy and Pillow; tkinter ships with Python. It reads XCOM 2,
Helldivers 2, Siege and UNIGINE from wherever their extractors put them —
Siege's path comes from `R6_LIBRARY`, default `D:
6_extracted`. Measured:
**494,611 assets catalogued in ~17 s**, cached to `workbench/asset_catalog.tsv`.

**Meshes render without a GPU** - a numpy rasteriser, same approach as
`xcom_parcel_render.py`, orthographic so two props are directly comparable.
Flat, normal-shaded and textured modes. `.obj` (XCOM, Siege) and `.glb`
(Helldivers) both load.

**Orbit camera**: drag to orbit, right-drag or shift-drag to pan, wheel to zoom,
double-click or `R` to reset.

**Animation**: select any rigged `.glb` and a clip list appears beside the
preview. Clips come from two places - the ones inside the file, and the
standalone `.anim` library `mercs_anim.py` wrote, bound to the skeleton **by
joint name**. That second source is what matters: every human in Mercenaries
shares one skeleton, so all 88 of them play the same 1,639 clips without the
export duplicating ~10 GB of identical curves. The three `_anim.glb` under
`gltf/animated/` carry their clips inline for Blender's sake; in the browser
they look the same as the rest.

A clip is offered when it and the skeleton overlap by 75% of the **smaller**
joint set. Measuring against the clip instead rejects every 25-joint soldier
against a 36-joint merc animation at 69%, which is exactly the case the feature
exists for. The rule still keeps a 4-joint flag clip away from a character: an
Apache is offered **0** clips, a flag its 5, and the artillery its single
`nk_veh_type66artillery_fire`. Click a clip to play it, `repeat` to loop, `bind pose` to
stop. The list has a filter box because a merc carries 1,639 clips, all of them
sharing the one human skeleton. Skinning runs on the CPU (3 ms for a 36-joint
character) and the posed vertices go to whichever renderer is live.

**Install `moderngl` if you can.** It is an optional import and it changes the
viewer's character completely. The numpy triangle rasteriser is a Python loop
over *faces*, so its cost tracks triangle count and dropping the resolution buys
almost nothing - 202 ms for a 3,216-triangle character, 1,094 ms for an
18,009-triangle mesh, at any size. That is why every moving frame used to swap
to scattered samples, and why dragging lost the texture and the surface detail.
The GPU path draws the same picture - same projection constants, same shading,
**0.97 silhouette IoU** against the software render across angles and zooms - in
**3.9 ms**, so one renderer now serves the still frame, the drag and animation
playback alike.

| | numpy | GPU |
|---|---|---|
| 3,216-tri character, shaded | 202 ms (5 fps) | **3.9 ms (257 fps)** |
| 2,556-tri Apache, shaded | 186 ms (5 fps) | **3.7 ms (269 fps)** |
| skinned + textured playback | splat only | **5.6 ms (177 fps)** |

Without moderngl the old behaviour is intact: splat while moving, shaded on
release. The preview says which renderer drew it.

| mesh | drag (points) | release (shaded) |
|---|---|---|
| 28,799 verts / 18,009 tris | **10.6 ms (94 fps)** | 1,094 ms |
| 3,866 verts / 1,934 tris | **6.7 ms (149 fps)** | 151 ms |

Zoom coalesces the same way - the wheel redraws coarse immediately and schedules
one sharp redraw 180 ms later, so a fast scroll costs one full render rather than
one per notch.

**Textures split into channels, which is the point.** These engines pack
unrelated data per channel, so the composite is often meaningless: XCOM's MSK
carries alpha cutout in BLUE only, and Siege's specular map holds gloss,
metalness and cavity. Siege normal maps come out with **B flat at zero and R/G
averaging 127** — two-channel BC5, where Z belongs to the shader — so the viewer
detects those and offers a `Z` view that rebuilds the third component.

## Which maps a mesh has

The `material` filter is the map set itself, not a yes/no, so "which meshes have
a spec map" is a direct query. Measured over XCOM's 10,107 meshes:

| map set | meshes | | map set | meshes |
|---|---|---|---|---|
| `dif+nrm+msk` | 5,415 | | `dif+msk` | 378 |
| `dif+nrm+msk+spc` | 1,222 | | `dif+nrm+spc` | 60 |
| `dif` | 840 | | `dif+msk+spc` | 12 |
| `dif+nrm` | 659 | | `none` | 1,521 |

Helldivers meshes read `embedded` (textures live inside the `.glb`) and every
Siege mesh reads `none`. A mesh opens **textured** when it has a diffuse and flat
when it does not, so an asset with materials no longer opens grey next to a row of
its own thumbnails.

**XCOM ships no roughness, emissive or AO maps.** That data is packed into the MSK
channels and the diffuse alpha, and only ~148 `_SPC` files exist in the whole
library. Measured over 40 files of each kind:

| map | R | G | B | A |
|---|---|---|---|---|
| `_MSK` | mean 69, varies | mean 202 | mean 24, **flat in 60%** - alpha cutout, foliage only | mean 72, never flat |
| `_DIF` | colour | colour | colour | mean 126, **never flat** - packed data, not opacity |

The texture view prints that note under any map whose packing is known, so a
false colour is not mistaken for the surface's colour.

## What it cannot pair

Only XCOM records mesh → material links, via `materials.csv` from
`xcom_materials.py`, so only XCOM meshes show their diffuse/normal/mask beside
them and only XCOM supports `--mode textured`. **Siege ships no asset names and
no material links at all**, so its meshes and textures are both browsable but not
pairable; Helldivers embeds textures inside each `.glb`, which the minimal glTF
reader here ignores (it takes geometry only — no scene graph, no node transforms,
no skinning).

Two traps worth knowing, both found the hard way:

1. **glTF vertex attributes are usually interleaved.** Reading them as
   contiguous walks into the neighbouring attribute and yields geometry that
   looks valid apart from a few absurd coordinates — it does not throw. Honour
   `byteStride`.
2. **Do not shell-glob these libraries.** `ls D:
6_extracted\mesh\*\*.obj`
   expands 172,743 paths and will outlast your patience; the catalogue walks the
   tree once and caches.

---

# Rainbow Six: Siege assets

Separate again: AnvilNext 2, no shared code with anything above.

| script | job |
|---|---|
| `r6_extract.ps1` | the driver — sweep archives into loose assets |
| `r6_forge.py` | the `.forge` (scimitar) container reader |
| `r6_index.py` | index/query what the archives contain |

## You have to build the extractor first

Unlike Helldivers, there is no working tool to point at. **All three public
toolkits fail on the current build** — [RainbowForge](https://github.com/parzivail/RainbowForge)
(archived March 2024), [RainbowForge3](https://github.com/hengtek/RainbowForge3),
[Prism_V2](https://github.com/kazonix/Prism_V2). RainbowForge3 is the closest
starting point; `workbench/r6/siege-v34-and-typing.patch` is the diff against it.

```powershell
# download RainbowForge3, apply the patch, build (needs the .NET 6 SDK)
cd workbench\r6\RainbowForge3\RainbowForge3-master
dotnet build DumpTool\DumpTool.csproj -c Release
```

Oodle is needed and the game does not ship it loose — it is statically linked
into `RainbowSix.exe`. RainbowForge3 bundles a copy under `Testing files\`; any
UE4/UE5 install has `oo2core_9_win64.dll`, which decodes `oo2core_8` streams if
you rename it. It must sit beside `DumpTool.dll` **and** be named in
`OODLE2_8_PATH` — the tool P/Invokes it by name and separately checks the
variable points at a real file. `r6_extract.ps1` sets the variable for you.

**What the patch fixes**, none of which any public tool does:

1. **The v34 header layout.** v34 inserts a `u32` at `0x1E`, shifting every field
   below it by four bytes. Read with the v33 layout it does not fail loudly — it
   yields a plausible `numEntries`, a garbage table pointer, and dies later on a
   wild seek. This silently blocked **215 of 307 archives**, including every
   texture and sound archive.
2. **Asset typing.** The FAT's metadata table is all zeros from v33 on, so
   RainbowForge3 forces every asset to `AssetType.Texture` — correct only inside
   a textures archive, corrupting everything else. The real type is in the
   `FileMetaData` block at the head of the payload.
3. **The encrypted v34 FAT**, bypassed by scanning for the container magic.
4. **Both sound paths** — see below.

## Sweeping

```powershell
.\tools\r6\r6_extract.ps1 -List                  # archive table by kind, extracts nothing
.\tools\r6\r6_extract.ps1 -Kind mesh             # 2.8 GB of archives, start here
.\tools\r6\r6_extract.ps1 -Pilot 1 -Kind texture # time one archive before committing
.\tools\r6\r6_extract.ps1                        # everything
```

Archives are classified by filename, which is reliable because Ubisoft packs by
type — a `*_mesh` archive holds meshes and nothing else. That is what makes
`-Kind` worth having:

| kind | archives | packed |
|---|---|---|
| `texture` | 65 | 28.7 GB |
| `guitexture` | 21 | 2.8 GB |
| `mesh` | 52 | 2.8 GB |
| `gidata` | 57 | 2.3 GB |
| `soundmedia` | 14 | 2.2 GB |
| `meshshape` | 38 | 2.2 GB |
| `soundbank` | 13 | 0.9 GB |
| `world` | 34 | 0.4 GB |

Resumable — an archive whose output directory carries a `.done` marker is
skipped, so an interruption costs nothing. Sliceable the same way as
`xcom_bulk.ps1`: `-Slice i -Of n` across n processes, disjoint by archive.

Measured on a full sweep: **940,110 files, 139.4 GB**, about 100 minutes across
four parallel workers (`-Slice 0..3 -Of 4`). 43.7 GB of archives expands a bit
over 3x, and textures are 70% of the result — 97.7 GB on their own. **Put the
output somewhere with 150 GB free**; the single 11.4 GB `merged_bnk_textures3`
will dominate one worker's slice however you split the rest.

| kind | files | size | | kind | files | size |
|---|---|---|---|---|---|---|
| `texture` | 238,350 | 97.7 GB | | `meshshape` | 268,265 | 3.4 GB |
| `mesh` | 172,743 | 21.5 GB | | `gidata` | 287 | 2.1 GB |
| `guitexture` | 28,719 | 6.2 GB | | `soundmedia` | 46,063 | 2.1 GB |
| `other` | 129,315 | 3.9 GB | | `world` | 1,996 | 1.6 GB |
| | | | | `soundbank` | 54,372 | 0.9 GB |

Against 941,094 assets catalogued by `r6_index.py` that is a **0.13% loss**.

## Querying

```powershell
py -3 tools\r6\r6_index.py --list                                  # instant, headers only
py -3 tools\r6\r6_index.py --build --out r6_extracted\index.csv    # ~1 h, see below
py -3 tools\r6\r6_index.py --library r6_extracted --query "mesh"
py -3 tools\r6\r6_index.py --library r6_extracted --query "pvp04_clubhouse"
```

`--build` is slow for a reason: the v34 allocation table is encrypted, so entries
are found by scanning the archive, and typing each asset means inflating the
first chunk of its payload. Use `--slice/--of` to spread it over processes.

**Query by property, because there are no names** (below). The archive name is
the most useful axis — it encodes map, season or content drop
(`pvp04_clubhouse`, `set01`, `mtx`, `evn12_rengoku`), which is most of the
context a filename would have carried. `--list` alone answers "where is the bulk
of the content" without extracting anything.

## Five things that will waste your afternoon

1. **Names do not exist.** Not "encrypted and awkward" — the FAT's name field is
   zeroed in all 307 archives, and the in-payload blob resists every reconstruction
   of the published key. Assets come out as `id1204224_typeDiffuse.dds`. The full
   negative result is in `study/games/shooters/rainbow_six_formats.md` §6 so it is not re-derived.
2. **There are no particle definitions and no FX archive.** The `FX` /
   `EffectData` class IDs from 2021 appear nowhere in this build. FX *textures*
   extract fine but land in the catch-all `Misc` category and, without names, have
   to be found by eye. §7.
3. **Sound is stored two incompatible ways.** `*_soundbank` are ordinary
   containers; `*_soundmedia` hold bare `.wem` end to end with **zero** container
   magics, so they enumerate as empty archives unless scanned for `RIFF`/`WAVE`.
   That is 46,063 files — most of the game's audio — invisible to every other
   tool.
4. **`.wem` still needs vgmstream.** Not vendored, same as the XCOM toolchain;
   extraction gives you `.wem`, conversion to WAV/OGG needs
   [vgmstream](https://github.com/vgmstream/vgmstream/releases).
5. **Mind the disk.** 43.7 GB packed. Textures are 28.7 GB of that before
   decompression, and audio inflates hard — Helldivers went 28 GB → 122 GB for
   the same reason. Run `-Kind` passes, not the lot, unless you have the room.

## Known limitations

* **No names, no particle definitions** (above).
* **World/entity data is opaque.** The 40 map archives (`pvp01_house` …
  `pvp29_privatecasino`, one per Siege map) hold ~80 assets each, nearly all of
  the undecoded type `0x569859AA`. These are the analogue of XCOM's parcels and
  decoding them is the highest-value unfinished work here.
* **Soundbank extraction is ~96%**; soundmedia is 100%.
* **No skinning.** Meshes export as OBJ with positions, normals, UVs and vertex
  colours. Siege's rigged characters need more than the OBJ writer emits.
* **Textures are DDS**, not PNG — DXT1/DXT5/BC5U with mips, which is the right
  form for a renderer but wants a converter for eyeballing.
* **Most type IDs are unknown.** RainbowForge's table is from 2021 and the two
  commonest types in this build are not in it; `r6_index.py` records unknown IDs
  as hex rather than guessing.

---

# R.U.S.E. — reading Eugen's object database

Separate again, and different in kind from everything above: this toolchain is
not for **assets**, it is for **structure**. R.U.S.E. ships its render graph, its
shader database, its terrain streaming policy, its 214 game-design constants and
its AI as *data*, and the point of these readers is to read them rather than to
convert anything.

| script | job |
|---|---|
| `ruse_edat.py` | the `edat` archive — header, prefix-tree dictionary, entry extraction |
| `ruse_ndf.py` | `EUG0`/`CNDF` object files — section directory, string tables, object walk |
| `ruse_python.py` | `.xyz` — zlib + `marshal`, then bytecode-walk module-level assignments |

```bash
python tools/ruse/ruse_edat.py   ".../R.U.S.E/Data/PC/190852/ZZ_GladNotPatchableWin.dat"
python tools/ruse/ruse_edat.py   ".../R.U.S.E/Maps/PC/DataMapM04_Cotentin_v09.dat"
python tools/ruse/ruse_edat.py   ".../Wargame Red Dragon/Data/WarGame/PC/510064564/NDF_Win.dat"
python tools/ruse/ruse_python.py eugen.ipk defines/front/bluff.xyz
```

Both archive versions are exercised: R.U.S.E. is `edat` v1, Wargame: Red Dragon
is v2 (2,640 files, all parsing).

**Python 2.7, and it is not negotiable.** The game's scripts are marshalled
Python 2.5 code objects; Python 2.7's `marshal` reads them unchanged and Python
3's cannot read them at all. The archive and NDF readers would port fine, but
splitting the three across two interpreters buys nothing.

## Where to point them

Six packages under `Data\PC\<version>\`, plus one archive per map under
`Maps\PC\`. The two worth opening first:

| file | holds |
|---|---|
| `ZZ_GladNotPatchableWin.dat` | `system3d\shaders.cpp` (the whole material system), `system3d\scene.cpp` (98 render layers), `visualdebuginfohandler.cpp` |
| `ZZ_GladPatchableWin.dat` | `gfx\everything.cpp` (2,772 gameplay objects), `gfx\gdconstanteoriginal.cpp` (one object, 214 tuning constants), `map\<name>\mapterrain.cpp` |

`.ipk` files inside `ZZ_Win.dat` are themselves `edat` archives — pull them out
with `ruse_edat.py`, then open them directly. `.kdt` files are uncompressed NDF,
so `ruse_ndf.py` opens them too; that is where the baked occlusion trees are.

## Three things worth knowing before you start

1. **The NDF value decoder is incomplete and says so.** Several type codes
   (notably `0x14`) are unresolved. `Ndf.objects()` stops decoding an object when
   it meets one and resyncs on the next `0xABABABAB` marker rather than returning
   plausible garbage — so a `<stopped>` entry in the output is the reader being
   honest, not a crash.
2. **Names are always safe, values sometimes are not.** `CLAS`, `PROP`, `STRG`
   and `TRAN` are plain length-prefixed string tables read directly, so class and
   property names never depend on the value decoder. Most of what
   `study/games/strategy/ruse.md` concludes rests on those 1,091 class names and 4,309 property
   names.
3. **The dictionary is a prefix tree, and the padding rule differs between file
   and directory records *and* between archive versions.** v1 pads files on an
   odd name length and directories on an even one; v2 pads both on even. Get it
   wrong and the walk **desyncs rather than fails** — R.U.S.E. reported exactly
   one file and Wargame reported 33 of 2,640 with garbage sizes, both of which
   read as a small archive rather than as a bug. `_pad()` is the whole fix and
   carries the rule in its docstring.

## Known limitations

* **No asset conversion at all.** `.tgv` textures, `.ess` audio, `.baf`
  animation and the `.tms` terrain chunk payloads are located and sized but not
  decoded. Nothing here needed them.
* **The `IMPR`/`EXPR` name trees are not walked**, so objects are identified by
  class and index rather than by their qualified names.
* **Only the gameplay half of a modern build needs these at all.** From Steel
  Division on, Eugen's own mod pipeline exports the gameplay database as
  **plain-text NDF** — run `GenerateMod.bat` in a `Mods\<name>\` folder and read
  the result with `grep`. These readers are for the engine-side data that the mod
  pipeline does not export, and for R.U.S.E. and Wargame, which predate it.
