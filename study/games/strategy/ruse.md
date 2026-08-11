# R.U.S.E. and IRISZOOM — read from the shipped build

Deep dive on **R.U.S.E.** (Eugen Systems / Ubisoft, 2010) and the **IRISZOOM**
engine underneath it: how the terrain is built and drawn, how the seamless
strategic-to-ground zoom is paid for, what spatial structure answers "can this
unit see that one", how combat and deception are specified, and how the AI is
put together.

> **On sources, up front — and this note supersedes the guesswork.**
> [`map_scale.md`](../../topics/scale/map_scale.md) opens with a warning that Eugen have published
> nothing and that everything architectural there is inference. That warning was
> correct about Eugen's *publications* and wrong about the *evidence available*.
> R.U.S.E. ships its entire content pipeline as data, and the containers are
> readable: `edat` archives, `EUG0/CNDF` object files, and marshalled Python.
>
> Everything tagged **[BUILD]** below was read out of the retail install on this
> machine — `E:\SteamLibrary\steamapps\common\R.U.S.E`, data version `190852` —
> with three small readers written for the purpose and kept in §12. Class names,
> property names and values are Eugen's own identifiers, transcribed, not
> paraphrased. Where the value parser could not resolve a type the note says so
> rather than guessing.

**§11 carries the story forward.** R.U.S.E. is the first game on this engine, and
*Wargame: Red Dragon* (2014) and *WARNO* (2022) are readable here too — the
former as a version-2 `edat` archive, the latter as Eugen's own **plain-text NDF
source**, which their mod pipeline exports with the comments intact. So "did this
technique survive?" is a check rather than a guess, and §11 is the twelve-year
diff: what carried through untouched, what was rebuilt, and the single move that
accounts for most of the change.

Tags: **[BUILD]** read from R.U.S.E.'s shipped data. **[BUILD-WRD]** read from
Wargame: Red Dragon's. **[BUILD-WARNO]** read from WARNO's NDF source.
**[EUGEN]** Eugen's own published statement. **[COMMUNITY]** player- or
modder-derived. **[inferred]** our reading.

Related: [`broken_arrow.md`](../flight/broken_arrow/broken_arrow.md) — **the control experiment**: the
same genre in 2025, on Unity, assembled from bought packages rather than built,
and choosing the opposite networking model. Read its §10 against this note's §5,
§9 and §11. **[`vehicle_animation.md`](../../topics/animation/vehicle_animation.md) extends §11's
twelve-year diff into the animation layer** — how a turret, a suspension, a
track and a visible crewman are driven across all four builds, and the finding
that R.U.S.E.'s 2010 models are the *elaborate* ones that later got simplified.
Also [`map_scale.md`](../../topics/scale/map_scale.md) (the older, outside-in note —
§1.2 here corrects its central number),
[`terrain_rendering.md`](../../topics/world/terrain_rendering.md),
[`world_streaming.md`](../../topics/world/world_streaming.md),
[`spatial_queries.md`](../../topics/agents/spatial_queries.md),
[`lod_systems.md`](../../topics/world/lod_systems.md), [`battle_scale.md`](../../topics/scale/battle_scale.md).

---

## 1. The scale, with the actual numbers

### 1.1 Every map, measured

**[BUILD]** Each map ships a baked occlusion tree whose bounding box is the
terrain's true extent, and two baked terrain meshes whose headers carry a chunk
grid. Reading all 33 shipped maps:

| Map | Extent (m) | High-def chunks | Low-def chunks | Occlusion tris | Subtrees | Relief (m) |
|---|---|---|---|---|---|---|
| Chess, Valley, Swamps, … | 1310.72 × 1310.72 | 4 × 4 | 2 × 2 | 88k–200k | 122–282 | 49–146 |
| Hurtgen | 1310.72 × 655.36 | 4 × 2 | 2 × 1 | 198,387 | 258 | 96 |
| Alpha, Beta, Square, Korsun, … | ~1965 × ~1965 | 6 × 6 | 3 × 3 | 100k–206k | 139–286 | 42–155 |
| M06_Ardennes | 1966.08 × 1310.72 | 6 × 4 | 3 × 2 | 97,639 | 131 | 76 |
| M03_Italie, CompassRose, Dolly | 2621.44 × 2621.44 | 8 × 8 | 4 × 4 | 102k–200k | 140–259 | 84–128 |
| M02_Tunisie | 3276.80 × 1966.08 | 10 × 6 | 5 × 3 | 196,559 | 263 | 111 |
| **M04_Cotentin** (largest) | **3932.16 × 2621.44** | **12 × 8** | **6 × 4** | 102,000 | 131 | 47 |

Three things fall straight out of that table.

**The chunk size is exact and universal.** Every extent is an integer multiple of
**327.68 m**, and the high-def chunk count is exactly extent ÷ 327.68 in each
axis. The low-def grid is exactly half the count per axis, so its chunk is
**655.36 m** — the same ground, one quarter the chunks. Two baked terrain
representations, 2:1 linear, 4:1 in area.

**The occlusion triangle count is a budget, not a function of area.** Cotentin
covers 10.3 km²; Chess covers 1.7 km². Cotentin's occlusion mesh has *fewer*
triangles (102,000 against 88,153 — same order). The counts cluster tightly on
~100k and ~200k across the whole set. **[inferred]** That is a decimation target
chosen per map, not a resolution: whatever the map's size, line-of-sight costs
about the same. Subtree count tracks triangles at roughly one subtree per
**700–780 triangles**, which is the streaming granule.

**R.U.S.E. maps are 1.3–3.9 km per side.** Not 150 km².

### 1.2 The correction to `map_scale.md`

**[BUILD]** `map_scale.md` §1.1 imports Wargame's published "up to 150 km²" and
reads R.U.S.E. through it. That is the wrong end of the family. R.U.S.E.'s
largest shipped map is **10.3 km²** — a factor of **fifteen** smaller — and the
median is about 4 km².

And the unit count is smaller still. From the shipped tuning table:

```
PopCapTotal      200
PopCapPerAlliance 100
PopCapPerPlayer  100
GhostCap          25
```

**[BUILD] Two hundred units, total, across every player.** `GhostCap 25` is the
separate cap on queued placement previews.

**[inferred] So R.U.S.E. is not an extent problem and it is not a count problem.
It is a *view* problem.** Two hundred units on four square kilometres is a
thinner battlefield than [`battle_scale.md`](../../topics/scale/battle_scale.md)'s Total War field
by three orders of magnitude in density, and the simulation is correspondingly
cheap. What is expensive is that the player may put the camera anywhere on a
continuum from twenty metres above a tank to the whole map in frame, at any
moment, with no loading — and the engine must have a correct representation of
everything at every point on that continuum. Nearly every architectural decision
below is downstream of that one requirement, and §3 and §4 are almost entirely
about it.

That reframes the older note: Eugen did not build a big-map engine and shrink it
for R.U.S.E. They built a *continuous-zoom* engine for R.U.S.E., and Wargame
later pushed its extent. The zoom came first.

---

## 2. How the build is packaged, and why it is legible

Worth a section because it is the reason this note can be first-party at all, and
because two of the three ideas are good ones.

### 2.1 `edat` — the archive

**[BUILD]** All content sits in `edat` containers. Version 1 (R.U.S.E.; Wargame
onward use version 2). Header:

```
0x00 'edat'
0x04 uint32 version
0x08 16-byte dictionary MD5      (zero in R.U.S.E.)
0x19 uint32 dictionary offset
0x1D uint32 dictionary length
0x21 uint32 content offset       (== dictionary offset + length)
0x25 uint32 content length
```

The dictionary is a **prefix tree**, not a flat path list: directory records
carry a shared string and a byte span, file records carry offset, size and a
suffix. `genpython\1000\test\map\` is stored once and the map names hang off it.
Fields are 2-byte aligned by padding after each name.

The retail install is six packages:

| Package | Contents |
|---|---|
| `ZZ_Win.dat` (2.29 GB) | 24,058 files — 17,462 `.ess` (**[inferred]** audio: the count matches the `gen_sound\` subtree exactly, and the paths are `…\scripting\dialog\dev\fr\*.ess`), 3,831 `.tgv` textures, 1,085 `.truendfbin` texture-group descriptions, plus the resource packs |
| `Data_Common.dat` (901 MB) | 96 `.webm` videos and the fonts |
| `ZZ_GladPatchableWin.dat` | 741 `.gladndfbin` — the patchable half of the object database |
| `ZZ_GladNotPatchableWin.dat` | 39 `.gladndfbin` — the half a patch may not touch |
| `DataMap_Win.dat` | 117 `.ndfbin`, 111 `.kdt`, 102 `.scenario`, 34 `.win` — per-map level design |
| `IA_Common.dat` | 82 marshalled Python modules — mission scripting and the AI test suite |
| `Maps/PC/DataMap*.dat` (33 files, 23–148 MB) | the baked terrain, one archive per map |

**[inferred] The patchable / non-patchable split is the interesting bit.** The
shader database, the render graph, the graphics values and the memory-manager
settings are in the package a patch cannot rewrite; unit stats, scenery sets and
scenario data are in the one it can. That is a deliberate blast radius: a live
balance patch can never accidentally change the renderer.

### 2.2 `EUG0`/`CNDF` — the object database

**[BUILD]** NDF is Eugen's serialised-object format, and it is the same format
still shipping in WARNO fourteen years later. A file is `'EUG0' 0 'CNDF'`, a
compression flag at `0x0C` (`0x80` = one zlib stream from `0x2C`, `0` = stored),
then a payload holding a `TOC0` section directory of nine sections:

| Section | Contents |
|---|---|
| `OBJE` | object records — class index, then property records, terminated by `0xABABABAB` |
| `TOPO` | a permutation of object indices: the topological order objects must be constructed in |
| `CLAS` | class names, length-prefixed |
| `PROP` | property names, length-prefixed, each followed by its owning class index |
| `STRG` | string literals |
| `TRAN` | the tokens qualified names are built from — `"$"`, `"M3D"`, `"Shader"`, `"MipMapBias"` |
| `IMPR` / `EXPR` | the import and export name trees |
| `CHNK` | chunk table |

A property record is `propIdx(4) typeIdx(4) value`, the value's width set by the
type. An object reference is eight bytes: a tag (`0xAAAAAAAA` for a cross-file
name reference, `0xBBBBBBBB` for a local object) and an index.

**[inferred] `TOPO` is the part worth stealing.** The object graph is a DAG with
forward references, and rather than fixing up pointers at load time or requiring
the file to be written in dependency order, Eugen ship the dependency order
*separately* from the storage order. Objects are laid out however the writer
found convenient; the loader walks `TOPO` and never has to patch a pointer.

### 2.3 `genglad` — NDF files named after the C++ that declares them

**[BUILD]** Every object file's path is the source file that defines its schema:

```
genglad\nonpatchable\system3d\shaders.cpp.gladndfbin      (26 KB, 697 objects)
genglad\nonpatchable\system3d\scene.cpp.gladndfbin        (13 KB, 345 objects)
genglad\nonpatchable\system3d\visualdebuginfohandler.cpp.gladndfbin (27 KB)
genglad\patchable\gfx\everything.cpp.gladndfbin           (597 KB, 2,772 objects,
                                                           386 classes, 3,338 props)
genglad\patchable\gfx\everything_debuginfo.cpp.gladndfbin (823 KB)
genglad\patchable\gfx\gdconstanteoriginal.cpp.gladndfbin  (1 object, 214 properties)
```

**[inferred]** The pairing of `X.cpp.gladndfbin` with `X_debuginfo.cpp.gladndfbin`
— the debug variant being 38% larger — says the shipped data drops the symbolic
names it does not need and keeps a parallel file that has them. The gameplay
database is literally called `everything.cpp`.

Across the 780 object files there are **1,091 distinct classes and 4,309 distinct
property names**. That vocabulary is the architecture, and most of the rest of
this note is reading it.

### 2.4 `genpython` — the game is scripted in Python 2.5

**[BUILD]** Mission scripting, level-design descriptors and the strategic AI's
objective layer are Python, shipped as `.xyz` files: `'XYZ0'`, the uncompressed
length at offset 6, a hash, then a zlib stream containing a **marshalled Python 2
code object**. Python 2.7's `marshal` reads them unchanged. The interpreter is
2.5 — `pythonpacklib25.ipk` ships the standard library.

The module tree is rooted at `datadir:\codeia\python\` — *code IA*:

```
eugen/base/{camp, geo_database, world_helper, eug_timer, pickling}
eugen/defines/front/{bluff, combat, visibilite, economie, game, strategic_ia}
eugen/front/{unit, batiment, fake_unit, game_rules, game_state, technologie_manager}
eugen/game/{world, camera, loadsave, nuclear_mode, total_war_mode}
eugen/headup/policies/{placement_ghost, move_ghost, placement_bluff, cancel_ghost}
eugensolo/leveldesignsolo/{strategic_ia, production, objectif, missions, units, camps}
eugentest/leveldesigntest/{rapports_de_force, testauto}
```

---

## 3. Terrain

### 3.1 Two baked meshes, not one mesh with LODs

**[BUILD]** Every map archive contains, in `output\`:

```
highdef.tmst_chunk_pc    42.9 MB    the streamed chunk payloads
highdef.tms               2.6 MB    the mesh header/table
highdef.tmst_pc           6.1 KB    the stream table
lowdef.tmst_chunk_pc     11.2 MB
lowdef.tms              658.7 KB
lowdef.tmst_pc            1.6 KB
```

`TMSG` headers carry the chunk grid and per-chunk extent; `TMST` stream tables
carry `ATEX` / `KEYS` / `TEXF` fourcc sections. For Alpha: high-def is 6 × 6
chunks of 327.68 m, low-def is 3 × 3 chunks of 655.36 m — same 1.97 km square.

**[inferred] This is not a LOD chain on one mesh, it is two independently baked
meshes over the same ground**, and the shader system confirms it holds them apart
at runtime: the uniform list includes both

```
uniWorldBoundingBoxMin / uniWorldBoundingBoxMax
uniWorldBoundingBoxMinLowDef / uniWorldBoundingBoxMaxLowDef
```

so the low-def world has its own world bounds, distinct from the high-def one.
The reason is §3.3: vertex positions are quantised into a bounding box, and a
coarser mesh over the same ground wants a different quantisation, so it cannot
share the box.

**[inferred]** The cost is obvious — 54 MB of terrain per map instead of 43 —
and the payoff is that the strategic view never touches high-def data at all.
Contrast [`world_streaming.md`](../../topics/world/world_streaming.md) §5, where Epic's
`ComputeLODBias()` clamps mesh LOD to the resident heightmap mip precisely
because there is only one representation and it can be half-loaded. Two separate
bakes cannot half-load; the low-def one is small enough to be resident.

### 3.2 The multi-scale level chain, and what streaming is told

**[BUILD]** Per-map, `genglad\patchable\map\<name>\mapterrain.cpp.gladndfbin`
holds a chain of `TTerrainMultiScaleBuilder_LevelDescriptor` objects, each
pointing at the next:

| Property | Alpha's values |
|---|---|
| `TransitionProportion` | `1` at the head, `0.5` at every level below |
| `NextLevel` | the next descriptor in the chain |
| `Size` | per-level extent |
| `PriorityInViewZone` | `-13`, `12`, … |
| `PriorityNearlyInViewZone` | `-18`, `-16`, … |
| `PriorityNear` | `-21`, `-19`, … |
| `DontKeepStreamOfVisibleTexture` | `True` on the coarse levels |
| `PlatformGenerationMask` | `1`, `4`, `7` |

Two chains of four to five levels each. The shaders take
`uniTerrainMultiScaleLevel`, `uniTerrainBorder` and `uniTilePitchUV`, and there
are dedicated parameter classes
`TUniformShaderParameterfloatFromTerrainMultiScale_Level` and
`TUniformShaderParameterfloat4FromTerrainMultiScale_WorldToUVProjection`.

**[inferred] The streaming priorities are the interesting data, not the LOD
ratios.** Three separate priorities per level — *in view*, *nearly in view*,
*near* — and they are signed and they cross over. At the fine level the in-view
priority is **−13** and at a coarser level it is **+12**: a coarse tile that is
merely near outranks a fine tile that is in frame. That is exactly right for a
camera that can zoom out faster than a disc can fill, and it is the opposite of
what a naive "load what's on screen" scheduler does.

`PlatformGenerationMask` (1, 4, 7) says which of PC / PS3 / 360 bake each level —
so the console builds simply do not generate the finest tiers.
`DontKeepStreamOfVisibleTexture` on the coarse levels says their textures are
allowed to be evicted while visible, because the next level up will cover.

### 3.3 Quantised vertices

**[BUILD]** The shader uniform set includes

```
uniMeshQuantizationBBoxMin  uniMeshQuantizationBBoxMax
uniMeshQuantizationMul      uniMeshQuantizationAdd
```

with dedicated terrain variants, `TUniformShaderParameterTerrainQuantizationBBoxMin`
and `...Max`.

**[inferred]** Terrain and mesh vertex positions are stored as integers and
expanded in the vertex shader by a per-object (per-chunk) affine transform. At
327.68 m per chunk, 16 bits gives 5 mm precision — far finer than needed — and
halves the vertex bandwidth against float32. This is the same trick
[`voxel_terrain.md`](../../topics/world/voxel_terrain.md) notes for Space Engineers' chunked
meshes, and it is the concrete reason the low-def mesh needs its own world
bounding box.

### 3.4 The diversity map — the anti-tiling layer, and its 14-year survival

**[BUILD]** Every map bakes `output\div_map.tgv` (3.59 MB for Alpha); the shaders
sample it as `uniDiversityMap`, and the map constants reference
`$/MapConstante/MapInstance/Map_TextureDiversity.FileName`.

**[COMMUNITY]** WARNO's published map-modding pipeline (2024) requires a
`Div_map.png`, described as *"used as base color influencing terrain, fillers and
vegetation color"*.

**[inferred]** Same file, same job, fourteen years apart: a low-frequency
whole-map colour field multiplied over terrain, ground clutter *and* vegetation
so the three vary together. This is [`terrain_rendering.md`](../../topics/world/terrain_rendering.md)
§5's cheapest anti-tiling layer — macro variation — and Eugen reached for it
first because at strategic zoom the entire map is on screen at once and a
repeating 8 m texture is visible as a plaid.

The same per-map bake also produces `sdb` (a mask; WARNO's equivalent
`sdb.png` erases trees by colour), the six faces of an environment cube map, and
the water inputs — `waterinputs.tgv`, `wateracceleration.tgv`,
`riverindirectionsurface.tgv`, matching the `uniWaterSimulationInputs`,
`uniWaterAcceleration` and `uniRiverIndirectionSurface` uniforms.

### 3.5 Terrain modifiers are shipped as data

**[BUILD]** The class vocabulary includes

```
TTerrainModifierHeightMapModifierAspectFlatter
TTerrainModifierHeightMapModifierAspectSetterAbsolutePlane
TTerrainModifierHeightMapModifierAspectSetterRelative
TTerrainModifierHeightFilterBinary
TTerrainModifierHeightMapNoFilter
TTerrainBorder  TTerrainLoader  TTerrainGeometryLoader  TTerrainManager
```

**[inferred]** Flattening under a building footprint, absolute and relative plane
setting, and a binary height filter — the standard set an RTS needs so a factory
does not sit on a slope, expressed as data objects rather than editor operations
baked into the heightmap. Which means they can run at bake time *and* be
inspected.

---

## 4. The renderer

### 4.1 Cg, cross-compiled, and 1,806 shaders that dedupe themselves

**[BUILD]** Shader sources are **NVIDIA Cg** — `libcgc.dll` sits in the install
root and the shader database names 200-odd `DataDir:\Code\Shader\*.cg` files.
The PC build then ships a precompiled cache:

```
genhlsl\v13_lim_ps_4_0_vs_4_0\shadercachev02.shc     2.99 MB
```

which is itself an NDF file holding one `TShaderCompiledCache` with **1,806
`TShaderCompiledCacheEntry`**, 1,216 `TShaderParameterInfo` and 604
`TStreamedShaderParameterInfos`. Its properties:

```
APrioriHashToSusbtitutedSourceHash
SusbtitutedSourceHashToCompiledCode
StreamShaderBuildContextToCompiledCode
ObjectCode  ParameterBindings  ProfileType  MainName
Params  Size  ParamName  ParamTypeName  Reg  IsArray
```

**[inferred] Two levels of hashing, and the second one is the point.** A shader
request is hashed *a priori* — from the requested feature set — into a first key;
that maps to the hash of the source *after substitution* (§4.2); and only that
second hash maps to object code. So two different feature requests that happen to
substitute down to identical source share one compiled blob and one runtime
object. A permutation explosion is deduplicated at the only place it can be
deduplicated correctly: after specialisation, before compilation.

### 4.2 Generic function substitution — an über-shader done in data

**[BUILD]** `shaders.cpp.gladndfbin` holds 697 objects. The largest classes:

| Count | Class |
|---|---|
| 183 | `TShaderDescriptor` |
| 128 | `TMeshMaterial` |
| 73 | `TMatBinder` |
| 58 | `TShaderGenericFunctionSubstituter_Constant` |
| 52 | `TNonTextureGroupRenderState` |
| 35 | `TTextureShaderParameterConstant` |
| 23 | `TMaterialPack_Static` |
| 17 | `TMultiRenderTypeMaterialPack`, `TShaderGenericFunctionSubstitution` |
| 16 | `TMultiPassShaderDescriptor_ExplicitList` |
| 13 | `TMaterialConditionFromTagsAndTextures`, `TTextureGroupRenderState` |

plus `TShaderGenericFunctionSubstituterWithManyNames`,
`TShaderGenericFunctionSubstitution_Activable`,
`TShaderGenericFunctionSubstituter_GetTerrainLightmap` and a
`TUtilListForShaderTranslator`.

The preprocessor tokens in the string table:

```
#EUG_SHADER_VERY_LOW_QUALITY   #EUG_SHADER_LOW_QUALITY
#EUG_SHADER_MEDIUM_QUALITY     #EUG_SHADER_HIGH_QUALITY
#EUG_SHADER_PS3_QUALITY        #EUG_SHADER_XBOX_QUALITY
#EUG_SHADER_BAKING_QUALITY
#DISABLE_FOG  #DISABLE_SHADOWS  #DISABLE_SPECULAR  #DISABLE_PARALLAX
#FLAT_PARALLAX  #SHADOW_MODE  #CUBE_MAP_LIGHTING  #NEED_TANGENT
#VEGETATION  #IGNORE_SUN  #HIGHLIGHT_COLOR  #FOR_CONSTRUCTION
#WOLRD_NORMAL_ATLAS  #READ_DEPTHSHADOW_MAP_IMPOSTEUR
```

**[inferred]** A shader is written once with named generic functions left
unbound; a *substituter* object binds each name to an implementation (a constant,
a terrain lightmap fetch, a no-op); the material picks its substituter set from
tags and textures via `TMaterialConditionFromTagsAndTextures`. Seven quality
tiers — including two named for specific consoles and one for offline baking —
come out of the same source. `#WOLRD_NORMAL_ATLAS` is misspelled in the shipped
data, which is a small guarantee these are Eugen's strings and not ours.

### 4.3 The frame is a render graph declared in NDF — in 2010

**[BUILD]** `scene.cpp.gladndfbin`, 345 objects:

| Count | Class |
|---|---|
| 98 | `TRenderLayer` |
| 28 | `TRenderLayerUpdateRenderTexture` |
| 12 | `TRenderLayerSetRenderSurface`, `TRenderLayerArray` |
| 11 | `TRenderSurfaceFactory`, `TRenderTreeContextFlag` |
| 10 | `TRenderTextureFactory` |
| 9 | `TRenderLayerClearRenderSurface`, `TRenderLayerRenderRect` |
| 7 | `TWorld3D`, `TSceneChooser`, `TVariable` |
| 6 | `TScene3D`, `TDirectionalLight`, `TSceneInterface2D` |
| 4 | `TRenderTreeContext` |
| 3 | `TViewVariantConfiguration`, `TViewVariantDescriptor`, `TCameraDataRecordRTTI` |
| 2 | `TLightingEnvironment`, `TAdditionalLightingEnvironmentProperties` |

with `TIntrinsicCall_2Param` / `_3Param`, `TConstant`, `TObjectEvaluable` and
`TEugBFloat` alongside — an expression language embedded in the same file.

**[inferred] Ninety-eight render layers, the surfaces and textures they read and
write, the clears, the rect passes, and the arithmetic that parameterises them,
all as data.** This is a frame graph, authored, five years before the idea got
its GDC talk. `TSceneChooser` and `TViewVariantConfiguration` are how the same
graph serves the strategic view and the ground view: a *view variant* selects a
different layer array.

The renderer itself is deferred — **[EUGEN]** WARNO's engine devblog says the
upgrade *"upgraded our deferred engine with full PBR support"*, describing the
lineage as unbroken since R.U.S.E.

### 4.4 Impostors, with their own shadows

**[BUILD]** Classes: `TImposteurManager`, `TClusterImposteurs`, `TSceneImposteur`,
`TSceneryDescriptorImposteur`. Uniforms:

```
uniImposteurMap  uniImposteurMap2  uniImposteurMapShadow
uniImposterPackMatrices  uniMatrixInitialRotateImposteur
uniImposteurShadowPlanX  uniImposteurShadowPlanY
uniMaxDepthShadowImposteur  uniGlobalSunView  uniSunView
```

with parameter classes `TUniformShaderParameterFromSunViewMatrixImposteur`,
`TUniformShaderParameterImposteurShadowPlanX`/`Y`, and the shaders
`FESMMergeImposteur.cg`, `FFilterColorImposteur.cg`, plus the define
`#READ_DEPTHSHADOW_MAP_IMPOSTEUR` and the vertex-shader flag
`GENVS_IsImposteur`.

**[inferred] The system is baked, packed and lit, not a runtime billboard.**
`uniImposterPackMatrices` says many impostors share an atlas; the *sun view*
matrix says the impostor was rendered from the sun's frame as well as the eye's;
`uniImposteurMapShadow` plus two shadow-plane uniforms say the impostor casts and
receives shadows through its own depth map rather than dropping out of the shadow
pass. That last part is what separates a usable impostor from a visible pop, and
it is exactly the failure [`source_fps_viewmodel.md`](../valve/source_fps_viewmodel.md)
§7 documents in Source's view models — geometry in its own list that no shadow
enumeration reaches. Eugen paid for the shadow.

### 4.5 Batching: texture groups, packs and proxies

**[BUILD]** `ZZ_Win.dat` contains a `gentexgroup\` tree of 1,085 `.truendfbin`
files, one set per scenery collection —
`gentexgroup\ww2\res3d\decors\ardennes\ardennes_01`, `…\ardennes\dest_3`,
`…\ardennes\lods` — produced by an `autotexturegrouper.cpp` object
(`TAutoTextureGrouper`, `TAutoTextureGrouperTextureEnumerateur`,
`TAutoTextureGrouperDirectiveForOtherTextures`). The `lods` group is
consistently the largest in each set.

Alongside, `gentexproxy\pack\` (74 files, 11.2 MB) and the resource packs:

```
gen\pack\basesystem3d.ppk                    614 KB
gen\pack\map\<mapname>envtextures.ppk        ~5 MB each, one per map
gen_5\pack\{africa,ardennes,…}.spk           scenery, per region
gen_5\pack\basebuilding\{fr,ger,…}.spk       per faction
genanim_15\pack\*.apk                        animation
```

**[inferred]** The batching strategy is *offline atlasing by set*: every prop in
a region shares one texture group so a village is few draw calls, and the LOD
meshes get their own group so the distant version of a village is one more. The
`gentexproxy` set is the low-resolution stand-in that stays resident while the
real texture streams. The `.ppk`/`.spk` split — one environment pack per map,
one scenery pack per region — is the streaming unit, chosen so that entering a
map loads a bounded, known set.

**[BUILD]** `.baf` animation files are named `coc_eolienne_animlod0.baf` — so
**animation has LODs too**, which is [`lod_systems.md`](../../topics/world/lod_systems.md)'s
deformation axis showing up explicitly in the filenames.

### 4.6 What else the uniform list gives away

**[BUILD]** Selected uniforms, grouped:

| Group | Uniforms |
|---|---|
| Ambient | `uniNewSHCoeffs.Coeffs0To3` … `Coeffs12To15`, `uniLightingCubeMap`, `uniDefaultEnvMap`, `uniEnvMapAmplifier` |
| Sun & shadows | `uniSunColor`, `uniSunDirection`, `uniDepthShadowMap`, `uniFilteredDepthShadowMap`, `uniFilteredDepthShadowMapHighQuality`, `uniDepthShadowReferenceValueForShadowLookup` / `…ForShadowRendering`, `uniGroundShadowReceiverMap` |
| Local lights | `uniPointLightPosition/ColorAndFalloff/EmissiveCubeTexture`, `uniSpotLightPosition/ColorAndFalloff/AlphaBeta`, `uniSubLocalPassIndexAndCount` |
| Atmosphere | `uniHazeColor`, `uniHaze_Z0_AtViewer`, `uniNuagesMap_Area`, `FSunCloudTraversal.cg`, `FPlanNuage.cg` |
| Water | `uniFFTWaterSurface` (+`HighQuality`, `Slow`, `SlowHighQuality`), `uniFoamTexture`, `uniWaterAcceleration`, `uniRiverIndirectionSurface`, `FWaterAdvection.cg`, `FWaterEvolveZDZ.cg`, `FWaterAdvectFoamAndTint.cg` |
| Temporal | `uniPrevFrameModelViewProj`, `uniInterFrameProportion`, `uniRenderDt`, `uniGameDt` |
| Baked per-mesh | `uniMeshLightMap`, `uniMeshDirMap`, `uniMeshNormalMap`, `uniLightMapAmplifier` |
| UI | the `uniScaleForm*` family (Scaleform Flash) |

**[inferred]** Four levels of SH ambient, an **FFT water surface with four
quality variants** in a 2010 RTS, height-based haze with a viewer-relative
reference plane, and a `uniSubLocalPassIndexAndCount` that says local lights are
applied in *sub-passes* with an index and a count — a forward multi-pass scheme
layered over the deferred base, which is what you do when the light count is
small and bounded (it is: this is daylight WWII, the lights are muzzle flashes
and fires).

`uniPrevFrameModelViewProj` and `uniInterFrameProportion` are the two things you
need for either motion blur or temporal reprojection, and nothing else in the
data distinguishes which.

### 4.7 Deception is rendered as world-space "area" maps

**[BUILD]** A distinct family of uniforms ends in `_Area`:

```
uniFakeMap_Area   uniFakeUnitMap_Area   uniFakeBatimentMap_Area
uniFireMap_Area   uniLightningMap_Area  uniNuagesMap_Area
uniMatrixMap_Area uniEnvMap_Area
```

with `DataDir:\Code\Shader\Area.cg`, a texture `gen\ww2\res2d\area\fake_unit.tgv`
(110 KB), the constants `FxRuseHeight 1000` and `FxRuseSpacing 800`, and the
material-pack switch `MultiRenderTypeMaterialPack_ForceRevealed`.

**[inferred]** Ruses are drawn by projecting a per-effect map over the world and
letting every affected material sample it — the same mechanism that projects
clouds and fire. Separate maps for fake units and fake buildings mean the
decoy overlay is not one boolean but a typed field, and `ForceRevealed` is a
whole alternate material pack for "this thing is being shown to you even though
you should not see it". The presentation of deception is therefore a *render*
feature with its own texture channel, not a per-entity flag the mesh code checks.

---

## 5. Space: there is no grid

This is the direct answer to "how do they handle grids and grid queries", and
the answer is that the load-bearing structure is not a grid.

### 5.1 `TStreamedMeshKdTree`

**[BUILD]** Each map archive contains three `.kdt` files, each an uncompressed
NDF holding exactly one object of class **`TStreamedMeshKdTree`**:

```
RTVersion                             '1.1.4'
BoundingBoxMin / BoundingBoxMax
TriangleCount
OffsetOfMainNode
OffsetOfTriangleIndexLists
OffsetOfIndexBuffer
OffsetOfCompressedSubtrees
OffsetOfCompressedSubtreeIndexBuffer
OffsetOfVertexBufferIndexes
OffsetOfIndexBufferIndexes
OffsetOfTriangleIndexBufferIndexes
IsStreamPacked   True
IsCompressed     True
SubtreeCount
Storage          <the packed blob>
```

The loader is `TStreamedMeshKdTreeLoader`; the world's is
`TWorldFloorStreamedMeshKdTree`; the bake parameters are `TMeshKdTreeBuildParams`
in `miscgenerationparams.cpp`.

**[inferred] Read the property list as a design statement.** The tree is
**streamed** (a root node plus independently-loadable subtrees), **compressed**,
and **stream-packed** — its index buffers, triangle index lists and vertex
indices each have their own offset so a subtree can be paged in without touching
the rest. That is a spatial index built to the same constraint as the terrain:
the camera can be anywhere, so the structure must be partially resident.

### 5.2 Three trees, split by *source* and by *purpose*

**[BUILD]** For M04_Cotentin:

| File | Bounding box (m) | Triangles | Subtrees | Size |
|---|---|---|---|---|
| `occlusioninfo_terrainonly.kdt` | 0…3932 × 0…2621, relief 47 | 102,000 | 131 | 1.37 MB |
| `occlusioninfo_objectsonly.kdt` | 1120…3123 × 1013…2338 | 556 | 1 | 6.3 KB |
| `occlusioninfo_camera.kdt` | **−175…4107 × −175…2796** | 15,196 | 15 | 188 KB |

**[inferred] Three facts, all of them consequences of the split.**

**Terrain and objects are separate trees because the question differs.** A shot
blocked by a hill is permanent; a shot blocked by a building is not. Keeping them
apart means the object tree can be small and rebuilt, and the terrain tree — the
expensive one — never is. It also means a query can ask *terrain only* cheaply,
which is what a long-range artillery or aircraft check wants.

**The object tree is startlingly small.** 556 triangles on the largest map, one
subtree, 6 KB. Whatever "objects" means for occlusion, it is a handful of hand-
placed blockers, not the map's visible scenery. Villages and forests are not in
it. That squares with R.U.S.E.'s design — forest is a *stat modifier*
(`CouvertBonusForet`, §7) rather than geometry you trace against — and it is the
same economy [`spatial_queries.md`](../../topics/agents/spatial_queries.md) §3.6 records for
CryEngine: raycasts are the dominant AI cost, so the thing you trace against is
deliberately not the thing you draw.

**The camera tree is a third of a different kind.** It extends **175 m beyond the
map on every side**: its bounding-box minimum is exactly `(-175000, -175000)` on
all 32 maps that ship one, regardless of map size. And it holds seven times the
triangles of the object tree but a seventh of the terrain tree. **[inferred]**
It is the surface the camera collides and slides against, so it needs to exist
outside the playable area (you can look in from beyond the edge) and it needs to
be smooth rather than accurate. Gameplay visibility and camera collision get
different meshes because they want different errors.

### 5.3 The bluff zones are the same structure

**[BUILD]** `DataMap_Win.dat` holds 111 `.kdt` files under
`test\map\<name>\zonebluff\leveldesign*.kdt` — one per map *per scenario variant*
(`leveldesign`, `leveldesign_challenge`, `leveldesign_2v2_v01`, …). Alpha's:

```
class TStreamedMeshKdTree
BoundingBoxMin  (348853.8, 190681.0,      0.0)
BoundingBoxMax (1725814.6, 1673319.2, 120000.0)
TriangleCount   686
SubtreeCount    1
```

Alongside them, `.scenario` files (`SCENARIO\r\n` + chunked records) whose `AREA`
chunks carry GUID-named zones — `zone_a78c9198-ab02-4608-a887-7d6a2c1bd692` —
and float vertex lists.

**[inferred] This is the finding that answers the question outright.** A ruse
zone in R.U.S.E. — the thing you drop a card on, the thing that gets a coloured
border on the strategic view — is **an arbitrary polygon, triangulated, extruded
to 120 m, and indexed by exactly the same kd-tree class as terrain occlusion**.
"Which zone is this point in" is a mesh query, not a grid lookup. There is no
sector array, no dalle index, no `(x,y) → sector` arithmetic.

And it costs 686 triangles. **The whole zone partition of a 2 km map is nine
kilobytes.**

The gain is that zones follow terrain — a river, a ridge, a town boundary — so
the strategic layer's units of territory are the same shapes the tactical layer
fights over. A grid would have forced the designer to approximate a valley with
squares, and every player would have seen the squares.

### 5.4 What *is* a grid, then

**[BUILD]** Grid-shaped concepts do exist, and they are all coarse and all
gameplay-side:

- `taille_case_portee_vision` — "vision-range **cell** size", a module-level name
  in `eugen/game/world.py`.
- `tps_rafraichissement_infmap` — "**influence map** refresh time", same module.
- `CaseSizeForMediumDiscrimination`, `CaseInfoKey`, `SupportSubCase` — properties
  in the gameplay database.
- `DalleBatimentDepotDescriptor`, `RayonRechercheDalleEtBatiment` (`130000`) —
  "**dalle**" (slab) is the placement lattice for depot buildings; the strategic
  AI's objectives take a `DallesInterdites` (forbidden-slabs) list.
- `AreaManagerClient`, `_zone_snap`, `LDDetectorProcessing`, `SlicedProcessing`,
  `unsliced_ia` — all in `eugen/game/world.py`.

**[inferred] So the division of labour is clean, and it is the right one.**
Geometry queries — line of sight, camera collision, zone membership — go to
kd-trees over triangles, because the world's shape is arbitrary. Aggregate
queries — where is the enemy massing, which cells can this unit see into, where
may a depot go — go to coarse regular structures, because those questions want
uniform buckets and cheap iteration. Trying to answer both with one structure is
the mistake, and this codebase makes the same split (`OcclusionGrid` and
`RayCaster` versus `SpatialHash`) for the same reason.

**[BUILD] And the vision circle really is traced.** The gameplay database carries
paired properties `GfxCercleZoneVision` / `GfxCercleZoneVisionSansRayTrace` and
`GfxCercleCheminPorteeMax` / `GfxCercleCheminPorteeMaxRaytrace`, and
`miscgenerationparams.cpp` carries `RayTraceTexture_SubdivisionThreshold`.
**[inferred]** The range ring you see under a selected unit is the result of
casting against the terrain tree and rasterising the answer to a texture with
adaptive subdivision — with a non-traced fallback for the low tiers. The player
is being shown the actual visibility solution, not a circle.

---

## 6. Deception as an architecture

### 6.1 Belief is per-observer, and it is a mode not a flag

**[BUILD]** `eugen/defines/front/visibilite.py`:

```python
class fow:
    names = ('no_fow', 'default_fow', 'total_fow')
couvert_bonus_foret
dico_hint_signature_radar
def int_to_fow_status(fow_status)
def get_no_fow_status(current_fow)
```

and the gameplay database carries `ForceRevealed`, `StateMaskReveal`,
`TimeToBeStealthedAfterReveal` (`10` s), `TempsActivationBatimentCamoufle`,
`InvisibleUniteWithScannerDescriptor`, `EnemyPlanRevealTime` (`5` s).

**[inferred]** Three fog states rather than two, a *reveal* that is a state mask
with a ten-second decay back to hidden, and a scanner-relative invisibility.
[`map_scale.md`](../../topics/scale/map_scale.md) §3.3 inferred that R.U.S.E. must hold a
per-player believed state that can diverge from truth; `StateMaskReveal` plus
`ForceRevealed` plus a per-material `MultiRenderTypeMaterialPack_ForceRevealed`
confirms it, and shows the cost is carried all the way into the material system.

### 6.2 The deception constants, in full

**[BUILD]** From `gdconstanteoriginal.cpp` and `eugen/defines/front/bluff.py`:

| Constant | Value |
|---|---|
| `NbMaxCardsInPool` | 99 |
| `MaxNbCardsPerZoneByAlliance` / `max_nb_cards_per_zone_by_team` | **2** |
| `NbInitialCardsInPoolForAllianceTaille_1..4` | 2 each |
| `PaliersTempsToChooseNewCardForAllianceTaille_1..4` | `[105, 210, 315]` s |
| `NbMin/Max_UniteLeurre_OffensiveGenerale` | 4 / 5 decoy units |
| `NbMin/Max_UniteLeurre_OffensiveAerienne` | 4 / 5 |
| `NbMin/Max_UniteLeurre_OffensiveBlinde` | 4 / 5 |
| `Ratio_Infanterie_OffensiveGenerale` | 0.5 |
| `nb_fake_buildings_to_build` | (1, 3) |
| `ConstructionDelayForFakeBuildingsMin/Max` | 5 / 10 s |
| `fake_building_fake_defense_ratio` | 0.3 |
| `EnemyPlanRevealTime` | 5 s |
| `MinArmyValueToUseManipulationCard` | 120 |

**[inferred]** The card economy is identical for every alliance size — the same
two starting cards and the same 105/210/315-second ladder whether it is 1v1 or
4v4 — so deception does not scale with player count, which keeps a big team game
from drowning in ruses. The **two cards per zone** cap is the load-bearing rule:
it bounds how much a zone's information state can be corrupted at once, which is
both a balance decision and, since a zone is a mesh region and cards attach to
it, a bound on how much per-zone state the network has to carry.

### 6.3 The tell that deception was designed in, not added

**[BUILD]** `TInGameCameraMoverForBluffZoneCardLaid` — a *camera mover* whose
entire reason to exist is "a bluff card was laid on a zone". `TBluffInfos`,
`TBluffZoneManagerDescriptor`, `TBluffCardDescriptor`,
`TInterfaceInGameBluffInfosResource`, `TInterfaceInGameResourceBluffInfos`.
The acknowledgement system is separately typed for it:
`TAcknowHQIntelDescriptor`, `TAcknowHQIntelContainerDescriptor`,
`TAcknowUnitDescriptor`, `TAcknowManagerDescriptor`.

**[inferred]** Deception reaches the camera system, the material system, the HUD
resource system and the acknowledgement (voice-line) system. It is not a
game-logic feature with a UI; it is a cross-cutting concern the whole engine
knows about. Which is the practical form of `map_scale.md` §4.2's fourth lesson:
*decide early whether visibility is per-observer*, because retrofitting this
would mean touching every one of those systems.

---

## 7. Combat, and the constant table

### 7.1 The tuning object

**[BUILD]** `genglad\patchable\gfx\gdconstanteoriginal.cpp.gladndfbin` is a
single object of class **`TTunableConstante` with 214 properties**. There is a
sibling, `gdconstanteatomic.cpp`, for the nuclear game mode
(`eugen/game/nuclear_mode.py` is its scripting half). Selected values, verbatim:

| Constant | Value | Reading |
|---|---|---|
| `DistanceMinimumVision` | 26000 | floor on any unit's sight |
| `DistanceMinimumVisionBatimentReco` | 208000 | recon buildings see 8× further |
| `CouvertBonusForet` | 78000 | forest concealment radius |
| `DistanceIgnoreForet` | 26000 | inside this, forest stops helping |
| `DistToOpenFire` | 104000 | |
| `DistancePourchasse` / `…Increment` | 65000 / 2600 | pursuit leash and its growth |
| `PorteeMinArmeEstConsidereCommeArtillerie` | 169000 | **a weapon is artillery iff its minimum range ≥ this** |
| `DistanceMaxPourAttackWhileMoving` | 169000 | |
| `PorteeObligationDeclenchementTirEnForet` | 78000 | |
| `PourcentagePorteeMaxPourDeclencherTirEnForet` | 1 | |
| `CombatZoneRadius` | 26000 | |
| `DistanceFriendlyFire` | 26000 | |
| `ToleranceTirTouche` | 6000 | hit tolerance |
| `MultiplicateurPinnedEmbuscade` | 3 | ambush suppression multiplier |
| `MultiplicateurDispersionQdTireurEnMouvement` | 1.5 | firing on the move |
| `RatioMultiplicateurPinnedProtectionForet` | 0.6 | |
| `RegenerationPinnedHorsCombat` | 10 | |
| `TempsSansTirNiDamagePourPasserHorsCombat` | 2 s | |
| `TempsSansTirNiDamagePourRegen` | 15 s | |
| `EmbuscadeAutomatiqueEnForet` | True | units ambush in forest without being told |
| `DistanceMaxDeRechercheDePointEmbuscade` | 13000 | |
| `AltitudeVol` | 52000 | flight altitude |
| `NbAvionsParAeroport` | 8 | |
| `SecondesEntreDeuxDecollages` / `…Atterrissages` | 3 / 1.666 | |
| `DistanceCoherence` | 130000 | formation cohesion radius |
| `Seuil_CampColor_Avion / Batiment / Unit` | 60000 / 130000 / 180000 | **camera altitudes at which each becomes a flat faction-coloured token** |
| `QteDeviseInitiale` | 200 | |
| `QteDeviseParCamion` / `NbCamionParConvoi` | 3 / 3 | |
| `TempsENtreDeuxConvois` | 30 s | (Eugen's capitalisation) |

**[inferred] The unit is the millimetre**, on three independent corroborations:
`RF_NbMetreMaxAParcourir` — a constant whose *name* says "number of metres" —
holds `182000`; `tir_ghost_parabolic_height` in `defines/front/combat.py` is
`40000`, a sane 40 m arc for a shot preview and an absurd 400 m one; and the map
extents come out at 1.3–3.9 km, which is what R.U.S.E. maps look like. So 26000
is 26 m, `AltitudeVol` is 52 m, and Cotentin is 3.93 × 2.62 km.

**[inferred]** Note that **26 m is the quantum**: 26000, 52000, 78000, 104000,
130000, 169000 (6.5×), 182000, 208000, 260000 are all multiples, and 13000, 6500,
2600 are its fractions. Almost the entire combat model is expressed in a single
gameplay grain. Two constants — `DistanceZoneAlerte 78` and
`DistanceMaxAParcourirPourSupportAttaqueEnnemi 52` — are the same numbers with
the 1000× dropped, i.e. stated in metres in a table otherwise stated in
millimetres. That is a shipped unit inconsistency, and the kind that survives
because both values happen to be used in code that converts.

### 7.2 Zoom level is a gameplay parameter

**[BUILD]** `eugen/defines/front/combat.py`:

```python
temps_minimum_de_maintien_d_ordre_move_and_attack_zoom1 = 30
temps_minimum_de_maintien_d_ordre_move_and_attack_zoom2 = 30
temps_minimum_de_maintien_d_ordre_move_and_attack_zoom3 = 30
temps_minimum_de_maintien_d_ordre_move_and_attack_zoom4 = 30
temps_minimum_de_maintien_d_ordre_zoom1 = 0     # …zoom2, zoom3, zoom4 = 0
temps_minimum_de_maintien_d_ordre_from_multiple_target_manager_attack = 30
```

and in the tuning table, `TempsMinimumDeMaintienOrdre_UniteMoveAndAttack 30`,
`…_OrdreAttack 30`, `…_UniteSansArme 10`.

**[inferred] There are exactly four zoom levels and they are a first-class
gameplay concept, not a camera range.** Every order type has a *minimum hold
time* declared per zoom level — how long a unit must persist with an order before
it may be re-evaluated. Attack orders hold for 30 units of time; plain moves hold
for 0.

The values are currently equal across all four levels, which is the more
interesting fact: **the mechanism was built to differ by zoom and then tuned
flat.** Someone anticipated that an order issued from the strategic view means
something different from one issued from the ground — coarser intent, longer
commitment — built the hook, and shipped it uniform. The `Seuil_CampColor_*`
thresholds (60/130/180 m) show the *presentation* side of the same idea did
differentiate.

That hold time is also this repository's hysteresis rule under another name.
[`motion_matching.md`](../../topics/animation/motion_matching.md) §8.2 and CLAUDE.md's *"when a choice
is re-made repeatedly, give the incumbent a small discount"* describe a bias;
Eugen's version is a hard floor in time. Same failure prevented — a unit that
re-decides every tick and visibly dithers — and the time floor is the blunter
instrument, because it cannot be overridden by a genuinely better option
appearing at second five.

### 7.3 Armour and damage

**[BUILD]** `everything.cpp` carries `TUniteDescriptor`, `TWeaponDescriptor`,
**`TMountedWeaponDescriptor`**, `TAvionDescriptor`, `TBatimentDescriptor`,
`TUniteAuSolDescriptor`, `TUniteBehaviourDescriptor`, `TUniteDescriptorBarycentre`,
`TBaseUniteConstantes`, and **`TFloatArmeArmureContainer`** — a float container
indexed by weapon × armour. The HUD ships icons `blindage_1` … `blindage_5`, plus
`infanterie`, `vehicule`, `tank`, `avion`, `batiment`. Properties include `Arme`,
`ArmureHint`, `BaseBlindage`, `Damage`, `BiasForDamage`.

**[inferred]** Five armour classes crossed with weapon types through a float
matrix — the same shape [`battle_scale.md`](../../topics/scale/battle_scale.md) describes for
Men of War and the same shape WARNO still ships as `DamageResistance.ndf`.
**[COMMUNITY]** `TMountedWeaponDescriptor` is one of the class names WARNO
modders document in 2024 — a literal identifier surviving fourteen years and
four games in the same engine.

---

## 8. The AI

### 8.1 A strategist running objectives against a request economy

**[BUILD]** `eugensolo/leveldesignsolo/strategic_ia.py` defines:

```
ActionIAObjectif            DescriptorIAObjectif
DescriptorIAObjectifDepots  DescriptorIAObjectifDefense
DescriptorIAObjectifAttaques / ActionIAObjectifAttaques
DescriptorUpgradeTechnoQuandNecessaire
DescriptorIAAll  DescriptorIARemoveAllObjectives
DescriptorIgnoreCampsList  ModifieParametresIA  TransfertUnitsToIA
```

`DescriptorIAObjectifDepots.__slots__` is the clearest statement of what an
objective *is*:

```python
('DallesInterdites', 'Monitor', 'MinPeriod', 'LaunchCount',
 'IncrementedOnLaunch', 'LimitArmyValueMin', 'LimitArmyValueMax',
 'ProductionEnabled', 'ProductionBatimentEnabled',
 'ProductionTechnoEnabled', 'DisableProductionAfter')
```

and `DescriptorIAObjectif.create_objectif(self, stratege)` names the owner: a
**stratège**. The internal modules referenced are `_stratege`, `_attaques`,
`_objectif`, `_production_request`, `_flag`, `_constantes`.

The tuning table supplies its numbers:

| Constant | Value |
|---|---|
| `ArmyValueForceLaunchAttack` | 600 |
| `MinArmyValueToUseManipulationCard` | 120 |
| `BP_HqValue` / `BP_DepotAddedValue` | 150 / 50 |
| `MaxProductionQueueSize` | 30 |
| `MaxWaitingRequest` | 30 |
| `NbMaxWaitingRequestBeforeRequestingNewFactory` | 8 |
| `VirtualFactoryQueueMaximumSlot` | 2 |
| `MaxBatimentAndTechnoProductionSimultaneous` | 2 |
| `MaxTimeWaitingRequest` / `CheckAndCancelWaitingRequest` | 60 / True |
| `SR_RefreshPositionEnnemi` | 60 |
| `RF_NbMetreMaxAParcourir` | 182000 |
| `RF_TypeRapportDeForceFeedback` / `…InIaStrat` | 1 / 1 |
| `MaximumBatimentProduction` | 2 |
| `DistanceMaxForProjection` | 260000 |
| `AverageOnPeriod` | 120 |

**[inferred] The architecture is a production economy with a queue, not a
behaviour tree.** Objectives do not command units; they raise *production
requests* against a virtual factory queue. When eight requests are backed up the
AI concludes it needs another factory (`NbMaxWaitingRequestBeforeRequestingNewFactory`),
and a request that waits sixty seconds is cancelled. Attacks are gated on an
army-value threshold rather than on a unit count.

**`RF_` is *rapport de force*** — balance of forces — and it appears in three
places: as a feedback type shown to the player
(`TGfxDescriptorPanelRapportDeForce`, `FRapportDeForce.cg`,
`FRapportDeForceAuSol.cg`, `StyleTargetRapportDeForce` with values
`Unit / Artillery / Reco / PadIdle`), as the AI's own evaluator
(`RF_TypeRapportDeForceInIaStrat`), and as an offline test harness
(`eugentest/leveldesigntest/rapports_de_force.py`, 10.5 KB).

**[inferred] The same force-ratio evaluator drives the player's "very easy →
high danger" attack indicator and the AI's decision to attack.** That is a
strong design position: the AI is not reading a hidden score, it is reading the
number the player is shown. It is also the concrete form of Le Dressay's
**[EUGEN]** claim about Steel Division 2 that *"a lot of players think the AI's
cheating. Of course it isn't cheating at all. It uses the same rules as the
player."* — the shared evaluator is why he can say that.

`SR_RefreshPositionEnnemi 60` and `tps_rafraichissement_infmap` say the AI's
world model is refreshed on a slow tick, which is CLAUDE.md's
"cull cheaply before testing expensively" applied in the time dimension and
`map_scale.md` §4.2's fifth lesson confirmed.

### 8.2 Time-sliced, in Python, and it survives being paused

**[BUILD]** `eugen/game/world.py` references `SlicedProcessing`,
`unsliced_ia`, `LDDetectorProcessing`, `NbIncreaseGameSpeedToDo`,
`decrease_game_speed`/`increase_game_speed`, `SynchroKeyManager` and
`tools.synchronisation`. `ActionIAObjectif` implements `__reduce__`,
`__getstate__` and `__setstate__`, and there is a `base/pickling.py`.

**[inferred] The AI's objective layer is pickled.** That is how a save game
captures an AI mid-plan, and it is why the objective layer is in Python at all —
the state is a small object graph the interpreter can serialise for free, where
the C++ side would need hand-written serialisation for every objective type. The
expensive parts (spotting, pathing, combat) stay in C++; only the slow-tick
deliberation lives up here.

`SlicedProcessing` against `unsliced_ia` says the work is spread across frames
with an explicit escape hatch — presumably for tests and for the strategic AI's
initial plan.

### 8.3 The retail build ships an AI regression suite

**[BUILD]** `IA_Common.dat` contains 82 marshalled Python modules, and among the
mission scripts are two directories of tests:

```
test\map\testia\scripting\maptests\
    testia  testattack  testattackciblemobile  testcapture  testcamp
    testmove  testpath  testproduction  teststrategicia  testcondition
    testobjectif  testtimers  testgamerules  testkillunits  testtechnos
    testavion  testcamera  testcutscenes  testfx  testalternatemode
    testnarrativescale  testmanipulation  testafficheinfo  testmisc
test\map\supercrossroads4\scripting_testia\maptests\
    testia  testattaque  testdefense  testdepot  testequivalence
    testmecaniqueia  testnationalites
```

plus a dedicated map, `testia`, whose terrain is 27 KB — the smallest in the
game — and `eugentest/testsuite.py` (12 KB), `leveldesigntest/testauto.py`,
`testsuites/tests/strategic_ia.py` with

```
TestMissionLaunched  TestAllMissionsRemoved
LaunchObjectifAttaque  LaunchObjectifHarcelement  StartAutoLaunch
TestDeviseIACorrect  CreateStrategicUnit  TestDistanceEntreBatiments
```

and `defines/testsuite.py` and `flat/ia/scenarioplayable`.

Decoding `testia.py`'s bytecode shows it building six AI camps — one per nation,
with `NiveauIA.Scripted`, `TDifficultes.Difficile`, `TProfils.ProfilStandard` and
`TProfils.ProfilTortue` ("turtle") — under `GameRulesSkirmish` with
`TPopCapConfig.NoPopCap` and `TGameTimerMode.Countdown`, then running 21 named
test descriptors against them.

**[inferred] This is the most transferable thing in the note and it costs 1.5
MB.** Eugen ship a flat, near-empty map whose only purpose is to be cheap, a
scripting layer expressive enough to set up six AIs and assert on the outcome,
and a suite that checks the AI still captures, still builds depots, still
launches attacks, still values its economy correctly, and — `testequivalence`,
`testnationalites` — that the seven factions remain comparable. An RTS AI is a
system where a regression is invisible until a player complains three patches
later; a headless test map with assertions is the only way to catch it, and the
suite is small enough to ship.

`generate_building_path.py` in the same pack suggests the building-path graph
(`TBatimentPaths`, `TPathGroupManager`) is generated by a Python tool rather than
authored.

---

## 9. Determinism, and the shape of the network

**[BUILD]** `eugen/game/world.py` imports `_network` and defines
`SynchroKeyManager`; `tools/synchronisation.py` exists; `micro_simu_replayer.py`
(8.8 KB) sits under `eugen/interface/`; save/load goes through
`base/pickling.py` and `eugen/game/loadsave.py`. The tuning table carries
`DureeDecompteLaunchGameMulti 10`, `NbLeague 7`, `NbLevelsPerLeague 10`,
`DefaultCote 1500`, `TempsEntreDeuxScan 30`, `NbScansAvantToleranceLeague 3`.

**[COMMUNITY]** Wargame players report desyncs in large team games and the
literature on the series describes lockstep command synchronisation.

**[inferred] Lockstep, on the evidence of a "synchro key manager", a micro-
simulation replayer, and a 200-unit cap.** Two hundred units is well inside what
lockstep handles comfortably, and a game whose whole design is per-player
divergent *belief* has a strong reason to want a single authoritative simulation
running identically everywhere: the visibility state is derived per observer from
one shared truth, rather than being state the server must filter and send.
Compare [`valve_networking.md`](../valve/valve_networking.md), where the entire
architecture exists to reconcile a client's view with a server's — Eugen's answer
is that there is nothing to reconcile because every machine runs the same
simulation and merely *displays* a different subset of it.

**[inferred]** The cost is the usual one, and R.U.S.E.'s 2026 re-release pays it
visibly: **[COMMUNITY]** the new build is documented as incompatible with the old
version's saves and replays, with a `compat-2` Steam branch kept to open them.
That is the lockstep tax [`valve_networking.md`](../valve/valve_networking.md) and
[`rainbow_six_siege.md`](../shooters/rainbow_six_siege.md) both name — change the simulation
in any way and every recorded input stream diverges.

---

## 10. What is worth taking

Ranked for this project.

### 10.1 Take: bake two representations rather than one with a wide LOD range

R.U.S.E. does not stream a single terrain and choose a mip. It bakes `highdef`
and `lowdef` as separate meshes with separate world bounds, and the strategic
view never touches the fine data. The cost is 25% more disk; the benefit is that
the coarse representation is small enough to stay resident, so *zooming out can
never stall*. Zooming out is exactly the motion during which a stall is most
visible, because the player is doing it to get information quickly.

The general rule: **when two viewing regimes differ by more than about 4× in
required detail, they are two assets, not two LODs of one.** A LOD chain shares a
budget and a residency policy; two bakes do not.

### 10.2 Take: split the spatial index by *purpose*, not just by contents

Three kd-trees per map — terrain-only, objects-only, camera — is the finding
worth internalising. They differ in size by 200×, they answer different
questions, and each is allowed to be wrong in a different direction. The camera
tree extends 175 m past the map and is smooth; the object tree is 556 triangles
of hand-chosen blockers and ignores every tree and house you can see.

This codebase already has the shape (`OcclusionGrid` derived from `Tile`, versus
`RayCaster` against the full lattice). The lesson to add is that **"what should
this query be allowed to get wrong" is a better axis to split on than "what
objects are in it"** — and that a purpose-built structure can be two orders of
magnitude cheaper than the general one.

### 10.3 Take: the AI regression map

§8.3. A flat, minimal map plus scripted setup plus assertions, shipped in the
retail build. This project has `xcom_tests` and `xcom_perf` and nothing that
checks the AI still behaves. The cost is a scripting hook and a bare map; the
bug class it catches — an AI that quietly stops doing one of the six things it
used to do — is otherwise found by a player.

### 10.4 Take: hysteresis on re-decided orders, with the caveat

`temps_minimum_de_maintien_d_ordre_*` is the same defence CLAUDE.md's incumbency
rule describes, implemented as a time floor rather than a score bias. Worth
knowing both forms exist: the **bias** lets a decisively better option win
immediately and only breaks ties; the **time floor** guarantees stability but
makes the unit ignore a genuine emergency for the duration. For target selection
and cover scoring, the bias is right. For "which building am I walking to", a
floor may be better, because thrashing there is worse than a second of delay.

### 10.5 Take, with care: zones as meshes rather than cells

R.U.S.E.'s territorial partition is a 686-triangle mesh in a kd-tree, and it
costs nine kilobytes. That is the right answer *for a game whose territory
follows terrain*. This project's tile lattice is the right answer for a game
whose territory is tiles. The transferable part is narrower and it is about
authoring: **when a designer must draw a region, let them draw the region.** If
we ever want named areas — ability zones, LOS volumes, mission triggers — a
triangulated polygon indexed by the structure we already have beats a bitmask
over the lattice, and it does not force the designer to see the grid.

### 10.6 Take: `TOPO`

§2.2. Ship the topological construction order as a separate table from the
storage order, and the loader never patches a pointer. Cheap, and it decouples
the writer's convenience from the reader's constraint.

### 10.7 Do not take: the Python layer

It buys free serialisation of AI plans (§8.2) and a scripting surface for the
test suite (§8.3). It costs an embedded interpreter, a second language in the
build, and a marshalling format that pins you to one Python version — R.U.S.E.
is welded to 2.5. For a project this size, the same benefits come from a small
data-driven objective format plus the existing test harness.

### 10.8 Do not take: 214 constants in one object

`TTunableConstante` is a single object with 214 properties covering combat
ranges, HUD alpha values, faction colours, joystick dead-zone timings and league
tables. It is a *global*. The unit inconsistency in §7.1 — two constants stated
in metres in a table otherwise stated in millimetres — is what that shape
produces: nobody owns the table, so nobody notices.

---

## 11. What changed by Wargame and WARNO

R.U.S.E. is the first game on this engine, not the last. Two later builds are
readable on this machine and they turn every "is this still how they do it?"
into a check rather than a guess.

> **[BUILD-WRD]** *Wargame: Red Dragon* (2014), `Data\WarGame\PC\510064564\NDF_Win.dat`
> — an `edat` **version 2** archive, 2,640 NDF files, all of which parse.
> Reading it is what found the version-2 bug in `ruse_edat.py`: the two versions
> disagree on which name-length parity gets the alignment filler, and the wrong
> rule desyncs the dictionary walk a few entries in rather than failing, so the
> archive reads as 33 files with garbage sizes instead of 2,640. Fixed and
> regression-checked against R.U.S.E.
>
> **[BUILD-WARNO]** *WARNO* (2022). The game itself is uninstalled here, but a
> **mod workspace survives** — and Eugen's mod pipeline exports the gameplay
> database as **plain-text NDF source**: 1,335 `.ndf` files, 39 MB, comments
> intact. That is a better source than the binary.
>
> **One caveat governs every WARNO comparison below.** The mod workspace exposes
> the *gameplay* half only. Engine-side classes — the renderer, the streaming,
> the spatial index — do not appear in it at all, so **absence from the WARNO
> column is not evidence of removal**. Where that matters it is said again.

### 11.1 The vocabulary is remarkably stable

**[BUILD]** Comparing class-name sets:

| | classes | |
|---|---|---|
| R.U.S.E. 2010 | 1,091 | |
| Wargame: Red Dragon 2014 | 1,321 | **652 of R.U.S.E.'s appear verbatim — 60%** |
| WARNO 2022 (gameplay subset) | 670 | 37 classes are present in all three |

The 37 that span twelve years and three games include `TTunableConstante`,
`THelperVisibility`, `TVisibilityRange`, `TVisibilityRangeContainer`,
`TArmorDescriptor`, `TFormationConstantes`, `TWorldConstantes`,
`TPathAndArrowSizeMultiplierHelper` and `TAcknowUnitContainerDescriptor`.

Namespaces persist too. R.U.S.E.'s gameplay database lives at
`genglad\patchable\gfx\everything.cpp`; WARNO's units reference their weapons as
`$/GFX/Everything/WeaponDescriptor_2K12_KUB_DDR`. **The path became a namespace
and kept its name.** Individual constants survive with their values —
`EliminationWarningsDuration = 5.0` and `FumigeneAlphaGhost = 0.3` are
byte-identical across twelve years.

### 11.2 The kd-tree survived — §5's answer is not a R.U.S.E. quirk

**[BUILD-WRD]** All three of `TMeshKdTreeBuildParams`,
`TStreamedMeshKdTreeLoader` and `TWorldFloorStreamedMeshKdTree` are present in
Red Dragon, unchanged.

**[inferred]** That matters more than it looks. §5 argued that Eugen answer
geometric queries with a streamed kd-tree over triangles rather than a grid; the
obvious objection is that R.U.S.E. is a small-map game and the technique would
not survive contact with 150 km². It did. The structure that indexes a 1.3 km
R.U.S.E. map is the structure that indexes a Wargame one, and the streaming and
subtree-paging in its property list is presumably why. (WARNO's mod source
contains none of these classes — but it contains no engine classes at all, so
that is silence, not evidence.)

### 11.3 The biggest change: terrain became a typed gameplay object

This is the one to read if you read only one.

**[BUILD]** In R.U.S.E., "forest" is a number in the global constant table:

```
CouvertBonusForet                          78000
DistanceIgnoreForet                        26000
RatioMultiplicateurPinnedProtectionForet   0.6
EmbuscadeAutomatiqueEnForet                True
```

**[BUILD-WRD]** Red Dragon introduces the class `TGameplayTerrain`.
**[BUILD-WARNO]** WARNO ships **22 instances** of it: `ForetDense`, `ForetLegere`,
`PetitBatiment`, `Batiment`, `Ruin`, `Tranchee`, `Barbeles`, `NidMitrailleuse`,
`Bloqueur`, `Urbain`, `Rocher`, `EauPeuProfonde`, `EauProfonde`,
`BloqueConstruction`, `SmokeLight`, `SmokeMedium`, `SmokeHeavy`, and four
`Strategic*` types for the campaign layer. Verbatim:

```
export ForetDense is TGameplayTerrain
(
    BloqueVision = true                 BloqueVehicule = true
    BloqueInfanterie = false            BloqueAmphibie = true
    HeightInMeters = 11                 StealthBonus = 6
    DissimulationModifierGroundGround = 24
    DissimulationModifierGroundAir = 20
    SpeedModifierInfantry = 1           SpeedModifierTrack = 0
    SpeedModifierAllTerrainWheel = 0    SpeedModifierBoats = 0
    InflammabilityProbability = 0.8     CriticalEffectProbability = 0.05
    DamageModifierPerFamilyAndResistance =
        MAP [ ("he", MAP [("infanterie",0.6)]),
              ("fmballe", MAP [("infanterie",0.5)]),
              ("balledca", MAP [("infanterie",0.5)]) ]
    DissimulationSliceSizeInGameUnits = ~/DissimulationUnitSlice
    TerrainType = ~/ETerrainType/ForetDense
    AuthorizeNearGroundFlying = false
)
```

**[inferred] Four things follow, and none of them is "more numbers".**

**`BloqueVision` is a per-terrain boolean**, so terrain participates in line of
sight as a *typed layer* rather than only as geometry. Dense forest blocks;
light forest (`BloqueVision = false`) does not, despite being 20 m tall against
dense forest's 11. That distinction cannot be expressed by tracing against a
mesh — it is a gameplay statement about a material.

**Concealment is directional**: `DissimulationModifierGroundGround = 24` against
`GroundAir = 20`. A wood hides you better from a tank than from an aircraft, and
that is one subtraction, not a second system.

**Smoke is a terrain type.** Three densities, sitting in the same table as
forests and buildings. Whatever mechanism reads terrain for concealment,
movement and LOS gets smoke for free — a genuinely elegant reuse, and the reason
the terrain layer has to be dynamic.

**Damage is modified per ammunition family per target type**, in the terrain
object. R.U.S.E. had one forest multiplier for pinning; WARNO has a matrix, and
it lives with the terrain rather than with the weapon.

`DissimulationSliceSizeInGameUnits` (globally `DissimulationUnitSlice is 1.0`)
**[inferred]** says concealment is accumulated in slices along the observer's
ray — so the modern system integrates cover *along* a line rather than testing a
single blocker, which is what lets a thin treeline and a deep wood differ.

### 11.4 Detection went from a distance test to a dice roll

**[BUILD]** R.U.S.E.: `DistanceMinimumVision`, `DistanceMinimumVisionBatimentReco`,
and a forest bonus. A comparison.

**[BUILD-WRD]** Red Dragon adds `TVisibilityModuleDescriptor`,
`TScannerConfigurationDescriptor`, `TModernWarfareScannerModuleDescriptor` and
— the change of kind — **`TModernWarfareVisibilityRollRule`** and its descriptor.
("ModernWarfare" is Eugen's internal name for the Wargame line; a whole parallel
class family carries the prefix, including a second `TModernWarfareTunableConstante`.)

**[BUILD-WARNO]** A shipped scanner, verbatim:

```
TScannerConfigurationDescriptor
(
    PorteeVision          = 10000 * Metre      DetectionTBA = 14000 * Metre
    PorteeVisionTBA       = 0 * Metre          PorteeVisionFOW = 0 * Metre
    PorteeIdentification  = 0.0
    OpticalStrength       = 63.675             OpticalStrengthAltitude = 250
    UnitDetectStealthUnit = False
    SpecializedDetections = MAP [ (EVisionUnitType/AlwaysInHighAltitude, 30000.0 * Metre) ]
    SpecializedOpticalStrengths = MAP []       IgnoreObstacles = MAP []
)
```

alongside `TVisibilityModuleDescriptor` carrying `AutoRevealType`,
`UnitConcealmentBonus`, `VisionUnitType` and `UnitIsStealth`, and a global
`PorteeDeVisionGlobale = 4000.0 * Metre`.

**[inferred] Being seen is now a roll, not a threshold**, and the roll machinery
is shared with combat. WARNO's `HitRollConstants.ndf` defines `ERoll/Hit`,
`ERoll/Pierce` and `ERoll/Critic` over a common `TDiceParameters` of
`DiceCount / DiceType / RollSuccessThreshold`, with the rule stated in a comment:
`Success if roll > RollSuccessThreshold - modifiersum`. Red Dragon's class list
adds `TModernWarfareHitRollRule`, `TModernWarfareTestMoralRollRule`,
`TModernWarfareVisibilityRollRule` and `TModernWarfareDistanceMultiplierRollRuleDescriptor`
— **hitting, holding your nerve, and being spotted are the same mechanism with
different modifier stacks.**

Two details in that file are worth having, because both are the kind of thing
only source shows. The dice comments all say `// 2d6` while the parameters say
`DiceCount = 1, DiceType = 100` — **a migration from 2d6 to d100 that did not
update its comments**. And `ERoll/Pierce` is `DiceCount = 1, DiceType = 1,
RollSuccessThreshold = 1`: a degenerate roll that always succeeds. Armour
penetration was made deterministic again while keeping the dice plumbing in
place, which is the cheap way to reverse a design decision you have already
built for.

### 11.5 The AI grew a memory, and a strategy machine above the objectives

**[BUILD]** R.U.S.E.'s AI knows where the enemy is because it re-reads the world:
`SR_RefreshPositionEnnemi = 60`. One refresh interval, no notion of stale belief.

**[BUILD-WARNO]** `IAStratVisionParameters.ndf`:

```
export IAStratVisionShootDetectionInfiniteMemory is TIAStratVisionParameters
(
    UnitsKnowledge = ~/EUnitsKnowledge/Seen | ~/EUnitsKnowledge/HasShot
    KnowledgeMemoTimer = 450   // once spotted, remembered for this many seconds (0 = infinite)
    KnowledgeTimerForAttackBehavior = 60
)
```

with three presets at 210 / 320 / 450 seconds, and a `NoShootDetection` variant
whose `UnitsKnowledge` omits `HasShot` entirely.

**[inferred] That is a belief model, and difficulty is expressed in it.** The AI
distinguishes *I saw it* from *I heard it fire*, forgets on a timer, and an
easier opponent is one that simply cannot learn from muzzle flashes — a far
better difficulty lever than the damage multipliers R.U.S.E. used
(`MultiplicateurDegatInfligeParLeJoueur_Facile` and friends), because it changes
what the AI *knows* rather than what the numbers do, and so stays invisible to
the player as cheating.

Above the objectives, Red Dragon adds `TIAGeneralStrategy` with an explicit
`TIAGeneralStrategyTransition`, plus `TIAStrategicAction`, `TIASTrategicActionLaunch`,
`TIAMapConstantes` and `TIATacticalResolutionConstantes`; and every one of
WARNO's 840 units carries a `TIAStratModuleDescriptor`, so the strategic AI has a
per-unit component rather than reasoning over a roster from outside.

WARNO's `IAStratConstantes.ndf` adds one line worth stealing:

```
SeedForIAStrat = 0   // set to 0 for a changing seed
```

**[inferred]** A pinnable RNG seed for the strategic AI, which is what makes an
AI regression suite (§8.3) able to assert on outcomes rather than on ranges.

### 11.6 Influence maps became first-class — and the §5.4 split got *sharper*

**[BUILD]** R.U.S.E. hints at one: `tps_rafraichissement_infmap`, a refresh
interval in `eugen/game/world.py`, and nothing else.

**[BUILD-WARNO]** `InfluenceMap.ndf` ships two configured instances:

```
export InfluenceMapCstTactic is TInfluenceMapConstantesDescriptor
(
    TailleDeCase = 500 * Metre   // Attention impacte lourdement sur les performances
                                 // (à voir si il ne faudrait pas plutot faire un réglage sur
                                 //  la quantité de case par map, là on table sur 300x300)
    DefaultDecay = 0.001         DefaultDecayIsolated = 0.08
    DefaultMomentum = 0.3        MinValueInfluenceForOwnership = 0.4
)
export InfluenceMapCstStrategic is TInfluenceMapConstantesDescriptor
(
    TailleDeCase = 100 * Metre   DefaultDecay = 0.1  DefaultDecayIsolated = 0.1
)
```

plus `TInfluenceMapGeometryParameters` with `SmoothDistance` and
`EmptyCellDefaultOffset`, and per-unit `TZoneInfluenceMapModuleDescriptor` /
`TInfluenceScoutModuleDescriptor` on 105 of the 840 units.

**[inferred] This is the grid R.U.S.E. did not have, and its arrival confirms
§5.4 rather than contradicting §5.** Twelve years on, geometric queries still go
to the kd-tree and aggregate queries go to a regular grid — the division just
became explicit and tuned. `DefaultDecay` against `DefaultDecayIsolated` (0.001
against 0.08) is the good detail: an isolated cell forgets **eighty times faster**
than a connected one, so a lone scout's footprint evaporates while a held line
persists.

The comment is worth reading twice. It carries a performance warning
(*"heavily impacts performance"*), an admitted design smell (*"we should perhaps
tune cell **count** per map instead"* — i.e. they know a fixed cell size makes
cost scale with map area), and the actual working budget: **~300 × 300 cells**.
Shipped data doing the job of a design document.

### 11.7 The unit became a component list

**[BUILD]** R.U.S.E.: `TUniteDescriptor` with flat properties.
**[BUILD-WARNO]**: `TEntityDescriptor` with a `ModulesDescriptors` array. Across
840 units, **19 module types appear on every single one** —
`TVisibilityModuleDescriptor`, `TTagsModuleDescriptor`, `TFlagsModuleDescriptor`,
`TDamageModuleDescriptor`, `TBaseDamageModuleDescriptor`,
`TAutomaticBehaviorModuleDescriptor`, `TIAStratModuleDescriptor`,
`TOrderableModuleDescriptor`, `TExperienceModuleDescriptor`,
`TRoutModuleDescriptor`, `TStrategicDataModuleDescriptor` and the rest — with a
long tail: `TAutoCoverModuleDescriptor` (625), `TDangerousnessModuleDescriptor`
(618), `TFuelModuleDescriptor` (512), `TLinkToTrenchModuleDescriptor` (297),
`TZoneInfluenceMapModuleDescriptor` (105), `TSupplyModuleDescriptor` (29).

Three mechanisms are layered on top:

- **`TModuleSelector`** — `Default = $/GFX/Everything/WeaponDescriptor_2K12_KUB_DDR`,
  `Selection = [~/NilDescriptorIfCadavre]`. A module can be swapped or nulled at
  runtime by predicate; a wreck loses its weapon manager without a branch in the
  weapon code.
- **Tags and flags side by side** — `TagSet` of strings (`"AA_radar"`,
  `"AllowedForMissileRoE"`, `"GroundUnits"`) for queries and rules-of-engagement,
  `InitialFlagSet` of symbols (`Flag_Detectable`, `Flag_LdDetectable`) for the
  hot path.
- **`DescriptorId = GUID:{c4c83faa-…}`** — stable identity across renames.

**[inferred]** And `TAutoCoverModuleDescriptor` is R.U.S.E.'s
`EmbuscadeAutomatiqueEnForet = True` grown up: `AutoCoverRange = 350*Metre`,
`OccupationRadius = 100*Metre`, and a **`TerrainListMask`** bitmask over
`ETerrainType` — so the same behaviour is now parameterised by the terrain table
from §11.3 instead of hard-coding "forest". That is the pattern of the whole
evolution in miniature: a boolean about one special case becomes a mask over a
typed enumeration.

CLAUDE.md's rule that the entity layer stays ordinary OOP and does not become an
archetype ECS is worth reading against this. Eugen went to a module list and
stopped there — modules are heterogeneous descriptor objects in an array, not
archetype-packed columns.

### 11.8 The language grew up, and the god object was partly answered

**[BUILD-WARNO]** Modern NDF has things R.U.S.E.'s binary shows no sign of:

| | |
|---|---|
| templates | `template InfluenceMapGeometryParameters [DistanceDeLissage : float = 10000.0] is T… ( … )` — typed parameters with defaults |
| **unit literals** | `4000.0 * Metre`, `350*Metre` |
| maps | `MAP [ (key, value), … ]` |
| references | `~/LocalName`, `$/Absolute/Path/Name` |
| visibility | `private`, `export` |
| identity | `GUID:{…}` |
| enums as objects | `EVisionUnitType is TBaseClass ( AlwaysOnGround is 1 … )`, with `// Réservé` holes preserved |

**[inferred] The unit literal is the direct fix for §7.1's bug.** R.U.S.E.'s
constant table mixes millimetres and metres — `DistanceZoneAlerte 78` sitting
beside `CouvertBonusForet 78000` — because a raw number carries no unit. Once
`Metre` is a language-level multiplier the mistake is not available.

`TTunableConstante` itself survives by name into all three games, 214 properties
becoming 727 lines. But two things changed around it. It is now headed by a
shouted convention:

```
///!\ MERCI DE RESPECTER L'ORDRE ALPHABETIQUE !
// Par catégories puis par constante.
```

and it was **split** — Red Dragon carries a second `TModernWarfareTunableConstante`
beside the shared one, so per-game tuning no longer piles into the same object.
**[inferred]** §10.8's complaint is half-answered: they kept the god object,
imposed an ordering discipline on it in a comment loud enough to be seen in a
diff, and peeled the game-specific half off into its own type.

### 11.9 Deception was deleted, wholesale

**[BUILD-WRD]** Every bluff class is gone by Red Dragon: `TBluffCardDescriptor`,
`TBluffInfos`, `TBluffZoneManagerDescriptor`,
`TInGameCameraMoverForBluffZoneCardLaid`, `TInterfaceInGameBluffInfosResource`,
`TInterfaceInGameResourceBluffInfos`.

**[inferred]** Which is the counterweight to §6.3. Deception reached the camera,
material, HUD and acknowledgement systems, and when the design was dropped it
came out cleanly enough to leave no residue in the class table. Pervasive is not
the same as entangled — it was pervasive because a lot of systems *asked* the
bluff manager questions, not because bluff logic was scattered through them.

Also dropped: all five `TTerrainModifier*` classes from §3.5. **[inferred]**
Runtime heightmap modification gave way to the offline bake pipeline
(`LaunchMapBaking.bat` in WARNO's published map workflow) — the flattening now
happens when the map is built, not when it is loaded.

### 11.10 Three process artefacts worth copying

**[BUILD-WARNO]** Left in shipped data:

- `// A maintenir synchro avec Engine\Code\Eugen\CPP\EugIA_Common\InfluenceMapConstantes.h`
  and `// A maintenir synchro avec EugIA_Common/TVisibilityModuleDescriptor.h` —
  **the data file names the C++ header it must stay in step with**, by path. Any
  enum defined in two places should carry this line; ours do not.
- `// spec: WARGAMENORMANDIE-285` and a
  `https://confluence.eugennet.com/…?pageId=29364229` link — the tuning table
  points at the ticket and the wiki page that justify a value.
- `AutoCover_DistanceMaxEntreLesChecksDeTerrains = 60 * Metre //100 * Metre` —
  the previous value kept commented beside the current one, so the tuning history
  is legible where the value is.

### 11.11 The shape of twelve years

**[inferred]** The engine kept its spatial and rendering core and rebuilt its
gameplay layer twice. The kd-tree, the visibility-range containers, the tunable
constant object, the namespace layout and 60% of the class vocabulary carried
straight through. What changed is almost entirely one move, applied over and
over:

> **A special case became a typed table, and the behaviour became a mask over
> it.** Forest-the-constant became `TGameplayTerrain` × 22 and `AutoCover` reads
> a terrain mask. A distance threshold became a roll rule shared by hitting,
> morale and spotting. A refresh interval became a knowledge model with a source
> flag and a decay timer. A flat unit descriptor became a module list with
> selectors.

For this project the transferable version is narrower than "add tables". It is
that **the things Eugen had to generalise are the ones that started as a boolean
about one special case** — `EmbuscadeAutomatiqueEnForet`, `CouvertBonusForet`,
`SR_RefreshPositionEnnemi`. Each was correct, shipped, and cheap; each became a
system. When a gameplay constant is named after a specific piece of content, it
is a table that has not been written yet, and the cost of the delay is that
every caller has meanwhile been written to ask the wrong question.

---

## 12. Reproducing this

Three readers, written against the shipped data and kept in the repository.
Python 2.7 — `marshal` has to be Python 2's to read the game's code objects, so
all three stay on 2.7 rather than splitting the toolchain:

| File | What it does |
|---|---|
| [`../tools/ruse/ruse_edat.py`](../../../tools/ruse/ruse_edat.py) | `edat` v1/v2 archives — header, prefix-tree dictionary, entry extraction |
| [`../tools/ruse/ruse_ndf.py`](../../../tools/ruse/ruse_ndf.py) | `EUG0`/`CNDF` — section directory, the four string tables, object and property walk |
| [`../tools/ruse/ruse_python.py`](../../../tools/ruse/ruse_python.py) | `.xyz` — zlib, `marshal.loads`, then walk the bytecode pairing `LOAD_CONST` with the following `STORE_NAME` to recover module-level assignments |

```
python tools/ruse/ruse_edat.py   ".../R.U.S.E/Data/PC/190852/ZZ_GladNotPatchableWin.dat"
python tools/ruse/ruse_python.py eugen.ipk defines/front/bluff.xyz
```

Caveats worth stating, since they bound what above is quotable:

- **The NDF value decoder is incomplete.** The type table was recovered by
  inspection; several type codes (notably `0x14`, and some 8-byte packed handles)
  are unresolved, and when the decoder meets one it stops that object and resyncs
  on the next `0xABABABAB`. Every *value* quoted in this note came from a
  property that parsed cleanly in sequence from the start of its object. Class
  names, property names and string tables are read from `CLAS`/`PROP`/`STRG`
  directly and do not depend on the value decoder at all — which is why the
  vocabulary sections are the most reliable parts of this note.
- **The `.tms` chunk-grid reading is inferred**, from two uint32s at offset 16
  and a float at offset 28. It is corroborated across all 33 maps: chunk count ×
  327.68 m reproduces the independently-measured kd-tree bounding box in both
  axes every time (to within 1.92 m on the three maps whose terrain bbox is
  trimmed short). That is strong, not proof.
- **The millimetre unit is inferred**, on the three corroborations in §7.1.
- **Runtime behaviour is inferred throughout.** The kd-trees exist, are named
  `occlusioninfo`, and are split terrain/objects/camera; that they are queried
  per LOS test at runtime is a reading, not an observation. Nothing here comes
  from instrumenting the executable.
- **§11's WARNO column is the gameplay half only.** Eugen's mod pipeline exports
  gameplay NDF, not engine NDF, so a class missing from the WARNO set may simply
  be engine-side. Every comparison that turns on WARNO's *absence* says so.
- **The class-count comparison in §11.1 is not like-for-like.** R.U.S.E. (1,091)
  and Red Dragon (1,321) are complete object databases read from the binaries;
  WARNO's 670 is what appears in a mod workspace. The R.U.S.E. → Red Dragon
  figure of 652 shared classes is a real 60%; the WARNO figures are floors.

---

## Sources

**Read directly**
- The retail R.U.S.E. install, data version `190852` — `Data\PC\190852\*.dat`,
  `Maps\PC\DataMap*.dat`. All **[BUILD]** claims.
- The retail Wargame: Red Dragon install — `Data\WarGame\PC\510064564\NDF_Win.dat`,
  an `edat` v2 archive of 2,640 NDF files. All **[BUILD-WRD]** claims.
- A WARNO mod workspace — `Mods\<name>\{GameData,CommonData}\**\*.ndf`, 1,335
  files of Eugen's own NDF source with comments intact. All **[BUILD-WARNO]**
  claims. Produced by the `GenerateMod.bat` in the same folder; any WARNO install
  can make one.

**Format documentation that made the readers possible**
- [enohka/moddingSuite](https://github.com/enohka/moddingSuite) — `EdataManager.cs`
  and `ndfbin_reversing.txt`; the archive dictionary walk is transcribed from it.
  Credits Giovanni Condello's "WargameEE DAT unpacker" as its own origin.
- [mathieujaumain/edataFileManager](https://github.com/mathieujaumain/edataFileManager)
- [Ulibos/ndf-parse](https://github.com/Ulibos/ndf-parse) — the modern textual NDF

**Eugen's own words**
- [WARNO devblog: "A new 3D engine to set the world on fire!"](https://steamcommunity.com/games/1611600/announcements/detail/3136192817402117365) — deferred engine, PBR, terrain specular at distance
- [How Eugen Systems built the massive RTS Steel Division 2](https://gamesbeat.com/how-eugen-systems-built-the-massive-rts-steel-division-2/) — Alexis Le Dressay on scalability, the tactical/operational AI split, and the AI using the player's rules
- [Wargame's map: Three Miles Island — designer's diary](https://eugensystems.com/wargames-map-three-miles-island-designers-diary/) — maps are built from square tiles, 16 against 9
- [Eugen Systems modding manual (Steel Division)](https://eugensystems.com/wp-content/uploads/2017/05/Modding_Manual_SteelDivision_Update.pdf)

**Later games in the same engine, for continuity**
- [WARNO — the complete map modding guide](https://steamcommunity.com/sharedfiles/filedetails/?id=3352892580) — `HeightMap.png` 16-bit, `Splat_map.png`, **`Div_map.png`**, `sdb.png`, navigation layers, `LaunchMapBaking.bat`
- [dreamfarer/WARNO-DATA wiki](https://github.com/dreamfarer/WARNO-DATA/wiki/Data-Dictionary) — `TMountedWeaponDescriptor`, `OpticalStrength`, `DamageResistance.ndf`
- [WARNO-DATA in-depth guide](https://github.com/dreamfarer/WARNO-DATA/wiki/In-Depth-Guide) — the armour × AP table and the accuracy roll

**Background**
- [R.U.S.E. (Wikipedia)](https://en.wikipedia.org/wiki/R.U.S.E.) — factions, ruse categories, IrisZoom, the 2026 re-release
- [IRISZOOM (ModDB)](https://www.moddb.com/engines/iriszoom)
- [R.U.S.E. re-release changes and modding notes](https://steamcommunity.com/app/21970/discussions/0/844005093415120012/) — the `compat-2` branch for old saves and replays
- [Secrets of R.U.S.E. — in-depth statistics](https://steamcommunity.com/sharedfiles/filedetails/?id=438883723) — research times, bomber counts, building resistance

**Related notes**
- [`map_scale.md`](../../topics/scale/map_scale.md) — the earlier outside-in note; §1.2 here corrects its scale figures for R.U.S.E.
- [`terrain_rendering.md`](../../topics/world/terrain_rendering.md) — anti-tiling, of which the diversity map is the first layer
- [`world_streaming.md`](../../topics/world/world_streaming.md) — Unreal's single-representation streaming, and why it needs the mip/LOD clamp R.U.S.E. avoids
- [`spatial_queries.md`](../../topics/agents/spatial_queries.md) — raycasts as the dominant AI cost, and building a cheap surface to trace against
- [`lod_systems.md`](../../topics/world/lod_systems.md) — the four meanings of LOD; R.U.S.E. uses geometry, aggregation and deformation
- [`valve_networking.md`](../valve/valve_networking.md) — the authoritative-server alternative to lockstep
- [`motion_matching.md`](../../topics/animation/motion_matching.md) §8.2 — hysteresis as a bias, against Eugen's time floor
