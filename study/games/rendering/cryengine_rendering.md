# CRYENGINE rendering — reference notes

How Crytek's engine draws what it draws, read for the same reason as
[`re_engine_rendering.md`](re_engine_rendering.md) and
[`frostbite_rendering.md`](frostbite_rendering.md): the transferable part is
architectural.

This one is different from those two in a way that has to be stated before
anything else. **Frostbite and RE ENGINE are documented by talks. CRYENGINE is
documented by its own source code, which we have.** Crytek's conference output
effectively stopped in 2014, and every "CryEngine rendering" article you will
find is a summary of a Crysis-3-era deck. But the C++ engine and the entire
shader library are readable, and they are ten years newer than the decks. So the
weight here sits on the source, and the talks are used for the *reasoning* the
source does not record — why deferred and not Forward+, why the GBuffer is
packed the way it is.

| tag | source |
|---|---|
| **[SRC57]** | **CRYENGINE 5.7 LTS shader library** — 145 files (`.cfx` HLSL shaders, `.cfi` includes, `.ext` material-feature manifests) plus 19 `sys_spec_*` quality-dial configs, read from the [CRYENGINE Community Edition](https://github.com/Pterosoft/-CRYENGINE-Community-Edition-) redistribution, which ships the whole `Engine/Shaders` tree in plain text. This is the **newest first-party rendering source that exists publicly**, stamped `Copyright 2004-2021 Crytek GmbH`. |
| **[SRC567]** | **CRYENGINE 5.6.7 C++ engine source** — 1,275 files across `RenderDll` (renderer + all 53 graphics-pipeline stages), `Cry3DEngine` (terrain, vegetation, SVO GI, LOD, decals, ocean), `CryAnimation` and `CryNetwork`. The last full public tree; see §0.2 on why this version. |
| **[CFG]** | The shipped `sys_spec_*.cfg` CVar groups — Crytek's own quality dials, which say what they consider expendable at low spec. Part of [SRC57]. |
| **[GDC14]** | GDC 2014 — *Moving to the Next Generation: The Rendering Technology of Ryse*, **Nicolas Schulz**, Crytek. The single most load-bearing talk here: it is where deferred-vs-Forward+, the GBuffer packing and the PBS transition are argued. |
| **[SIG14-GC]** | SIGGRAPH 2014 — *Real-Time Geometry Caches*, **Axel Gneiting**, Crytek. Slides *plus speaker notes*, which is where the compression numbers are. Answers the "do they use VATs" question. |
| **[SIG13]** | SIGGRAPH 2013 Advances — *CryENGINE 3 Graphics Gems*, **Tiago Sousa**. |
| **[SIG13-SH]** | SIGGRAPH 2013 course — *Playing with Real-Time Shadows*, Sousa / Andreev / Kasyan. |
| **[C3]** | GDC 2013 — *Rendering Technologies of Crysis 3*, Tiago Sousa. |
| **[GDCE13]** | GDC Europe 2013 — *Shining the Light on Crysis 3*, Pierre-Yves Donzallaz. |
| **[SIG11]** | SIGGRAPH 2011 — *Spherical Skinning with Dual Quaternions and QTangents*, Ivo Zoltan Frey, Crytek. Cited by [SIG14-GC] and still live in the shipped vertex format. |
| **[CE]** | Crytek's own news posts and documentation on cryengine.com. |
| **[3P]** | Third-party technical analysis (Digital Foundry) and developer interviews. Observation of output, or a paraphrase of an engineer — never a claim about implementation. |
| **[inferred]** | Our reading, not Crytek's. |

---

## 0. Method, and the version problem

### 0.1 "The latest CRYENGINE" is three different things

This is the first thing to get straight, because the answer to "what does the
latest CryEngine do" depends entirely on which of these you mean.

| | what it is | date | available to us |
|---|---|---|---|
| **5.7.1 LTS** | The last **public** engine release. Source moved to a private, request-gated repository the same month. | **19 May 2022** [CE] | Shaders: **yes**, complete [SRC57]. C++: no. |
| **5.6.7** | The last public release whose **full C++ tree** was on GitHub before the 2022 takedown. | 2020 | **Yes**, complete [SRC567] |
| **5.11** | The **shipped** engine. Runs *Hunt: Showdown 1896*. Never released publicly, never documented, no source. | **15 Aug 2024** [3P] | No — interviews only |
| **Community Edition 1.0** | A **community** patch on top of 5.7 LTS, MIT-licensed, by Pterosoft. Not Crytek. | **3 Oct 2025** | Yes |
| **"a new iteration"** | Crytek have said 5.7 is the last CRYENGINE V and that a successor is in development. No name, no features, no date. | — | No |

**Crytek's own news archive has no engine release post after 19 May 2022.** The
most recent engine-related item of any kind is a documentation migration in
September 2024. As a *public platform* CRYENGINE is frozen; as an *internal
engine* it plainly is not, because Hunt shipped on 5.11 two years after 5.7.

So: **this document describes 5.6.7/5.7 in detail and flags the 5.11 delta
separately in §20.** Where a section says "CryEngine does X", it means the
version we can read. That is a real limitation and it is not hidden anywhere
below.

### 0.2 Why 5.6.7 for the C++ and 5.7 for the shaders

They are not the same version, and the mismatch is deliberate rather than
sloppy. The 5.7 shader tree is redistributable and is what Community Edition
ships; the 5.7 C++ tree is not, and Crytek gate it behind an account request.
5.6.7 is the last C++ tree that was ever public.

The gap between them is small and knowable. 5.7's headline changes were
Scaleform 4, the GamePlatform plugin, VS2022/C++17 support and bug fixes [CE] —
**no renderer architecture changes were announced**. Every shader in [SRC57]
that has a matching stage in [SRC567] agrees with it: same constant-buffer
layouts, same technique names, same `TILED_SHADING_TILE_SIZE_X 8`. Where the two
disagree the shader wins and it is marked.

### 0.3 What was not done

**No frame capture was taken.** *Hunt: Showdown* ships EasyAntiCheat, and
injecting a graphics debugger into an anti-cheat-guarded process is the exact
behaviour such a system exists to detect. The same call as
[`frostbite_rendering.md`](frostbite_rendering.md) §0.1, for the same reason.
Nothing here carries a measured millisecond from a current build. Every cost
figure below belongs to the talk it came from, on the hardware that talk was
about — mostly Xbox One and PS4 in 2013–2014 — and is recorded as history, not
as a description of anything shipping now.

**The consequence, stated plainly:** this document is strong on *structure* —
what the passes are, in what order, at what resolution, in what format, with
what defaults — and weak on *cost*. Structure is what transfers anyway.

---

## 1. The frame, in order

This is the single most valuable thing in the document, and it is a direct read
of `CStandardGraphicsPipeline::Execute()` [SRC567]. Not a reconstruction from a
capture, not a diagram from a talk — the actual dispatch order, with the actual
conditionals.

CryEngine 5 defines **37 stage types** in `EGraphicsPipelineStage`, each a
`CGraphicsPipelineStage` subclass with `Init` / `Update` / `Execute`. The
standard pipeline registers **36** of them (the exception is
`MobileComposition`), and there are **six whole pipelines** that compose the same
stage pool differently — `Standard`, `Minimum`, `Mobile`, `Billboard`,
`CharacterTool`, `Debugger`. The billboard impostor baker (§14.4) is a graphics
pipeline in its own right, which is a nice piece of design: baking impostors is
"render the scene with a different stage list", not a special-case tool path.

```
GRAPHICS_PIPELINE
├── VolumetricClouds::ExecuteShadowGen        cloud shadows, before shadow masks
├── ComputeSkinning::Execute                  compute-shader skinning (async capable)
├── ComputeParticles::PreDraw
├── Rain::ExecuteRainOcclusion                rain occlusion map
│
├── SceneGBuffer::Execute                     ← 3× RGBA8 + depth-stencil (§2.2)
├── ShadowMap::Execute                        5 pass types (§4)
├── DeferredDecals::Execute                   ← writes into the GBuffer (§11)
├── Rain::ExecuteDeferredRainGBuffer          GBuffer modifiers: wetness…
├── Snow::ExecuteDeferredSnowGBuffer          …and snow, both edit the GBuffer
├── VolumetricClouds::ExecuteShadowGen
│
├── SVOGI                                     voxel GI cone tracing (§5)
├── ScreenSpaceReflections::Execute           half-res by default (§6.1)
├── HeightMapAO::Execute                      large-scale AO from a heightmap (§7.2)
├── ScreenSpaceObscurance::Execute            SSDO (§7.1)
├── TiledLightVolumes::Execute                light culling into 8×8 tiles (§2.1)
├── Water::ExecuteWaterVolumeCaustics
│
├── DEFERRED_LIGHTING
│   ├── ClipVolumes::Generate/Prepare/Execute portal & vis-area stencil IDs
│   ├── ShadowMask::Prepare/Execute           shadows resolved to a mask texture
│   ├── TiledShading::Execute                 ← one compute pass, all lights (§2.1)
│   └── ScreenSpaceSSS::Execute               separable SSS blur (§8)
│
├── FORWARD Z
│   ├── SceneForward::ExecuteOpaque           forward materials: hair, eyes
│   └── Sky::Execute
│
├── Water::ExecuteDeferredOceanCaustics
├── VolumetricFog::Execute                    froxel volume, 1/10 res × 32 (§9.1)
├── Fog::Execute                              analytic height fog
├── VolumetricClouds::Execute                 (§9.2)
├── Water::ExecuteWaterFogVolumeBeforeTransparent
│
├── FORWARD T                                 ← the water sandwich
│   ├── SceneForward::ExecuteTransparentBelowWater
│   ├── Water::Execute                        ocean + water volumes
│   ├── SceneForward::ExecuteTransparentAboveWater
│   ├── SceneForward::ExecuteTransparentDepthFixup
│   └── SceneForward::ExecuteTransparentLoRes  half-res particles, 1 or 2 passes
│
├── Snow::ExecuteDeferredSnowDisplacement
├── POST_EFFECTS_HDR
│   ├── Rain::Execute                         screen-space rain streaks
│   ├── FrameToFrame copy                     (before motion blur, avoids a copy)
│   ├── DepthOfField::Execute
│   ├── MotionBlur::Execute
│   ├── Snow::Execute
│   ├── ½-res downsample → ¼-res downsample   shared by the next three
│   ├── AutoExposure::Execute
│   ├── Bloom::Execute
│   ├── LensOptics::Execute                   physical lens-flare system
│   ├── SunShafts::Execute
│   ├── ColorGrading::Execute
│   └── ToneMapping::Execute                  HDRTarget → DisplayTarget
│
├── SceneForward::ExecuteAfterPostProcessHDR
├── PostEffect::Execute                       includes PostAA (§10)
├── SceneForward::ExecuteAfterPostProcessLDR
└── OmniCamera::Execute                       360° capture
```

Four things are worth pulling out of that.

**Decals run after the GBuffer and write into it.** Not a forward pass, not a
separate buffer — `DeferredDecals` re-renders into the same three targets, which
is why the specular chrominance encoding in §2.2 exists at all. See §11.

**Rain and snow are GBuffer modifiers.** `ExecuteDeferredRainGBuffer` edits the
albedo, normals and roughness of everything already drawn, before any lighting.
Wetness is not a material property in CryEngine; it is a post-pass over the
GBuffer. That is a genuinely good idea and it generalises — it is the same trick
as a decal, applied to the whole screen with a mask.

**Shadows are resolved to a mask before lighting.** `ShadowMask::Execute` writes
a screen-space texture of "how lit is this pixel by each shadow-casting light",
and `TiledShading` reads that rather than sampling shadow maps itself. This is
what keeps the tiled shading compute kernel's register pressure down — see the
warning comment quoted in §2.1.

**Transparency is sorted around the water plane.** Below-water transparents,
then the water surface, then above-water transparents. Explicit, three passes,
no per-object sorting cleverness. For a game with an ocean this is the cheap
correct answer and it is worth remembering.

---

## 2. Lighting

### 2.1 Tiled deferred shading — 8×8 tiles, 255 lights, one compute pass

CryEngine 5's opaque lighting is **tiled deferred**, not clustered — though the
engine does contain a real 3D clustered light grid, used only by the volumetric
fog (§9.1). The tile numbers are hard-coded and asserted in both the C++ and the
HLSL so they cannot drift apart:

```cpp
// TiledLightVolumes.cpp [SRC567]
constexpr uint32 MaxNumTileLights = 255;
const uint32 LightTileSizeX = 8;
const uint32 LightTileSizeY = 8;
static_assert(MaxNumTileLights <= 256 && LightTileSizeX == 8 && LightTileSizeY == 8,
              "Volumes don't support other settings");
```

```hlsl
// TiledShading.cfi [SRC57]
#define TILED_SHADING_MAX_NUM_LIGHTS  255
#define TILED_SHADING_TILE_SIZE_X       8
#define TILED_SHADING_TILE_SIZE_Y       8
#define TILED_THREAD_GROUP_SIZE  TILED_SHADING_TILE_SIZE_X * TILED_SHADING_TILE_SIZE_Y
```

Dispatch is `ceil(width/8) × ceil(height/8)`, one thread group per tile, 64
threads per group — one thread per pixel. **The light limit is 255 per frame,
not per tile**, which is a meaningful architectural difference from a clustered
system: the light list is a global array and tiles index into it with a bitmask
(`Fwd_TileLightMask`, a `Buffer<uint>`).

Culling volumes are analytic, four types:

```hlsl
#define TILEDLIGHT_VOLUME_SPHERE   1
#define TILEDLIGHT_VOLUME_CONE     2
#define TILEDLIGHT_VOLUME_OBB      3
#define TILEDLIGHT_VOLUME_SUN      4
```

The OBB type matters — it is how **environment probes** are culled, because a
CryEngine probe is a box-projected cubemap with an oriented bounding box, not a
sphere. Probes go through the *same* tiled light list as analytic lights, which
is why the tile shade info carries `Fwd_SpecCubeArray` and `Fwd_DiffuseCubeArray`
as `TextureCubeArray` bindings.

The file opens with a comment that is worth reproducing in full, because it is
the whole performance story of tiled deferred in five lines:

> ```
> // Wave occupancy is essential to get a performance benefit from tiled deferred shading.
> // Branch heavy code can increase the number of VGPRs that are required to execute
> // the shader which in turn will lower the wave occupancy.
> // When modifying the shader code, please check in PIX on Xbox One that the occupancy is
> // at least 3 for the tiled deferred shading pass.
> ```
> — `TiledShading.cfi` [SRC57]

**Occupancy ≥ 3, verified in PIX, as a maintenance rule written into the shader.**
That is the discipline that makes a monolithic lighting kernel work, and it is
also the reason shadows are resolved to a mask beforehand (§1) rather than
sampled inline: every branch you keep out of that kernel buys registers.

There is a `r_DeferredShadingTiled` mode selector with a `eDeferredMode_1Pass`
threshold, and asynchronous compute is available but **off by default**:

```
r_D3D12AsynchronousCompute  0 = Off, +1 = GPU-Skinning, +2 = GPU-Particles, +4 = Tiled-Shading …
```

### 2.2 The GBuffer — three RGBA8 targets, and every byte fought over

From [GDC14], and unchanged in structure through [SRC567]:

| target | RGB | A |
|---|---|---|
| **RT0** RGBA8 | Normals XYZ (best-fit normals) | Translucency luminance **or** prebaked AO |
| **RT1** RGBA8 | Diffuse albedo | Subsurface scattering **profile ID** |
| **RT2** RGBA8 | R: roughness | GBA: specular **YCbCr** *or* transmittance CbCr |

Plus the device depth-stencil surface. That is **12 bytes per pixel**, which by
2014 standards was tight and by current standards is very tight indeed.

Three decisions in there are worth stealing:

**Best-fit normals** [KAPLANYAN10] rather than octahedral or spherical — a lookup
texture that picks, for a given normal, the scaling that minimises quantisation
error in 8 bits. CryEngine was the origin of that technique and still uses it.

**Specular colour stored as YCbCr.** [GDC14] is explicit about why, and it is
entirely about decals:

> "Specular color stored as YCbCr to better support blending to GBuffer (e.g.
> decals) — Allow blending of non-metal decals despite not being able to write
> alpha during blend ops"

A decal wants to blend albedo and normal and roughness into an existing GBuffer.
Blend ops cannot write alpha independently, so putting specular *luminance* in
the same channel as roughness and *chrominance* in two others lets a decal blend
the parts it wants. This is a format chosen for the *writer*, not the reader.

**Aliasing two mutually exclusive fields.** Specular chrominance and transmittance
chrominance share bytes, justified as: "colored specular just for metal,
translucency just for dielectrics." A material cannot be both, so the bits are
reused. Cheap, and it needs no extra bit to signal which, because metalness is
already implied by the values.

### 2.3 Deferred versus Forward+ — why Crytek went deferred, and what that cost

Directly relevant to [`clustered_forward_lighting.md`](../../plans/clustered_forward_lighting.md),
so it is quoted rather than summarised. [GDC14], slide "Forward+ versus Deferred":

> - Considered Forward+ at the beginning, in combination with MSAA
> - Many open challenges in practice
>   - How to handle surface modifiers like decals or wetness efficiently
>   - Requires two rendering passes again for efficient light culling
>   - Most research so far considered just simple light models (mostly point lights)
>   - Many more different light types used in practice (projectors, shadow casting
>     lights, area lights, environment probes, etc.)
>   - Potentially low wave occupancy due to number of GPRs required for branching
>     when using complex light models
>   - Potential overshading/performance waste due to quad occupancy of tiny triangles
> - **Definitely still an interesting option for the future though**
> - Ended up with hybrid approach
>   - Majority of objects going through efficient full deferred shading path
>   - **Forward+ rendering for materials that have very specific shading
>     requirements (mostly hair and eyes)**

Two of those six objections are *dead* for this project and four are live.
"Requires two passes for light culling" is a 2014 statement about depth
pre-passes and no longer bites the same way. "Most research considered simple
light models" has been overtaken. But **decals and wetness** (§1, §11) and **wave
occupancy under branchy light models** are exactly the problems a forward
clustered renderer has to answer, and Crytek's answer was to keep a deferred
path for the 95% and run Forward+ only for hair and eyes.

Note what that hybrid implies: CryEngine 5 ships **both**, and the forward path
reads the *same* tiled light list (`Fwd_TileLightMask`, `Fwd_TiledLightsShadeInfo`
in `TiledShading.cfi` — the `Fwd_` prefix is literally "forward"). The light
culling is shared; only the shading site differs. That is the design worth
copying: cull once into a tile/cluster structure, then let both a deferred kernel
and a forward pass consume it.

### 2.4 Physically based shading

Ryse was the transition [GDC14]. The model has not changed since:

- **Specular:** Cook-Torrance microfacet, **GGX** NDF, Schlick Fresnel,
  Schlick-Smith visibility term — with roughness *remapped* on the visibility
  term "to avoid highlights getting too hot on smooth surfaces … (artistic
  choice)".
- **Diffuse:** **Oren-Nayar**, not Lambert. "Subtle but nice quality improvement
  for rough materials like stone. Converges to Lambertian model for smooth
  materials." Crytek is unusual in shipping this — most engines take Lambert and
  spend the cycles elsewhere.
- **Authoring:** smoothness maps, not roughness maps. "Found inverse roughness to
  be more intuitive to author."
- Specular reflectance from **IOR** values.

**Specular antialiasing** got its own treatment, and this is the part most worth
taking: normals and roughness are *strictly coupled*, roughness lives in the
normal map's alpha in source assets, and **normal-map variance from mip
generation is baked into the roughness mips** [HILL12]. A flat mip does not mean
a flat surface; it means a rougher one. If you downsample normals without doing
this you get exactly the sparkle PBR is famous for.

---

## 3. Light types and probes

| type | notes |
|---|---|
| Point / omni | sphere volume |
| Projector / spot | cone volume, `Fwd_SpotTexArray` — projector textures are a `Texture2DArray`, so a spot light with a gobo costs no extra binding |
| Area | supported; [GDC14] lists them among "many more different light types used in practice" |
| Sun | its own volume type (`TILEDLIGHT_VOLUME_SUN`) |
| Environment probe | OBB volume, **box-projected** cubemap, split into `SpecCubeArray` and `DiffuseCubeArray` |
| Ambient light | a light entity with no direction, used as a fill |

**Probes are two cubemap arrays, not one.** Diffuse irradiance and specular
radiance are separate `TextureCubeArray`s bound to the tiled shading pass, so
diffuse can be a very low resolution while specular keeps its mip chain.
Resolution is a dial: `r_EnvCMResolution` 0/1/2 = 64/128/256, **default 256**.

Per-pixel probe and light culling inside portals is a separate switch,
`r_VisAreaClipLightsPerPixel` (default on) — this is the `ClipVolumes` stage in
§1, which stamps a stencil ID per vis-area/portal so a light in one room cannot
leak into the next through a shared tile. `STiledClipVolumeInfo` packs two blend
IDs and flags into a single `uint`, and `GetClipVolumeWeightBinary` does the
test. For an indoor game this is the mechanism that makes tiled deferred behave.

---

## 4. Shadows

### 4.1 Five shadow pass types

```cpp
enum EPass : uint8 {
    ePass_DirectionalLight       = 0,   // cascaded sun
    ePass_DirectionalLightCached = 1,   // cached/static cascades
    ePass_LocalLight             = 2,   // shadow pool
    ePass_DirectionalLightRSM    = 3,   // reflective shadow map, for GI
    ePass_LocalLightRSM          = 4,   // reflective shadow map, for GI
};
```

The RSM passes exist to feed SVOGI (§5.3) — a reflective shadow map records
albedo and normal at the shadow sample, so the GI system can bounce light from
it. `e_svoTI_RsmUseColors` toggles the colour channels, and [CFG] turns it *off*
at the lowest spec, which is a clean example of a quality dial that removes a
render target rather than lowering a count.

### 4.2 The shadow cache — CryEngine's "don't redraw the static world"

`ShadowCacheGenerator` [SRC567] splits the sun cascades into **dynamic cascades,
rebuilt every frame**, and **cached cascades, rebuilt incrementally**. The
controls:

```
e_ShadowsCacheUpdate           0 = update only when needed
e_ShadowsCacheObjectLod        0 = LOD used for cached shadow generation
e_ShadowsCacheRenderCharacters 0 = characters excluded from the cache by default
e_ShadowsCacheExtendLastCascade
e_ShadowsCacheMaxNodesPerFrame 50   ← the amortisation budget
e_ShadowsCacheJobs             1
static const int MAX_RENDERNODES_PER_FRAME = 50;
```

**Fifty render nodes per frame** is the whole idea: a distant cascade covering
kilometres is not redrawn, it is *repaired* fifty objects at a time, and
characters are excluded by default because they move and would invalidate it
constantly. This is the same family of idea as RE ENGINE's shadow cache
([`re_engine_rendering.md`](re_engine_rendering.md)) — two independent engines
arriving at "cache the static shadow, amortise the repair" is a strong signal
that it is the right answer for a large world.

The cached cascades are also what **HeightMapAO** (§7.2) is built on:
`r_HeightMapAO*` CVars all fire `OnChange_CachedShadows`, because the AO
heightmap is generated from the same cached sun frustum. One structure, two
consumers.

### 4.3 Filtering and the local-light pool

- **Irregular PCF**, 16 taps: `#define SHADOW_SAMPLE_COUNT 16` with
  `irreg_kernel_2d` bound as a shader param, plus a `Fwd_RandomRotations`
  texture per-pixel. Not VSM, not ESM, not a moment map.
- `r_ShadowJittering` default **3.4**, and the help text records a real design
  decision: *"In PC the only use of this cvar is to instantly see the effects of
  different jittering values, because any value set here will be overwritten by
  ToD animation as soon as ToD changes."* **Shadow softness is animated by time
  of day**, not a constant.
- Local lights share a **shadow pool** (`Fwd_ShadowPool` is a single `Texture2D`),
  so a scene with many shadow-casting point lights allocates from one atlas
  rather than one texture each.
- Particles get their own jitter dials (`r_ShadowsParticleJitterAmount 0.5`,
  `…AnimJitterAmount 1.0`) — particle shadows are deliberately noisier and
  animated so the noise reads as translucency rather than as artefacts.
- **Tessellated shadows**: `e_ShadowsTessellateCascades 1` — only the first
  cascade gets displaced geometry in its shadow. Beyond that the silhouette
  difference is not worth the hull shader.

---

## 5. Global illumination — SVOGI / "Total Illumination"

This is CryEngine's signature system and the one thing it has that most engines
do not.

### 5.1 What it is

**Sparse Voxel Octree Global Illumination.** The scene is voxelised into a sparse
octree of bricks; every frame the GPU cone-traces through that octree (and
through shadow maps) to gather occlusion and indirect light. Crytek's own
summary [CE]:

> "prepares a voxel representation of scene geometry at run-time on CPU
> asynchronously and incrementally, then every frame on GPU traces thousands of
> rays through the voxels and shadow maps to gather occlusion and indirect light"

Voxelisation is **CPU-side and incremental**, with an explicit budget:

```
e_svoMaxBricksOnCPU     8192   bricks cached CPU-side
e_svoMaxBrickUpdates       8   bricks uploaded CPU→GPU per frame
e_svoMaxStreamRequests   256   streaming/building requests per frame
e_svoMaxNodeSize          32 m maximum node size for voxelisation
e_svoMaxAreaMeshSizeKB  8000   KB per area for the voxelisation mesh
e_svoTI_MaxSyncUpdateTime  2 s synchronous voxelisation cap (level start)
```

There is also a **real-time GPU voxelisation** path for the near field
(`e_svoVoxGenRes 512`, `e_svoVoxDistRatio 14`, `e_svoVoxNodeRatio 4` — leaf
nodes only, close to the camera). So it is a hybrid: CPU builds the far field
slowly, GPU rebuilds the near field every frame.

### 5.2 Three integration modes

From `TiledShading.cfi`'s `ApplyGI()` [SRC57] — this is the exact code, and the
three modes are a good ladder:

| mode | what it does |
|---|---|
| **0 — AO only** | GI output multiplies diffuse and (Fresnel-weighted) specular. "AO + sun bounces". The cheap mode. |
| **1 — GI replaces probe diffuse** | `diffuseAcc = diffuseIrradiance` outright; specular is *rescaled* by the luminance ratio between the traced irradiance and the probe irradiance, clamped to 2×. Probes still supply specular. |
| **2 — Full GI** | both from the trace. |

Mode 1 is the clever one and worth understanding: rather than tracing specular
(expensive, noisy), it keeps the environment probe's specular and corrects its
*brightness* by however much the traced diffuse differs from the probe's diffuse.
A probe baked at noon used at dusk gets darkened by the right amount without
being re-baked. The blend is weighted by `surfGloss * luminance(specCol)` so
mirrors are left alone and rough surfaces are corrected most.

`e_svoTI_HighGlossOcclusion` is the companion dial: "Normally specular
contribution of env probes is corrected by diffuse GI. This parameter controls
amount of correction (usually darkening) for very glossy and reflective
surfaces."

### 5.3 Resolution, rays and the dials that matter

```
e_svoTI_ResScaleBase       2   diffuse cone-tracing target = ½ res
e_svoTI_ResScaleAir        4   "air"/volumetric tracing    = ¼ res
e_svoTI_ResScaleSpecular   1   specular                    = full res
e_svoTI_DualTracing        2   double the rays per fragment (1 = always on)
e_svoTI_RsmConeMaxLength  12 m maximum RSM ray length — "shorter rays work faster"
e_svoTI_MinVoxelOpacity  0.1   voxelise only geometry above this opacity
e_svoTI_VegetationMaxOpacity 0.18  vegetation voxels are deliberately thin
e_svoTI_VoxelOpacityMultiplier 1   "helps reducing light leaks"
e_svoTI_PointLightsMaxDistance 20 m  beyond this, point lights stop bouncing
```

`e_svoTI_VegetationMaxOpacity 0.18` is the kind of number you only find in
source: **vegetation is voxelised at 18% opacity on purpose**, so a tree canopy
dapples light instead of casting a solid voxel shadow. That is a shipped hack
that solves a real problem, and it costs one clamp.

Crytek's own published cost, from the CryEngine 3 documentation era [CE]: *"On
Xbox One it typically takes 4-5 ms of GPU time and on a good PC (GTX 780) it
takes 2-3 ms in low-spec mode with AO and sun bounce."* Those are **2015 numbers
on 2013 hardware** and are recorded as history.

The quality ladder is in [CFG] — `sys_spec_Shading.cfg` moves
`e_svoti_LowSpecMode` from **6** (lowest) through 4, 3 (default), to **2**
(very high), and simultaneously shortens `e_svoTI_RsmConeMaxLength` from 8 m to
4 m and drops `e_svoTI_RsmUseColors` at the bottom. Three coordinated knobs, one
dial.

### 5.4 Mesh ray tracing — the Neon Noir path, and it is in the public source

The 2019 *Neon Noir* demo was widely reported as "ray tracing without RTX". What
it actually is, from Crytek's own write-up [CE] and confirmed by the CVars in
[SRC567]:

> "For each voxel, we store a reference to overlapping triangles, plus the usual
> information like albedo, opacity, and normal data." … "For diffuse rays, true
> mesh tracing is needed only near the beginning of the ray but for the rest of
> the ray more efficient cone tracing can be used without any visual artifacts."
> … "only smooth and clean surfaces like mirrors require true mesh ray tracing,
> while most low-gloss, less shiny surfaces can be traced much faster simply by
> tracing voxels."

**There is no BVH.** Crytek explicitly reused the SVO as the acceleration
structure — "rather than implementing a traditional BVH … making integration a
relatively straightforward step". The voxel *is* the acceleration structure, and
each voxel holds a triangle list.

The CVars are all there, marked `VF_EXPERIMENTAL`:

```
e_svoTI_RT_Active        0   activates mesh ray tracing for reflections
e_svoTI_RT_MaxDistRay        maximum ray distance for mesh tracing
e_svoTI_RT_MaxDistCam        maximum camera distance for mesh tracing
e_svoTI_RT_MinGloss          minimum surface glossiness for mesh tracing
e_svoTI_RT_MinRefl           minimum surface reflectance for mesh tracing
e_svoTI_RT_MaxTrisPerVoxel 100  triangles registered per voxel
e_svoTI_RT_MaxTexRes     512    maximum texture size for GPU tracing
e_svoTI_RT_SafetyBorder 0.25    extends voxel/triangle intersection
```

**`MinGloss` and `MinRefl` are the whole trick**: they are the threshold at which
a surface is promoted from cone tracing to mesh tracing. Everything else falls
back to voxels. Vega 56 at 1080p30, or 1440p40+ with half-resolution reflections
[CE] — again, 2019 hardware.

The same system has an experimental **ray-traced sun shadow** mode
(`e_svoTI_ShadowsFromSun`, "normally supposed to be used in combination with
normal shadow maps and screen space shadows") and can fold the whole terrain
heightmap into it (`e_svoTI_ShadowsFromHeightmap`).

There is also a **"Troposphere"** mode — 14 experimental CVars that replace fog
entirely with SVO-traced atmospherics, layered fog, and even procedural cloud
generation inside the octree. It is clearly unfinished (three CVars are
documented as "CloudsGen magic number") but it shows where Crytek were pointing.

### 5.5 Prehistory — Light Propagation Volumes

Before SVOGI, CryEngine 3 shipped **Light Propagation Volumes** (Kaplanyan,
I3D 2010) — a cascaded 3D grid of spherical harmonics that light is injected
into and then iteratively propagated through. It is superseded and no longer the
GI path, but it is the ancestor of a great many "cascaded SH volume" systems and
the papers are in the archive. **Verdict: historical.** SVOGI replaced it.

---

## 6. Reflections

### 6.1 Screen-space reflections

`ScreenSpaceReflections` stage, run **before** deferred lighting (§1) so the
tiled shading pass can consume it.

```
r_SSReflections  0   glossy screen space reflections [0/1]   ← off in the code default
r_SSReflHalfRes  1   half resolution                          ← on
```

The code default is off but [CFG] turns it **on at default spec** and only
disables it at spec 1; `r_SSReflHalfRes` goes to full resolution only at spec 4.
So the shipped ladder is: off → half-res → full-res. That is exactly the
"quality dial" shape this project prefers.

Note the ordering consequence: SSR runs from the *previous* frame's colour, since
the current frame's lighting has not happened yet. Standard, but it is why the
`FrameToFrame` copy exists in the post chain.

### 6.2 The reflection stack, in priority order

CryEngine layers four things and they are not alternatives:

1. **SVOGI specular** (§5.2 mode 1/2) or mesh-traced reflections (§5.4) —
   the ground truth when enabled.
2. **SSR** — screen-space, half-res, glossy.
3. **Environment probes** — box-projected cubemaps in a `TextureCubeArray`, the
   fallback everywhere SSR fails.
4. **Planar reflections for water** — a genuine second scene render (§12.2).

`r_ReflectionsQuality` picks what goes into the planar reflection pass:
*"0 (terrain only), 1 (terrain + particles), 2 (terrain + particles + brushes),
3 (everything)"* — default **3**.

---

## 7. Ambient occlusion — two systems at two scales

This is a distinctive CryEngine answer and the most directly reusable idea in the
document for a tile-based game.

### 7.1 SSDO — screen-space *directional* occlusion

Not SSAO. `r_ssdo` default **1**, and the parameters make the difference clear:

```
r_ssdoRadius            1.2    world-space radius
r_ssdoRadiusMin         0.04   clamped in screen space
r_ssdoRadiusMax         0.20
r_ssdoAmountDirect      2.0    ← occlusion applied to light sources
r_ssdoAmountAmbient     1.0    ← occlusion applied to probe irradiance
r_ssdoAmountReflection  1.5    ← occlusion applied to probe specular
r_ssdoColorBleeding     1      "avoid overly dark occlusion on bright surfaces"
r_ssdoHalfRes           2      "low res depth except for small camera FOVs"
```

**Three separate strengths for three lighting terms** is the thing to notice.
Occlusion is not one multiplier applied at the end; it is applied at 2.0× to
direct light, 1.0× to ambient and 1.5× to reflections. Direct light is
*over*-occluded relative to ambient, deliberately, because contact shadows read
as the effect people expect. Most engines expose one AO strength and quietly
apply it to ambient only.

`r_ssdoHalfRes 2` is a nice defensive default: use low-resolution depth normally,
but fall back to full resolution at small FOVs where the reprojection error
becomes visible. A quality heuristic with a stated failure mode.

**Colour bleeding** requires tiled deferred shading — the AO term carries chroma
so a red wall's occlusion tints red rather than going grey.

### 7.2 HeightMapAO — large-scale occlusion from a heightmap

```
r_HeightMapAO            1     0=off, 1=quarter res, 2=half, 3=full
r_HeightMapAOAmount    1.0
r_HeightMapAOResolution 2048   texture resolution of the heightmap used
r_HeightMapAORange     1000    metres around the viewer
```

A **2048² heightmap covering a kilometre**, generated from the cached sun shadow
frustum (§4.2), used to compute occlusion at a scale SSDO cannot see. SSDO's
radius is 1.2 m; HeightMapAO's is 1000 m. They are not competing, they are two
octaves of the same signal, and CryEngine runs both by default.

**This is the pattern worth taking wholesale.** A tile game has a heightmap
already. Large-scale AO from a coarse height representation is nearly free,
completely stable (no temporal noise, no screen-space dependence), and it
supplies exactly the term screen-space AO is worst at — a building occluding the
valley below it. See [`surface_depth.md`](../../topics/surfaces/surface_depth.md)
and [`terrain_rendering.md`](../../topics/world/terrain_rendering.md).

There is a third, distance-only term inside SVOGI:
`e_svoTI_DistantSsaoAmount 0.5`, "large scale SSAO intensity in the distance",
and `e_svoTI_SSAOAmount` which scales SSDO *down* when GI is active — because
traced occlusion already contains it and double-darkening is the classic bug.

---

## 8. Subsurface scattering

`ScreenSpaceSSS` stage, run **inside** the deferred lighting block immediately
after `TiledShading` (§1), reading `m_pTexSceneTargetR11G11B10F[0]`.

```
r_DeferredShadingSSS   "Toggles deferred subsurface scattering (requires full deferred shading)"
```

The mechanism:

- **RT1.a of the GBuffer holds a subsurface scattering *profile ID*** (§2.2), not
  a colour. The blur kernel is selected per-pixel by profile.
- **RT0.a holds translucency luminance**, and RT2.GBA can hold transmittance
  chrominance instead of specular chrominance — so a dielectric gets a coloured
  transmission term for free by reusing the metal-only bytes.
- The blur is **separable, screen-space, post-lighting** — the classic
  Jimenez-style separable SSS applied to the lit diffuse buffer.
- `%SUBSURFACE_SCATTERING` is a material feature flag in `Illum.ext` (mask
  `0x80000`), so any Illum material can opt in; skin is not a special shader for
  SSS purposes.
- `HumanSkin.cfx` exists as a *separate* shader anyway, with its own `.ext`,
  tessellation include (`HumanSkinTess.cfi`) and validation include — for
  wrinkle maps and the eye/skin-specific forward path [GDC14] mentions.
- **Hair and eyes are Forward+, not deferred** [GDC14] — `Eye.cfx`, `Hair.cfx`
  and `r_DeferredShadingTiledHairQuality` (0/1/2 across the spec ladder [CFG]).

The design point: **SSS profile as a GBuffer ID rather than parameters** costs one
byte and lets the blur pass batch by profile. It is the same idea as a material
ID, scoped to one effect.

---

## 9. Volumetrics

### 9.1 Volumetric fog — a froxel grid, and the resolution is startling

```
r_VolumetricFogTexScale   10    width/height divided by 10  ← not 8, not 4
r_VolumetricFogTexDepth   32    depth slices
r_VolumetricFogReprojectionBlendFactor 0.9
r_VolumetricFogReprojectionMode 1   0 = conservative, 1 = advanced
r_VolumetricFogSample     0    0 = 1 sample/voxel, 1 = 2, 2 = 4
r_VolumetricFogShadow     1    0 = 1 shadow sample/point … 3 = 4 samples
r_VolumetricFogDownscaledSunShadow      1
r_VolumetricFogDownscaledSunShadowRatio 1   0 = ¼, 1 = ⅛, 2 = 1/16 downscaled
r_VolumetricFogMinimumLightBulbSize 0.4  "small bulb size causes the light flicker"
```

Format is `eTF_R16G16B16A16F` [SRC567].

At 1920×1080 that is a **192 × 108 × 32 froxel volume** — about 663k voxels.
Compare RDR2's froxel grid in [`rdr2_atmospherics.md`](rdr2_atmospherics.md); the
XY resolution here is notably *coarser* and the depth resolution comparable. The
temporal reprojection blend of **0.9** is doing a lot of work to hide that: nine
tenths of each froxel comes from history.

**The pipeline inside the stage**, from `VolumeLighting.cfi` [SRC57]:

```
StoreDownscaledMaxDepth (H, V)   ← conservative max-depth pyramid, so injection
                                    can skip froxels fully behind geometry
BuildLightListGrid               ← 3D light culling, 4×4×4 froxel clusters
InjectFogDensity                 ← global fog + N artist-placed fog volumes,
                                    per-froxel, clip-volume aware
BlurHorizontal/VerticalDensityVolume
(inscattering: sun + regular lights, jittered)
BlurHorizontal/VerticalInscatteringVolume
TemporalReprojection
raymarch → final volume
```

### CryEngine *is* clustered — just only for fog

This is the correction the fog system forces, and it is worth pulling out of §2.1:
the opaque lighting path is 2D-tiled, but the **volumetric fog does real 3D
clustered light culling**, and the shader is unambiguous:

```hlsl
#define BLOCK_SIZE_X 4
#define BLOCK_SIZE_Y 4
#define BLOCK_SIZE_Z 4          // ← a Z dimension: this is a cluster, not a tile

RWBuffer<uint> LightGridOutput  : register(u0);
RWBuffer<uint> LightCountOutput : register(u1);
groupshared uint LightIndicesGrid[TILED_SHADING_MAX_NUM_LIGHTS];

[numthreads(BLOCK_SIZE_X, BLOCK_SIZE_Y, BLOCK_SIZE_Z)]
void BuildLightListGridCS(…)
{
    // Scale and bias for frustum to fit grid cells
    …
    float depthFront = GetVolumetricFogLinearDepth(float(GroupID.z) * BLOCK_SIZE_Z - 1);
    float depthBack  = GetVolumetricFogLinearDepth(float(GroupID.z) * BLOCK_SIZE_Z + 3);
    // Cull light against frustum planes
```

**4×4×4 clusters, per-cluster frustum planes built from scaled/biased projection
columns, lights culled into a group-shared index list, written to a global light
grid.** That is textbook clustered forward culling, in production, in a 2020
CryEngine — and it shares the *same* 255-light array and `SVolumeLightCullInfo`
structure as the 2D tiled path. Crytek built the clustered machinery, used it
where the depth dimension was unavoidable, and left opaque shading on tiles.

For [`clustered_forward_lighting.md`](../../plans/clustered_forward_lighting.md)
this is the more useful precedent than §2.1: it shows one light list feeding both
a 2D and a 3D culling structure, which is exactly the arrangement a renderer
wants if it has both opaque geometry and a participating medium.

Three more details worth stealing:

**Sun shadows are sampled from a downscaled shadow map inside the fog pass** —
1/8th by default. Fog does not need cascade-resolution shadows and sampling the
full-res map from a compute shader over 663k voxels would be ruinous. The CVar
even documents replacing the first two cascades with a static shadow map.

**A minimum light bulb size to stop flicker.** A point light with a tiny
attenuation radius, sampled at froxel centres, aliases catastrophically as it
moves. Clamping the bulb size to 0.4 m removes the flicker at the cost of a
slightly softer light. That is a one-line fix for a bug that reads as "the
volumetrics are broken".

**Everything is jittered along the ray, and the jitter is per-frame.**
`GetJitterInternal(pixelCoord.xy, frameCount.xx)` produces an offset that is used
in *every* accumulation — density injection (`lerp(depthFront, depth, jitter)`),
sun shadow accumulation, and each regular light. So each froxel samples at a
different depth within itself each frame, and the 0.9 reprojection blend
integrates those samples into a smooth result. **The jitter is what makes 32
depth slices look continuous**; without it the slices band visibly. This is the
same bargain as TAA and it fails the same way — fast camera motion breaks the
history and the banding briefly returns, which is why there are two reprojection
modes ("conservative" and "advanced").

Density and inscattering both get **separable horizontal + vertical blurs** as
whole passes over the volume, and there is a **downscaled conservative max-depth
buffer** so injection can skip froxels entirely occluded by geometry.

`FogVolume.cfx` and `WaterFogVolume.cfx` are separate — artist-placed local fog
volumes and the underwater fog volume respectively, both feeding the same system.
`r_FogShadowsWater 1` gives water volumes volumetric fog shadows.

### 9.2 Volumetric clouds

A full `VolumetricClouds` stage with its own **shadow-generation pass that runs
twice per frame** — once before shadow-mask generation and once after the GBuffer
(§1). Clouds cast shadows onto the world, and that shadow has to exist before the
shadow mask is resolved.

`Clouds.cfx` and `DistanceClouds.cfx` are the older billboard/impostor cloud
systems that still ship alongside it. Compare
[`unigine_clouds.md`](unigine_clouds.md) and
[`rdr2_atmospherics.md`](rdr2_atmospherics.md) for how the same problem is
answered elsewhere — CryEngine's is the least documented of the three and the
source is the only description.

### 9.3 Sun shafts — a screen-space radial blur, and it is worth being clear about that

CryEngine's marketing says "accurate sun light shafts". The `SunShafts` post
effect is **not** a raymarch and not physically accurate; it is a two-iteration
8-tap radial blur toward the sun, at quarter resolution. The shader is short
enough to characterise exactly [SRC57], `Sunshafts.cfx`:

```hlsl
// SunShaftsGenPS — run twice (m_passShaftsGen0, m_passShaftsGen1)
float2 sunDir = GetScaledScreenTC(sunPosProj.xy) - IN.baseTC.xy;
sunDir.xy *= cbSunShafts.params.x * fSign;

half4 accum = GetTexture2D(SSHFTG_Target, …, IN.baseTC.xy);          // tap 0, weight 1
accum += GetTexture2D(…, IN.baseTC.xy + sunDir.xy * 1.0) * (1.0-1.0/8.0);
accum += GetTexture2D(…, IN.baseTC.xy + sunDir.xy * 2.0) * (1.0-2.0/8.0);
…                                                                     // 8 taps total
accum /= 8.0;

OUT.Color = accum * 2 * float4(sunDist.xxx, 1);
```

Three passes in total:

| pass | technique | what |
|---|---|---|
| 1 | `SunShaftsMaskGen` | builds the occlusion mask from **linear depth** — sky and distant pixels pass light, near geometry blocks it |
| 2–3 | `SunShaftsGen` ×2 | 8-tap radial blur toward the projected sun position, weights falling off linearly as `1 - i/8`, the second pass with a wider stride so the effective reach is ~64 taps |

Plus an `OcclCheckTechnique` — an occlusion-query quad at the sun's position, so
the whole effect can be faded out when the sun is behind geometry.

`sunDist` fades the effect by angular distance from the sun with an **aspect-ratio
correction** (`float2(1, fAspectRatio)`), so the falloff is circular on screen
rather than elliptical. A small correctness detail most implementations of this
effect omit.

**Where the actually-shaped shafts come from is §9.1, not here.** The volumetric
fog samples sun shadows per froxel (`AccumulateSunShadow`, jittered, from a
downscaled cascade), which produces genuine light shafts through windows and
foliage with correct occlusion and correct perspective. The `SunShafts` post
effect is the cheap screen-space bloom-toward-the-sun that sits *on top* of that.
Two systems, often confused, and only one of them is a light transport
calculation. **[inferred]** — Crytek do not say this anywhere; it follows from
reading both shaders.

- `r_WaterGodRays 1` + `r_WaterGodRaysDistortion 1` — a separate underwater
  god-ray path, again screen-space.
- Sky: `Sky.cfx`, `SkyHDR.cfx`, `Stars.cfx`, and **`SkyLightNishita`** in
  Cry3DEngine — a CPU-side Nishita atmospheric scattering model driving a sky
  update rate (`e_SkyUpdateRate`, 1.0 default, 0.5 at low spec [CFG]). The sky is
  *baked incrementally on the CPU*, not raymarched per frame.
- `LensOptics.cfx` + an `OpticsManager` — a physically-modelled lens flare system
  with tessellated flare geometry (`r_FlaresTessellationRatio`), not a sprite
  overlay.

---

## 10. Antialiasing

Five modes, and the list has not changed since Crysis 3 [SRC567]:

```cpp
enum eAntialiasingType {
    eAT_NOAA, eAT_SMAA_1X, eAT_SMAA_1TX, eAT_SMAA_2TX, eAT_TSAA,
    eAT_DEFAULT_AA = eAT_SMAA_1TX,          // ← the default
};
constexpr const char* s_pszAAModes[] = { "NO AA", "SMAA 1X", "SMAA 1TX", "SMAA 2TX", "TSAA" };
```

| mode | what it is |
|---|---|
| **SMAA 1X** | pure morphological — edge detect → blend weights → neighbourhood blend, using the standard `AreaTex.dds` / `SearchTex.dds` lookups |
| **SMAA 1TX** | SMAA + **one** temporal history sample, no jitter. **The default.** |
| **SMAA 2TX** | SMAA + 2× temporal with a 2-sample jitter pattern |
| **TSAA** | full temporal AA, Halton jitter |

Jitter patterns are **Halton(2,3)** at three sequence lengths — `% 8`, `% 16` and
`% 1024` depending on `r_AntialiasingTAAPattern`; SMAA 2TX forces pattern 2, TSAA
forces pattern 5. TSAA also applies a **negative mip LOD bias**
(`r_AntialiasingTSAAMipBias`, applied in `DriverD3D.cpp`) because temporal
accumulation lets you sharpen the texture sampling.

The TAA tuning exposes something most engines hide:

```
r_AntialiasingTAAFalloffHiFreq  6.0   "high contrast regions"
r_AntialiasingTAAFalloffLowFreq 2.0   "low contrast regions"
r_AntialiasingTAASharpening     0.2
```

**Two separate history falloffs, one for high-frequency and one for low-frequency
content.** High-contrast regions get a *larger* falloff (6.0 — more temporal
stability, more blur) than flat regions (2.0). The help text states the trade in
both directions: "Bigger value increases temporal stability but also overall image
blurriness." That split is the correct instinct — ghosting and shimmer live at
different frequencies and one clamp cannot serve both.

Also present: `r_Supersampling` (1/2/3 = 1×1, 2×2, 3×3) with a choice of resolve
filter — **box, tent, Gaussian, Lanczos** (`r_SupersamplingFilter`). And
`r_AntialiasingModeSCull`, a **stencil-culling optimisation** that restricts the
morphological passes to pixels the edge-detect flagged.

**No DLSS/FSR/XeSS in 5.6.7 or 5.7.** Hunt: Showdown 1896 ships **FSR 2.1.2**
[3P] — that is a 5.11 addition and one of the clearest markers of the gap between
the public engine and the shipped one (§20).

---

## 11. Decals

Two entirely separate systems, and the choice between them is exposed:

```
e_DecalsForceDeferred     0  1 = force all decals to deferred
e_DecalsDeferredStatic    1  non-planar designer-placed decals → deferred
e_DecalsDeferredDynamic   1  gameplay decals → deferred (2 = force non-deferred)
e_DecalsMaxTrisInObject 8000 don't create mesh decals on objects above this
e_DecalsMaxUpdatesPerFrame 4 static decal render-mesh updates per frame
e_DecalsRange                less precision for decals outside this range
e_DecalsNeighborMaxLifeTime 4.0  new decals force old ones to fade in 4 s
e_DecalsOverlapping       0  0 = don't spawn if too close to an existing decal
```

**Mesh decals** clip the receiving geometry and build a render mesh — accurate,
follows the surface exactly, costs a mesh build (budgeted at 4 per frame) and is
refused on objects over 8,000 triangles. **Deferred decals** are boxes projected
into the GBuffer.

The deferred path [SRC567] `DeferredDecals.cpp`:

- decals are **sorted before drawing**, by `DECAL_HAS_NORMAL_MAP` first and
  `nSortOrder` second — so all the non-normal-mapped decals draw as one batch and
  all the normal-mapped ones as another. Sorting by *state* before sorting by
  *order*, which is only correct because decals within a group are commutative.
- a **temporary copy of the scene normals** is made (`m_pTexSceneNormalsBent`)
  so the pass can read the normals it is also writing — a decal's normal is
  transformed into world space against the surface it lands on, which needs the
  original.
- the GBuffer's YCbCr specular encoding (§2.2) exists **specifically** so
  non-metal decals can blend without writing alpha.

The lifetime management is the part games actually need and rarely build:
`e_DecalsNeighborMaxLifeTime` makes a *new* decal accelerate the fade of nearby
old ones, and `e_DecalsOverlapping` refuses spawns that are too close. Bullet
holes therefore cannot pile up unboundedly in a firefight, and the cap is
enforced spatially rather than by a global count.

> Directly relevant to the decal work in `src/cromwell/decal/` — the
> sort-by-normal-map-then-order rule and the neighbour-fade lifetime policy are
> both cheap and both solve problems that show up later. See
> [`decals.md`](../../topics/surfaces/decals.md).

---

## 12. Water

### 12.1 The ocean — and FFT is *off* by default

```
e_WaterOceanFFT   0   "Activates fft based ocean"     ← default OFF
e_WaterOceanBottom 1
```

`m_bOceanFFT` is only set when `e_WaterOceanFFT` is on **and** the water shader
quality is `eSQ_High` or better [SRC567]. The FFT path produces a **64×64
displacement grid** (`pGridFFT[u + v * 64]`) that the CPU also reads back to
answer height queries for physics and buoyancy — the same grid serves rendering
and simulation, sampled bilinearly.

The default path is not FFT. It is a procedural sum-of-waves evaluated in the
vertex shader over a screen-space-projected grid, which is why the tessellation
CVars are shaped the way they are:

```
e_WaterTessellationAmount   10
e_WaterTessellationAmountX  10
e_WaterTessellationAmountY  10
e_WaterTessellationSwathWidth 12  "swath width for the boustrophedonic mesh stripping"
r_WaterTessellationHW        0    hardware tessellation, off by default
```

"Boustrophedonic mesh stripping" — the grid is emitted as a serpentine strip so
adjacent rows share vertices. That is a 2007-era optimisation that has never been
removed.

Compare [`sea_of_thieves_water.md`](sea_of_thieves_water.md): Sea of Thieves is
FFT-first, UNIGINE is Gerstner-first, CryEngine ships both and defaults to
neither-FFT. **Verdict:** the CryEngine ocean is the weakest of the three as
shipped, and Crytek evidently knew it — Hunt: Showdown 1896's water was rebuilt
with fluid simulation in 5.11 [3P] (§20).

### 12.2 Reflections, and the reflection budget

Water reflections are a **real second scene render**, and the update logic is
unusually careful:

```
r_WaterReflections 1
r_WaterUpdateFactor 0.01            base update rate
r_WaterUpdateDistance 2.0
r_WaterReflectionsMinVisiblePixelsUpdate 0.05   only update above 5% screen coverage
r_WaterReflectionsMinVisUpdateFactorMul  20.0   ← 20× slower when mostly occluded
r_WaterReflectionsMinVisUpdateDistanceMul 10.0
r_ReflectionsQuality 3              0=terrain, 1=+particles, 2=+brushes, 3=everything
r_WaterUpdateThread 5               water update on hardware thread 5
```

**The reflection is updated at a rate proportional to how much of the screen the
water occupies**, with a 20× penalty when it is mostly occluded and a 5%
threshold below which it stops entirely. This is a *temporal* LOD on a render
pass, not a spatial one, and it is the right shape for anything expensive whose
output changes slowly.

The stage also copies the SSR result into the water reflection
(`m_passCopySSReflection`) and generates a mip chain
(`m_passWaterReflectionMipmapGen`) so rough water samples a blurrier mip.

### 12.3 Caustics

Two systems again:

- **Ocean caustics** — `ExecuteDeferredOceanCaustics`, a deferred screen-space
  pass, `r_WaterCausticsDeferred` (0/1/2, where 2 adds a stencil pre-pass),
  `r_WaterCausticsDistance 100`.
- **Water-volume caustics** — a real projected caustic mesh.
  `r_WaterVolumeCausticsDensity` **128** (clamped 16–255) builds a
  (128+1)² vertex grid with 128²×6 indices, snapped to a grid
  (`r_WaterVolumeCausticsSnapFactor 1.0`, *"to avoid aliasing"*), dilated and
  blurred (`m_passWaterCausticsDilation`, `m_passBlurWaterCausticsGen0/1`), with
  a 35 m max distance.

The **snap factor** is the detail worth remembering: a projected caustic grid
that moves continuously with the camera crawls; snapping its projection to a
world-space grid makes it stable. Same trick as snapping shadow cascades.

### 12.4 Ripples and the rest

- `WaterRipples` stage — a GPU ripple simulation that things moving through water
  write into, feeding the normal map.
- `WaterFogVolume.cfx`, `WaterOceanBottom.cfx`, `WaterVolume.cfx` — the
  underwater fog volume, the ocean floor (drawn separately so refraction has
  something to bend), and artist-placed water volumes (rivers, ponds).
- `WaterCausticsPass.cfi`, `WaterReflectionsPass.cfi`, `WaterCommon.cfi` — shared
  includes, so ocean and water volumes share their wave and shading code.
- Foam is a single texture (`WaterFoam.tif`), not a simulation.

---

## 13. Terrain

### 13.1 Structure

A **heightmap on a quadtree of sectors**, with two independent LOD criteria:

```
e_TerrainLodDistanceRatio 0.5   LOD by sector distance vs sector size
e_TerrainLodErrorRatio   0.05   LOD by max elevation error within the sector
```

**Distance *and* geometric error**, both, and the coarser of the two wins. A flat
sector drops LOD aggressively; a cliff holds detail further out. That is the
correct pair of criteria and many terrain systems ship only the first.

```
e_TerrainMeshInstancingMinLod 3         distant sectors switch to instanced meshes
e_TerrainMeshInstancingShadowLodRatio 0.3  even coarser for shadow generation
e_TerrainMeshInstancingShadowBias 0.5   render distant sectors slightly lower in
                                        shadow passes to avoid self-shadowing
e_TerrainTextureStreamingPoolItemsNum 256  base-texture streaming pool
e_TerrainOcclusionCullingMaxDist 200    terrain used as an occluder, 200 m rays
```

**Mesh instancing for distant sectors** — beyond LOD 3 the terrain stops being a
unique mesh per sector and becomes instanced copies of a canonical grid, displaced
in the vertex shader. And in the *shadow* pass it goes coarser still and is
pushed 0.5 m down to stop self-shadow acne, which is a blunt fix that costs
nothing.

Terrain is also an **occluder**: `terrain_hmap_occlusion.cpp` raymarches the
heightmap for CPU occlusion culling out to 200 m.

### 13.2 Layer blending — 4-bit weights, three layers per vertex

From `Terrain.cfx` [SRC57], the exact decode:

```hlsl
// decode 3 weights
int arrWeights[3];
arrWeights[2] = ((nW >> 4) & 15);
arrWeights[1] = (nW & 15);
arrWeights[0] = 15 - arrWeights[1] - arrWeights[2];
```

**Three layers per vertex, 4 bits each, with the third implied** — two nibbles in
one byte, and the first weight is whatever is left over so the set always sums to
1. Eight bits of blend data per terrain vertex, total. Then each detail material
finds its own weight by matching a layer ID:

```hlsl
weight = max(weight, saturate(1 - abs(colorG * 255 - terrainLayerInfo[2].w)) / 15.f * arrWeights[c]);
```

`e_TerrainDetailMaterialsWeightedBlending 1` — "Enable advanced weighted blending
between terrain detail materials" — is the switch for the height-aware version.
CryEngine's terrain and its `Illum` blend layer both blend by **height map**, not
by linear alpha: the docs are explicit that "Height maps can be plugged in with
either OBM or POM enabled because the blending will actually use the height map
to determine how the materials blend together" [CE], and `BlendFactor` /
`BlendFalloff` are the exposed knobs. Gravel appears in the mortar lines before
it appears on the brick faces.

### 13.3 The base texture, and how CryEngine hides tiling

**This is the answer to "what anti-tiling tech do they have", and the answer is:
almost none of the modern kind, and one very effective old one.**

Searched across all 145 shader files [SRC57]: **no triplanar mapping, no
stochastic/hex-tile texturing, no histogram-preserving blending, no variance
texturing.** Those terms do not appear. This is a genuine absence, not a gap in
the search — the shader library is complete.

What CryEngine has instead is a **baked, unique, low-frequency base texture over
the whole terrain**, with tiling detail materials multiplied on top of it:

```
e_TerrainAutoGenerateBaseTexture        0     build the base texture from painted layers
e_TerrainAutoGenerateBaseTextureTiling  1/16  tiling of the baked diffuse textures
e_TerrainDetailMaterials                1
e_TerrainDetailMaterialsViewDistZ / …XY       max view distance for detail materials
r_DetailDistance                        8 m   (4 m at low spec [CFG])
```

and in `Terrain.cfx`:

```hlsl
// automatic high pass, controlled by DetailTextureHighPassRange
…
DetailTextureStrengthFade  "Controls fading of Detail Texture depending on terrain base texture darkness"
```

The mechanism: the **base texture carries all the low-frequency variation** and is
unique per square metre; the tiling detail material is **high-passed** so it
contributes only high frequencies, then multiplied in and faded out with
distance. Tiling is invisible not because the tile is broken up but because *the
repeating signal has had its low frequencies removed*, and the low frequencies
come from somewhere unique.

This is the same conclusion RUSE reached with a diversity field and Broken Arrow
paid Granite 27 GB to avoid (see [`ruse.md`](../strategy/ruse.md) §3.4 and
[`broken_arrow.md`](../flight/broken_arrow/broken_arrow.md)). **High-passing the
tiling layer is the cheapest of the three and it is what CryEngine ships.**

The same trick is applied to objects: `e_VegetationUseTerrainColor 1` blends
vegetation towards the terrain's base colour with distance, and
`FOB_ALLOW_TERRAIN_LAYER_BLEND` lets a placed mesh receive terrain layers at its
base so a rock does not sit on the ground like a sticker.

### 13.4 Parallax occlusion mapping and displacement

CryEngine has a **four-level micro-detail ladder**, selected per material and
resolved in `GetMicroDetailParams` [SRC57]:

| level | flag | what it does |
|---|---|---|
| bump | `%NORMAL_MAP` | normal map only |
| **OBM** | `%OFFSET_BUMP_MAPPING` (0x20000) | offset bump mapping — one-step UV offset |
| **POM** | `%PARALLAX_OCCLUSION_MAPPING` (0x8000000) | ray-marched, **15 steps** in the terrain shader |
| **Silhouette POM** | `%SILHOUETTE_PARALLAX_OCCLUSION_MAPPING` (0x10000) | POM that also modifies the silhouette; `r_SilhouettePOM 0` gates it |
| **Displacement** | `%DISPLACEMENT_MAPPING` (0x10000000) | real geometry, needs tessellation |

The terrain shader runs POM **on terrain layers**, with self-shadowing:

```hlsl
if (mdQuality == MICRO_DETAIL_QUALITY_OBM)
    baseTC.xy = OffsetMap(baseTC.xy, -vViewTS, 2, mdDisplacement, mdHeightBias, 0, 0);
else if (mdQuality == MICRO_DETAIL_QUALITY_POM)
    const float3 offsetBest = ParallaxOcclusionMap(baseTC.xy, lod, vViewTS, 15, mdDisplacement, …);
…
// Self shadowing
…
// Force standard model for compatibility of overlay passes with opaque geometry passes
// (but this disables POM self-shadowing)
if (!bAllowPomModel)  // force standard model for terrain layers on the top of normal objects
```

**POM displacement is multiplied by the layer's blend weight** (`mdDisplacement
*= weight`), so a layer fading out also flattens out rather than popping. And
the comment records a real compatibility cost: terrain layers projected onto
*objects* integrated into the terrain must fall back to the standard model,
losing POM self-shadowing, because the overlay pass has to match the opaque pass.

Global controls:

```
r_UseDisplacementFactor 0.2   global displacement amount
r_SilhouettePOM         0
```

### 13.5 Tessellation

```
e_Tessellation            1     allowed
e_TessellationMaxDistance 30 m  "also affects distance-based displacement fadeout"
r_TessellationTriangleSize 8    desired screen-space triangle size in pixels
e_ShadowsTessellateCascades 1   only cascade 0
e_StatObjTessellationMode 1     0 = none, 1 = load pre-tessellated from disk, 2 = generate on load
```

Two subdivision schemes are material flags: **`%PHONG_TESSELLATION`** (0x20000000)
and **`%PN_TESSELLATION`** (0x40000000) — PN triangles. Plus per-system
tessellation for water (`r_WaterTessellationHW`, off), particles
(`r_ParticlesTessellation 1`, `…TriSize 16`, "for higher quality lighting"),
merged meshes (`e_MergedMeshesTesselationSupport 0`, off) and flares.

**30 metres and 8-pixel triangles** is the whole policy: tessellation is a
close-range detail tool with a hard cutoff, not a geometry pipeline. Every
tessellated shader has a `*Tess.cfi` companion (`IllumTess`, `VegetationTess`,
`HumanSkinTess`, `CommonZPassTess`, `CommonShadowGenPassTess`, …) — twelve of
them — which is the maintenance cost of hardware tessellation stated in file
count.

### 13.6 Objects integrated into terrain

`e_TerrainIntegrateObjectsMaxVertices 30000` per sector,
`e_TerrainIntegrateObjectsMaxHeight 32` — meshes near the ground can be *merged
into the terrain sector mesh*. The terrain vertex shader has explicit handling:

```hlsl
// normally in the distance we move terrain down in order to avoid covering small objects on terrain
// but for object meshes integrated into terrain we use opposite logic in order to avoid
// z-fighting between terrain detail layers and object itself
```

**The terrain is pushed *down* with distance so small props do not sink into it**
— a per-LOD vertical bias, inverted for integrated objects. A hack, documented in
the shader, solving the single most common terrain artefact.

### 13.7 Terrain ↔ mesh blending — two mechanisms, often confused

CryEngine's "terrain and mesh blending" is **two independent systems** that solve
two different halves of the same complaint ("this rock looks like it was pasted
on"). Both are readable in full.

#### Mechanism 1 — terrain layers projected *onto* the mesh, gated by stencil

A render object can opt in to receiving terrain detail layers over its own
surface. It is a render-object flag, promoted to a stencil bit
[SRC567] `CompiledRenderObject.cpp`:

```cpp
ERF_FOB_ALLOW_TERRAIN_LAYER_BLEND = BIT64(43);   // render-node flag
FOB_ALLOW_TERRAIN_LAYER_BLEND     = BIT64(34);   // render-object flag

const bool bAllowTerrainLayerBlending = CV_e_TerrainBlendingDebug == 2 ||
    (CV_e_TerrainBlendingDebug == 0 && (objFlags & FOB_ALLOW_TERRAIN_LAYER_BLEND));
m_StencilRef |= (bAllowTerrainLayerBlending || bTerrain) ? BIT_STENCIL_ALLOW_TERRAINLAYERBLEND : 0;
```

The terrain's detail-layer pass then draws over the whole screen region and the
**stencil test decides which pixels are allowed to receive it**. Anything with the
bit set — terrain sectors themselves, brushes, vegetation, characters, particles,
geometry caches, ropes, and *roads unconditionally* — gets sand or grass or snow
blended over its lower surfaces exactly as though it were terrain.

Which node types opt in, from the source:

| node type | default |
|---|---|
| `RoadRenderNode` | **always** (`FOB_ALLOW_TERRAIN_LAYER_BLEND \| FOB_ALLOW_DECAL_BLEND`, unconditional) |
| terrain sectors and detail objects | always |
| Brush, Vegetation, Character, GeomCache, MergedMesh, Rope, ParticleEmitter | per-instance flag, on unless `bIgnoreTerrainLayerBlend` |

`e_TerrainBlendingDebug` is the diagnostic and the CVar help is the clearest
statement of the design: *"0 = Only blend objects that have
FOB_ALLOW_TERRAIN_LAYER_BLEND set (default), 1 = Disable blending on all objects,
2 = Enable blending on all objects."*

**Doing this with a stencil bit rather than a material feature is the good idea.**
It costs one bit on an existing stencil reference, needs no shader permutation on
the receiving object, and lets an artist toggle it per instance.

#### Mechanism 2 — the mesh's *material* blends toward the terrain with distance

Separately, `%BLENDTERRAIN` in `CommonZPass.cfi` [SRC57] pulls a mesh's surface
attributes toward the terrain's own, weighted per attribute:

```hlsl
void ApplyVegetationTerrainColor(vert2fragZ IN, inout MaterialAttribsCommon attribs)
{
    half3 terrainCol = GetTerrainTex(terrainBaseTex, IN.terrainParams0.xy).rgb;
    attribs.NormalWorld   = lerp(attribs.NormalWorld,   terrainNor,              IN.terrainParams0.z * BlendTerrainCol_Normal);
    attribs.Albedo        = lerp(attribs.Albedo,        terrainCol,              IN.terrainParams0.z * BlendTerrainCol_Albedo);
    attribs.Reflectance   = lerp(attribs.Reflectance,   TERRAIN_BASE_SPEC_COLOR, IN.terrainParams0.z * BlendTerrainCol_Reflectance);
    attribs.Smoothness    = lerp(attribs.Smoothness,    TERRAIN_BASE_SMOOTHNESS, IN.terrainParams0.z * BlendTerrainCol_Smoothness);
    attribs.Transmittance = lerp(attribs.Transmittance, 0,                       IN.terrainParams0.z * BlendTerrainCol_Transmittance);
}
```

with the blend factor built in the vertex shader from camera distance:

```hlsl
float fBlendFactor = saturate(BlendTerrainCol + (fCameraDistance /
                              (blendColInfo.w - blendColInfo.w * BlendTerrainColDist)));
```

**Five attributes, five independent artist weights**, and it happens in the
GBuffer pass so the blended result is what gets lit. Not just albedo — normals,
reflectance, smoothness and transmittance too, which is why a distant tree
dissolves into the hillside instead of staying a shiny green blob against a matte
brown one.

This is the mechanism behind `e_VegetationUseTerrainColor 1` (§13.3), and
`e_VegetationUseTerrainColorDistance` documents the distance policy including the
legacy behaviour: *"0 = Use 30% of maximum view distance of each vegetation
instance (old default way); <0 = maximum view distance is calculated using
vegetation mesh size (non-scaled CGF size) and then multiplied by …"*.

> **Worth taking.** Mechanism 2 is nearly free — it is a lerp in a pass that is
> already running — and it is the single largest contributor to "the vegetation
> sits in the world" at distance. Mechanism 1 is more machinery but it is the
> answer to object bases, and the stencil-bit implementation means the cost is
> confined to the terrain layer pass. See
> [`terrain_rendering.md`](../../topics/world/terrain_rendering.md) and
> [`material_detail.md`](../../topics/surfaces/material_detail.md).

---

## 14. Vegetation and merged meshes

### 14.1 Three tiers

| tier | what | when |
|---|---|---|
| **Vegetation instances** | real `CGF` meshes, LOD'd, instanced | near |
| **Merged meshes** | thousands of grass/bush instances merged into one mesh, with CPU simulation | mid |
| **Billboards** | a single camera-facing quad with a baked texture | far |

Merged meshes are the interesting tier, and they are memory-budgeted rather than
count-budgeted:

```
e_MergedMeshesPool        2750 KB  total main memory for merged meshes
e_MergedMeshesPoolSpines    32 %   share of the pool for "spines"
e_MergedMeshesActiveDist   250 m   streamed in within this range
e_MergedMeshesViewDistRatio 30
e_MergedMeshesLodRatio       3
e_MergedMeshesInstanceDist 4.5     distance at which animation turns off
e_MergedMeshesDeformViewDistMod 0.45  deformables stop updating past this
e_MergedMeshesMaxTriangles 600     "It's more efficient to render them without merging"
e_MergedMeshesUseSpines      1     touch bending
```

**"Spines"** are the touch-bending representation: a chain of segments per plant
that the CPU simulates so a player walking through grass pushes it aside. 32% of
the whole merged-mesh pool is spent on them. There is even
`e_MergedMeshesBulletSpeedFactor` / `…BulletScale` / `…BulletLifetime` — **bullets
displace grass**, approximated as short-lived expanding spheres.

`e_MergedMeshesMaxTriangles 600` is the rule that keeps the system honest: a mesh
above 600 triangles is *not* merged, because merging costs more than instancing
at that size.

### 14.2 Bending

```
e_VegetationBending  2   (does not affect merged grass — that uses spines)
e_Wind               1   global wind, drives bending
e_WindBendingStrength 1.0
e_WindBendingAreaStrength 1.0
e_FoliageWindActivationDist  strong wind force-activates visible foliage
e_FoliageBranchesTimeout 4 s  maximum lifetime of branch ropes without collisions
```

Two systems: **vertex-shader bending** for vegetation instances (cheap, driven by
a global wind vector and per-vertex bending weights painted into vertex colours),
and **spine simulation** for merged meshes (physical, collides). Plus
`StatObjFoliage` — real rope physics on branches, with a 4-second timeout so an
activated bush relaxes back to the cheap path.

### 14.3 LOD

```
e_Lods            1     use LOD models
e_LodRatio        6.0   distance ratio
e_LodFaceArea     1     "use geometric mean of faces area to compute LOD"
e_LodFaceAreaTargetSize 0.005  threshold
e_LodTransitionTime 0.5 "if non 0 - use dissolve for smooth LOD transition"
e_LodMin / e_LodMax / e_CharLodMin
e_ViewDistRatioVegetation 30.0
```

**LOD selection is driven by the geometric mean of face area, not by
bounding-sphere size.** The formula is exact and short [SRC567]:

```cpp
// StatObjRend.cpp — computed once per mesh, at load
m_fLodDistance = sqrt(lodInfo.fGeometricMean);

// Brush.cpp / Vegetation.cpp / CharacterRenderNode.cpp — identical in all three
const float fDistMultiplier = 1.0f / (fEntityLodRatio * frameLodInfo.fTargetSize);
for (uint i = 0; i < SMeshLodInfo::s_nMaxLodCount; ++i)
    distances[i] = lodDistance * (i + 1) * fDistMultiplier;
```

So LOD *i* switches at `sqrt(meanFaceArea) · (i+1) / (lodRatio · 0.005)`, and the
switch distances are **evenly spaced**, not exponential. The geometric mean is
accumulated in log space and weighted by face count (`SMeshLodInfo::Merge`), so
merging sub-objects gives the right answer rather than a naive average.

This is a better criterion than distance-over-bounding-radius because a long thin
object — a fence, a lamppost — has a large bounding sphere and tiny triangles;
sizing by triangle area gets it right, and it means an artist can halve a mesh's
LOD distances by halving its triangle size without touching a setting. See
[`lod_systems.md`](../../topics/world/lod_systems.md).

Transitions are **dissolve over 0.5 s** — a stipple/alpha-test cross-fade, not a
hard pop, and not geomorphing.

### 14.4 Billboard impostors — yes, and they are one quad

**Do they have billboard impostors?** Yes, and the implementation is much simpler
than the question usually implies.

```
e_VegetationBillboards 0   "Allow replacing distant vegetation with billboards
                            Billboard textures must be prepared by
                            ed_GenerateBillboardTextures command in the editor"
```

`CObjManager::GetBillboardRenderMesh()` [SRC567] builds **one shared mesh, ever**:
four vertices, two triangles, a unit quad in XZ with a fixed tangent frame. Every
billboarded tree in the level draws that same mesh with a different material. The
material carries the baked texture, generated offline by
`ed_GenerateBillboardTextures`, and `%BILLBOARD` (mask 0x2000) is a feature flag
on the standard `Illum` shader — so a billboard is lit by the normal material
system, GBuffer and all, not by a special path.

There is a whole **`BillboardGraphicsPipeline`** [SRC567] whose job is to render
the scene into those impostor textures — impostor baking is a *pipeline
configuration*, reusing the real GBuffer and shadow stages, rather than a
bespoke tool renderer. That is the design worth taking.

Note what is *not* there: **no octahedral impostors, no multi-angle impostor
atlas** in the public source. It is a single baked view. The older
`e_VegetationSprites` system and its five companion CVars are all marked
**`VF_DEPRECATED`** — Crytek replaced sprites with billboards and left the tombstones.

---

## 15. Objects, culling and draw submission

Worth a short section because it is where a large-world engine actually spends
its CPU.

- **Octree** (`ObjectsTree.cpp`, `ObjectsTree_MT.cpp`) for the world, with a
  separate multithreaded traversal file.
- **Coverage-buffer occlusion culling** — `CCullThread`, `CCullRenderer`: a
  software-rasterised depth buffer on its own thread, plus terrain heightmap
  occlusion (§13.1) and `OcclusionTest.cfx` / `SoftOcclusionQuery.cfx` for GPU
  queries.
- **Permanent render objects** (`e_PermanentRenderObjects 1`,
  `r_GraphicsPipeline 3`): a static object's PSO, constant buffer and draw state
  are compiled **once** and reused, rather than being rebuilt per frame. This is
  the single biggest CPU win in CryEngine 5 over CryEngine 3, and
  `r_GraphicsPipeline` documents the ladder: *0 legacy, 1 new pipeline with
  objects compiled on the fly, 2 with permanent render objects*.
- **Render jobs**: `e_ExecuteRenderAsJobMask` defaults to brushes, movable
  brushes, vegetation, roads, decals and water volumes — those six node types are
  culled and submitted on worker threads by default.
- **Static instancing**: `e_StaticInstancing 0` (off) caches instancing info,
  "mostly for vegetation".

---

## 16. Animation

### 16.1 Skinning

- **Compute skinning** — `ComputeSkinning` is a pipeline stage running *before*
  the GBuffer (§1), with its own `.cfx` and an `IComputeSkinning` interface.
  Async-compute capable (`r_D3D12AsynchronousCompute +1`) but off by default.
- **Dual quaternion skinning**, from [SIG11] — *Spherical Skinning with Dual
  Quaternions and QTangents*. Still the shipped path.
- **QTangents** — the tangent frame is stored as a **single quaternion** rather
  than a tangent + bitangent + sign. Four components instead of nine, and it
  interpolates correctly. This is Crytek's contribution to the field and it turns
  up again in the geometry cache format (§16.4).
- **Blend shapes / morphs** are GPU-side (`SActiveMorphs` in
  `IComputeSkinning.h`), with culling: `ca_vaBlendCullingThreshold 1.0`,
  `ca_vaBlendEnable 1`, and `ca_vaBlendPostSkinning 0` — morphs are applied
  *before* skinning by default.

### 16.2 Structure

`CryAnimation` is 183 files. The shape:

- **`.chr` / `.skin` / `.caf` / `.dba`** — skeleton, skin, animation clip,
  animation database. DBAs are compressed clip bundles, streamed
  (`ca_StreamDBAInPlace 1`, `ca_StreamCHR 1`).
- **Controllers** — `ControllerPQLog`, `ControllerTCB`, `ControllerOpt`, with
  `QuatQuantization.h`: rotation curves are quantised quaternions and there is a
  defragmenting heap (`ControllerDefragHeap`) for them, which tells you clip
  memory was a real problem.
- **`GlobalAnimationHeaderLMG`** + `ParametricSampler` — LMG is the blend space
  ("locomotion group"): a parametric blend over N clips sampled by velocity/turn
  rate. This is CryEngine's motion-matching-shaped hole; there is **no motion
  matching** in the public source. See
  [`motion_matching.md`](../../topics/animation/motion_matching.md).
- **`Command_Buffer` / `Command_Commands`** — the pose evaluation is a *command
  buffer*: layered clips, IK and modifiers are compiled into a list of commands
  and executed, rather than a hard-coded evaluation order. Good design; it is how
  you keep layered animation extensible.
- **PoseModifier/** (27 files) — IK, look-at, foot placement, physics blending, as
  pluggable pose modifiers on that command buffer. See
  [`rigging_ik.md`](../../topics/animation/rigging_ik.md).
- **`AttachmentVCloth`** — a full cloth simulation as an attachment type. §16.5.
- **`SkeletonPhysics`** + `ca_DeathBlendTime 0.3` — ragdoll blending, with a
  documented "low-detail dead body skeleton". See
  [`networked_animation_physics.md`](../../topics/animation/networked_animation_physics.md).
- **Joint culling**: `ca_ResetCulledJointsToBindPose 1` — off-screen or
  irrelevant joints stop being evaluated.

### 16.3 Facial animation

24 files of dedicated facial animation, plus lipsync with a documented
`ca_lipsync_vertex_drag 1.2` — "vertex drag coefficient when blending morph
targets". Legacy-shaped (it predates the modern blendshape-from-audio approach)
but complete.

### 16.4 Do they use VATs? — no, they use *geometry caches*

**No vertex animation textures.** The equivalent problem — "get a simulation out
of the DCC and play it back" — is solved by **Alembic geometry caches**, and
[SIG14-GC] documents the format precisely. It is a better-engineered answer than
a VAT and worth reading against one.

The motivation, verbatim from the speaker notes:

> "Lots of VFX set pieces. Art pipeline bottleneck (required baking to joints).
> Needed simpler way to get animations into engine. **Solution: Import Alembic.**"
> … "Animations like cloth, water simulations, fur. Alembic: No engine specific
> markup. One click to import and run. Outsourcing much easier."

The budget and the naïve cost:

> "Naïve approach: 56 bytes per vertex (14 floats) — Position, UV, Normal,
> Tangent, Binormal, (Color). **Sail: 30000 vertices, 30 FPS ≈ 50MB/s. Ryse
> budget: 10MB/s**" — and 10 MB/s was for the *whole scene*, not one cache.

The compression ladder, 50 MB/s → under 10:

| step | technique | data rate |
|---|---|---|
| naïve | 56 B/vertex | **50 MB/s** |
| transforms | bake static hierarchies; rigids store transform only, 40 B | 50 |
| **quantisation** | positions **3× uint16** in bbox space (mm accuracy over 64 m, artist-specifiable precision); UVs **2× int16** mapped to [-1024,1024]; **QTangents 4×10 bits** [SIG11]; colours 8-bit RGBA. *"Only lossy step"* | **16 MB/s** |
| **block compression** | per-frame block, **Deflate** or **LZ4 HC** — *"LZ4 HC is usually 20% worse compression, but 10× faster decode. Almost like memcpy."* | **10 MB/s** |
| **prediction** | temporal + spatial prediction, store residuals — *"residual symbols cluster around zero"* | below 10 |

**Restriction: static topology only.** Vertex count and connectivity cannot
change over the cache.

Runtime streaming is budgeted like any other stream:

```
e_GeomCacheBufferSize            128 MB
e_GeomCacheMaxPlaybackFromMemorySize 16 MB  above this, always stream from disk
e_GeomCachePreferredDiskRequestSize 1024 KB
e_GeomCacheMinBufferAheadTime    2.0 s
e_GeomCacheMaxBufferAheadTime    5.0 s
e_GeomCacheDecodeAheadTime       0.5 s
```

**Buffer 2–5 seconds ahead, decode 0.5 s ahead.** Two separate lookaheads,
because reading from disk and decompressing are different latencies.

Compared to a VAT: a VAT is simpler (a texture, sampled in the vertex shader, no
CPU work) but pays for it in memory — a VAT stores every vertex of every frame at
texture precision with no temporal prediction, and cannot stream. The geometry
cache is more machinery for roughly 5× less data and unbounded length. For a
one-second flag flap, a VAT. For a two-minute cinematic sail, this.

---

### 16.5 VCloth — character cloth, and its LOD story is the interesting part

`AttachmentVCloth` is a **position-based dynamics** cloth solver attached to a
character like any other attachment. `SVClothParams` [SRC567]
`CryCommon/CryAnimation/IAttachment.h` is unusually well commented and the
grouping tells you what mattered:

**Constraints** — four kinds, separately tunable:

```cpp
float stretchStiffness;              // 0..1, "bigger for over-relaxation"
float shearStiffness;
float bendStiffness;
float bendStiffnessByTrianglesAngle; // bend stiffness from triangle angles,
                                     // "thus the stiffness is not affecting elasticity"
float pullStiffness;                 // strength of pulling vertices to the SKINNED position
```

`pullStiffness` is the one that makes cloth shippable: the simulation is
continuously pulled back toward the plain skinned result, so it can never drift
into a pose the animator did not intend. Simulation as a *perturbation of
skinning*, not a replacement for it.

`bendStiffnessByTrianglesAngle` is the correct formulation — deriving bend
resistance from the dihedral angle between adjacent triangles rather than from a
distance constraint, so stiffening the bend does not also make the cloth
inextensible.

**Nearest-neighbour distance constraints** — a second constraint family layered on
top, specifically to stop the classic PBD failure of cloth stretching under fast
motion:

```cpp
bool  useNearestNeighborDistanceConstraints;
float nndcAllowedExtension;      // e.g. 0.1 = 10 %
float nndcMaximumShiftFactor;    // e.g. 0.5 -> half way to closest neighbour per iteration
float nndcShiftCollisionFactor;
```

**Substepping and solver budget:**

```cpp
float timeStep;                     // pseudo-fixed step
int   timeStepsMax;                 // cap on substeps in one frame
int   numIterations;                // positional stiffness & collision solver iterations
int   collideEveryNthStep;          // ← collide less often than you integrate
float collisionMultipleShiftFactor; // multi-contact particles shift along the average direction
```

`collideEveryNthStep` is the cheap win: integration is cheap, collision is not, so
run the constraint solver every substep and collision every N.

**The LOD story** — this is the part worth copying, because it is a five-way
fallback rather than a distance cutoff:

```cpp
bool  forceSkinning;                    // hard override
float forceSkinningFpsThreshold;        // frame rate drops → skin instead
float forceSkinningTranslateThreshold;  // character teleports/moves too far → skin instead
bool  checkAnimationRewind;             // animation rewound → re-init cloth to collision proxies
float disableSimulationAtDistance;      // camera distance cutoff
float disableSimulationTimeRange;       // ← fade between skinning and simulation over time
float enableSimulationSSaxisSizePerc;   // enable if the character's bbox exceeds N% of viewport
```

Six independent reasons to stop simulating — **frame rate, teleport distance,
animation rewind, camera distance, screen size**, plus a manual force — and a
*time range* over which the two blend, so the cross-fade is never a pop. The
screen-size criterion (`enableSimulationSSaxisSizePerc`) is better than distance
for a game with variable FOV or a zoom.

Backed by the global valve already noted:

```
ca_VClothMode 1                     0 = off entirely, 1 = on, 2 = force skinning
ca_ClothForceSkinningAfterNFrames 3 "safety mechanism: if framerate falls below
                                     threshold for n-frames, skinning is forced"
```

**Three frames of bad frame rate and the cloth turns itself off.** That is the
right instinct for any optional simulation: it should have an opinion about the
frame budget and a way to give up gracefully. See
[`rigging_ik.md`](../../topics/animation/rigging_ik.md) and
[`networked_animation_physics.md`](../../topics/animation/networked_animation_physics.md).

`AttachmentVClothPreProcess` + `AttachmentVClothPreProcessDijkstra` do the offline
half: building the constraint graph and computing geodesic distances from the
attached vertices outward (that is what the Dijkstra is for), which is how
`stiffnessDecayAnim`-style falloffs know how far from an anchor each vertex is.

---

## 17. Ropes, soft bodies and destructible lattices

Not rendering, but asked for, and CryPhysics is the least-documented part of the
engine. All of this is read from `Code/CryEngine/CryPhysics/` and
`CryCommon/CryPhysics/physinterface.h` [SRC567].

CryPhysics has seven entity types: rigid, articulated, living, particle, **rope**,
**soft**, wheeled vehicle. Ropes and soft bodies are *first-class simulation
types*, not constraints assembled from rigid bodies — which is why they are as
capable as they are.

### 17.1 Ropes — two solvers, and the mesh rope is a render node on top

`pe_params_rope` is the public surface, and it is large. The parts that matter:

**Two operating modes**, and most of the parameters are documented as belonging
to one or the other:

| mode | how it solves | tuning |
|---|---|---|
| **no subdivision** | fixed segment count, rotational unprojection per frame | `unprojLimit` ("rotational unprojection limit per frame"), `noCollDist` ("fraction of the segment near the attachment point that doesn't collide") |
| **dynamic subdivision** | segments are split as needed, a penalty solver runs over sub-vertices in the strained state | `nMaxSubVtx` ("maximum internal vertices per segment"), `penaltyScale`, `attachmentZone` ("don't register solver contacts within this distance around attachment points"), `minSegLen` ("delete segments below this length") |

`length` is documented as *"'target' length; 0 is allowed for ropes with dynamic
subdivision"* — a subdivided rope can have no fixed length at all, which is what
lets it wrap round obstacles.

**Two frictions, not one:**

```cpp
float friction;      // "friction for free state and lateral friction in strained state"
float frictionPull;  // "friction in pull direction in strained state"
```

A rope over a pulley slides *along* itself differently from how it slides
*sideways*. One coefficient cannot express that, and a winch or a pulley behaves
wrongly without the split.

**Three target-pose modes**, the same "simulation as perturbation" idea as VCloth
(§16.5) but with an explicitly physical option:

```cpp
int bTargetPoseActive;  // 0 - no target pose (no shape-preservation stiffness)
                        // 1 - simplified target pose (vertices pulled directly to targets)
                        // 2 - physically correct target pose (the rope applies
                        //     penalty torques at joints)
float stiffnessAnim;      // shape-preservation stiffness
float stiffnessDecayAnim; // "final shape stiffness interpolated from full to
                          //  full*(1-decay) at the end"
float dampingAnim;
```

Mode 1 is cheap and can violate momentum; mode 2 is correct and costs torques.
Exposing both, and saying which is which in the comment, is the honest way to
ship a shortcut.

**Environment coupling** is built in rather than bolted on:

```cpp
Vec3  wind;             // local wind, in addition to phys-area wind
float windVariance;
float airResistance;    // "needs to be >0 in order to be affected by the wind"
float waterResistance;  // medium resistance when underwater
float density;          // "used only to compute buoyancy"
```

So a rope is affected by wind volumes, floats, and drags underwater, with a
per-rope wind override on top of the global field.

**Breaking and re-attachment:**

```cpp
float maxForce;      // "when breached, the rope will detach itself unless rope_no_tears is set"
float sensorRadius;  // "size of the sensor used to re-attach the rope if the host entity breaks"
```

`sensorRadius` is the detail that shows this was built for a destructible game: if
the *thing the rope is tied to* shatters, the rope sweeps a sensor of that radius
and re-ties itself to whichever fragment is now there. Without it, every rope in
a Crysis level would fall to the floor the moment a plank broke.

**Joint limits with a decay along the length:**

```cpp
float jointLimit;      // "joint rotation limit (doesn't work when both ends are tied)"
float jointLimitDecay; // "joint limit change (0..1) towards the unattached rope end;
                       //  can be positive or negative"
```

A cable can be stiff at the anchor and floppy at the free end — one parameter,
and it is signed so it can go the other way for a whip.

**Collision filtering:** `flagsCollider` ("only collide with entity parts flagged
this way"), `collTypes` (a mask of `ent_*` classes) and `collisionBBox[2]` (a
proximity-query box in the host's space). Three levels of narrowing before the
solver sees anything — the "cull cheaply before testing expensively" rule from
this project's own CLAUDE.md, applied to a physics broadphase.

**"Custom mesh rope"** is `RopeRenderNode` [SRC567] — a render node that takes the
rope's simulated vertex chain and builds geometry from it each frame. It carries
the full render-node feature set: it takes `ERF_FOB_ALLOW_TERRAIN_LAYER_BLEND`
like any other object (§13.7), casts shadows, and participates in the normal
material system. **The physics rope and the visual rope are separate objects**,
which is what lets a rope be a simulated chain of 20 segments and a rendered tube
of several hundred vertices.

### 17.2 Soft bodies — cloth, flags and inflatables in one solver

`pe_params_softbody` drives `CSoftEntity`, a vertex/edge mass-spring system
(`se_edge`, `m_pVtxEdges`) distinct from the character VCloth solver. This is the
one used for physicalised cloth *objects* — flags, banners, tarpaulins, awnings —
rather than garments on a skeleton.

```cpp
float ks;                 // stiffness against stretching;
                          // "<0 means fraction of maximum stable"   ← note this
float kdRatio;            // damping in stretch direction, "in fractions of 0-oscillation damping"
float thickness;          // collision thickness
float maxSafeStep;        // time step cap
int   nMaxIters;          // "complexity = O(nMaxIters * numVertices)"
float accuracy;           // solver accuracy (velocity)
float shapeStiffnessNorm; // resistance to bending
float shapeStiffnessTang; // resistance to shearing
float massDecay;          // "decreases mass from attached points to free ends;
                          //  mass_free = mass_attached/(1+decay) (can improve stability)"
float hostSpaceSim;       // 0 = world-space simulation, 1 = fully host-space
```

Four of those are worth taking on their own:

**`ks < 0` means "a fraction of the maximum stable stiffness".** Rather than
making the artist find the value at which the solver explodes, the solver
computes it and the artist asks for 80% of it. **Parameterise against the
stability limit, not against absolute units** — that is a genuinely good API
decision and it generalises to any explicit integrator.

**`massDecay`** — vertices get lighter the further they are from an anchor, with
the exact formula in the comment. Lighter free ends are more stable *and* look
more like fabric, because real hanging cloth carries the weight of everything
below it at the top.

**`nMaxIters` documented with its complexity class** — `O(nMaxIters × numVertices)`
written into the header, so a designer raising it knows what they are buying.

**`hostSpaceSim` 0..1** — a continuous blend between simulating in world space and
in the host object's space. At 1 the cloth ignores the host's motion entirely
(no inertia, perfectly stable on a moving vehicle); at 0 it feels every
acceleration. A dial between "correct" and "doesn't explode in a moving truck",
which is a trade every cloth system has to make and most hide.

**Pressurised closed bodies** — the same solver does inflatables:

```cpp
float pressure;      // ">0 tells that the object is closed and applies this pressure
                     //  from inside in the original state (proportionally more when compressed)"
float dpressure;     // pressure change speed
float densityInside; // "for closed objects, density of the medium inside;
                     //  -1 to match outside air density"
```

`densityInside = -1` matching outside air is how a balloon becomes neutrally
buoyant without a special case.

Target-pose pulling appears here too (`stiffnessAnim`, `stiffnessDecayAnim`,
`dampingAnim`, `maxDistAnim` — *"max deviation from the target pose at the rim"*),
so soft bodies also simulate as a bounded perturbation of an authored shape.
`BakeCurrentPose()` freezes the current simulated state as the new rest state.

### 17.3 Tetrahedral lattices — the destruction system

`tetrlattice.cpp/h` is a **tetrahedral lattice** over a mesh with per-tetrahedron
stress evaluation — the structure behind CryEngine's procedural breaking. An
impulse propagates through the lattice, tetrahedra whose stress exceeds a
threshold fail, and the mesh is split along the resulting boundary. `boolean3d.cpp`
does the mesh CSG that produces the two halves.

This is the piece behind Crysis's breakable architecture, and it is worth knowing
exists because it is a fundamentally different approach from pre-fractured
chunks: **the break pattern is computed from where and how hard the object was
hit**, not selected from authored variants. It is also why `pe_params_rope`
needs `sensorRadius` (§17.1) — in a world where the thing you tied to can be
computed away, attachments need to be able to find a new host.

> **For this project:** ropes and soft bodies are out of scope, and Jolt is
> already the chosen physics library. The transferable parts are the *API
> decisions*, not the solvers: `ks < 0` meaning a fraction of the stability limit,
> `hostSpaceSim` as a continuous world/local blend, complexity classes written
> into parameter comments, and simulation-as-perturbation-of-an-authored-pose with
> a decay along the structure. Those apply to anything with a solver in it.

---

## 18. Particles

Two systems in the tree:

- **Legacy particles** — `ParticleEmitter`, `ParticleContainer`, `ParticleRender`,
  CPU-simulated, sorted, rendered as camera-facing quads.
- **ParticleSystem/** — 81 files, the newer modular "features" system where an
  emitter is a stack of composable features.
- **GPU particles** — `GpuParticles.cfx`, `GpuParticlesCommon.cfi`,
  `GpuPhysicsParticleFluid.cfx`, `GpuCollisionScreenSpace.cfi`, plus
  `BitonicSort.cfx` and `GpuMergeSort.cfx` for depth sorting on the GPU. There is
  a **screen-space collision** path — GPU particles collide against the depth
  buffer — and a **particle fluid** solver.
- `ParticleImposter.cfx` + `ParticleImposter.ext` — impostor rendering for
  particles.
- **Half-resolution particles**: `SceneForward::ExecuteTransparentLoRes` runs at
  1 or 2 passes depending on `r_ParticlesHalfResAmount`, with a
  `ExecuteTransparentDepthFixup` before it.
- **Particle tessellation** — `r_ParticlesTessellation 1`, 16-pixel triangles,
  *"for higher quality lighting"*: a tessellated particle quad gets per-vertex
  lighting at a useful density instead of four corners.

---

## 19. Networking

Not a rendering topic, but asked for, and CryNetwork is genuinely unusual. 290
files.

### 18.1 Aspect-based replication

The unit of replication is an **aspect** — a numbered slice of an entity's state
(physics, script, health, …) that serialises independently. From
`INetwork.h` [SRC567], the aspect flags:

- *"Aspect will not be sent to clients that don't control the entity"*
- *"Aspect is serialized without using compression manager (useful for data that
  is already well quantised/compressed)"*
- *"Aspect can be client controlled (delegated to the client)"* — client
  authority, per aspect
- *"Aspect has more than one profile (serialization format)"* — an aspect can
  change its *format* at runtime; the canonical case is a physics aspect
  switching between "alive, animated" and "ragdoll"
- *"Aspect needs a timestamp to make sense (i.e. physics)"*

**Per-aspect profiles and per-aspect client authority** is a stronger model than
"replicate these properties", and it is why CryNetwork can hand physics authority
to a client for the pawn it controls while keeping everything else
server-authoritative.

### 18.2 Arithmetic coding, per field

`Compression/` is 70 files, and this is the part that is rare:

```
ArithModel, ArithAlphabet, ArithPrimitives      ← arithmetic coding core
AdaptiveFloat / AdaptiveFloatPolicy
AdaptiveVec3Policy, AdaptiveUnitVec3Policy
AdaptiveOrientationPolicy
AdaptiveVelocity / AdaptiveVelocityPolicy
AdaptiveBoolPolicy / AdaptiveBoolPolicy2, BoolCompress / BoolCompress2
BiggerOrSmallerPolicy, EntityIdPolicy, FloatAsIntPolicy
ErrorDistributionEncoding
CompressionManager
```

Every replicated field gets a **compression policy**, chosen in
`Config/DefaultScripts/CompressionPolicy.xml` [SRC57], and the "Adaptive" ones
build a **probability model from observed traffic and arithmetic-code against
it**. A velocity that is usually near zero costs a fraction of a bit; the rare
large value costs more. `ErrorDistributionEncoding` goes further and models the
*prediction error* distribution.

That is a level of bit-shaving most engines skip entirely, and it dates from
CryEngine's console-era bandwidth budgets:

```
net_defaultChannelBitRateDesired    200000 bits/s   ← 25 KB/s per channel
net_defaultChannelPacketRateDesired     50 packets/s
net_defaultChannelIdlePacketRateDesired  0.05
net_defaultChannelBitRateToleranceLow/High, …PacketRateToleranceLow/High
```

**25 KB/s and 50 Hz per channel, with tolerances**, and a scheduler
(`Config/DefaultScripts/Scheduler.xml`) that prioritises messages to fit. There
is a `HIGH_PRIORITY_ASPECT_MASK` with a *"'hack' scheduling policy group"* for
aspects that must not be delayed.

### 18.3 The rest

- **Contexts** (`INetContext`) manage the set of synchronised objects, with an
  explicit state machine — `eCVS_Begin` → `EstablishContext` → `ConfigureContext`
  → … — so level loads and map changes are a first-class network operation.
- **Demo recording** is built into the context (`record this context as a demo
  file`), not bolted on.
- **VOIP** (10 files) and an `IVoiceContext`.
- **Debug tooling** is unusually complete: `NetDebugChannelViewer`,
  `NetDebugTrafficBandwidth`, `NetDebugProfileViewer`, `NetDebugServerInfo`,
  `NetDebugInternetSimulator` (built-in latency/loss simulation),
  `DistributedLogger`.
- Transport is UDP with its own reliability layer (`Protocol/`, 32 files) — not
  ENet, not a third-party stack.

Compare [`mmo_architecture.md`](../../topics/scale/mmo_architecture.md) and
[`networked_animation_physics.md`](../../topics/animation/networked_animation_physics.md).

---

## 20. The 5.11 delta — what Hunt: Showdown 1896 changed

Everything above describes 5.6.7/5.7. This section is what is known to have
changed by 5.11, and **all of it is interview or observation, none of it is
source.** Graded accordingly.

| change | evidence | note |
|---|---|---|
| **DX11 → bindless DX12 renderer** | Crytek CTO Clive Gratton, interview [3P] | The largest change. 5.6.7's DX12 backend exists (112 files) but the resource model is still slot-based; "bindless" implies a descriptor-heap redesign. |
| **FSR 2.1.2** | shipped graphics menu [3P] | Nothing in the AA enum (§10) — a new upscaling path, not a fifth AA mode. |
| **HDR output** | shipped [3P] | |
| **Improved global illumination**, "better lighting in shadows" | Crytek [3P] | Almost certainly SVOGI work; no detail published. |
| **Water rendering with fluid simulation** | Crytek [3P] | Squares with §12.1 — the shipped ocean was the weak point. |
| **Native 4K/60 on PS5 and Series X**, 1440p/60 on Series S | Crytek [3P] | Digital Foundry measured ~90% of the time at 60 fps, with drops on view/vision-mode changes [3P]. |
| Texture, lighting and animation quality passes | Crytek [3P] | Unquantified. |

**What this does not tell us** is whether the tiled deferred architecture,
the 8×8 tile size, the 255-light cap, the three-RT GBuffer or the SMAA/TSAA
family survived. A bindless DX12 rewrite is exactly the sort of change that would
motivate revisiting the 255-light array and the slot-bound `TextureCubeArray`
probes, but that is speculation. **[inferred]**

If this is ever revisited: Hunt's executable will carry the full `r_*`/`e_*` CVar
table *with help strings* — CryEngine registers them at runtime with the
descriptive text quoted throughout this document, so a plain string dump of the
shipped binary would produce a directly comparable inventory and settle most of
the questions above without touching the anti-cheat. That is the method used in
[`frostbite_rendering.md`](frostbite_rendering.md) §0.2 and it works here for the
same reason.

---

## 21. What is worth taking, and what is not

Ranked by what it would actually buy this project.

### Take

1. **HeightMapAO (§7.2).** A 2048² heightmap over 1 km giving large-scale ambient
   occlusion, generated from the cached shadow frustum. A tile game already has
   the heightmap. Stable, cheap, and it supplies the octave screen-space AO
   cannot. **The highest value-per-effort item in this document.**
2. **High-passing the tiling detail layer (§13.3).** Unique low-frequency base ×
   high-passed tiling detail. No triplanar, no stochastic sampling, no virtual
   texture. It is what CryEngine ships and it is the cheapest of the three known
   answers to tiling.
3. **Split AO strengths per lighting term (§7.1).** `AmountDirect 2.0`,
   `AmountAmbient 1.0`, `AmountReflection 1.5`. One extra multiply, visibly
   better contact shadowing.
4. **The shadow cache with a per-frame repair budget (§4.2).** 50 nodes/frame,
   characters excluded. Independently arrived at by RE ENGINE, which is the
   strongest kind of corroboration.
5. **Decal sort by state then order, and neighbour-fade lifetimes (§11).**
   Directly applicable to `src/cromwell/decal/` today.
6. **Two LOD criteria for terrain: distance *and* geometric error (§13.1).**
7. **Normal-variance baked into roughness mips (§2.4).** The fix for specular
   sparkle, done at bake time, costs nothing at runtime.
8. **Temporal LOD on expensive passes (§12.2).** Update rate proportional to
   screen coverage, with a hard cutoff. Generalises to any slow-changing
   expensive render.
9. **One light list, several culling structures (§2.3, §9.1).** The `Fwd_`-prefixed
   bindings mean the *forward* path consumes the same tile list as the deferred
   kernel; the volumetric fog builds a **4×4×4 clustered grid** from the same
   255-light array. Cull once, express the result in whatever structure each
   consumer needs. This is the piece that makes
   [`clustered_forward_lighting.md`](../../plans/clustered_forward_lighting.md)
   compatible with keeping a deferred path for the bulk of the scene.
10. **Terrain colour blending in the GBuffer pass (§13.7).** Five attributes —
    albedo, normal, reflectance, smoothness, transmittance — each lerped toward
    the terrain's own with its own artist weight and a distance-driven factor. A
    handful of lerps in a pass that already runs, and it is the single biggest
    contributor to distant objects sitting in the world rather than on it.
11. **Per-froxel jitter plus temporal reprojection (§9.1).** 32 depth slices look
    continuous only because every sample within a froxel is offset by a
    per-frame jitter and integrated by a 0.9 history blend. The technique is what
    buys the low slice count.
12. **Simulation as a bounded perturbation of an authored pose (§16.5, §17).**
    VCloth's `pullStiffness` toward the skinned position, the rope's three
    target-pose modes, the soft body's `maxDistAnim` rim clamp. A simulation that
    cannot drift far from what the artist authored is one that can ship. The
    companion idea — `ks < 0` meaning "a fraction of the maximum *stable*
    stiffness" (§17.2) — is how to expose a solver parameter without asking
    anyone to find the explosion point by hand.

### Note but do not take

- **SVOGI (§5).** Magnificent and wrong for this project. It solves "dynamic GI
  in an unprepared scene at arbitrary scale"; a tile game has a known, mostly
  static world where the SDF/probe approach in
  [`indirect_lighting.md`](../../plans/indirect_lighting.md) is far cheaper. The
  *modes* are worth stealing though — particularly mode 1, correcting probe
  specular by the ratio of traced to probed diffuse (§5.2).
- **Mesh ray tracing over an SVO (§5.4).** Interesting as an existence proof that
  you can skip the BVH if you already have a voxel structure. Not needed.
- **CryNetwork's arithmetic coding (§19.2).** Correct for a 25 KB/s console
  budget in 2011. Modern bandwidth makes per-field adaptive arithmetic coding a
  large amount of machinery for a small win. **The aspect model with per-aspect
  profiles and client authority is the part worth keeping** (§19.1).
- **Geometry caches (§16.4).** The quantisation ladder (uint16 positions in bbox
  space, QTangents at 4×10 bits, LZ4 HC over Deflate for decode speed) is a
  reusable recipe even if the Alembic pipeline is not. Reach for it if a baked
  simulation ever needs to play back; reach for a VAT if it is short.
- **Hardware tessellation (§13.5).** Twelve `*Tess.cfi` companion files is the
  maintenance cost, for a 30 m effect radius. POM buys most of the same look for
  none of that.

### Explicitly absent, which is itself information

Searched and **not present** in the complete 5.7 shader library or the 5.6.7
engine source:

- **Triplanar mapping** — no shader references it.
- **Stochastic / hex-tile / histogram-preserving texturing** — none.
- **Virtual texturing** — an `AdvVirtualTexTopics` deck exists from the CryEngine
  2 era, but there is no VT in the shipped 5.x renderer.
- **Vertex animation textures** — geometry caches instead (§16.4).
- **Octahedral or multi-angle impostors** — one baked quad (§14.4).
- **Motion matching** — parametric blend spaces (LMG) instead (§16.2).
- **Mesh shaders / GPU-driven culling** — coverage buffer on a CPU thread (§15).
- **DLSS / FSR / XeSS** in the public source — FSR arrives in 5.11 (§20).
- **Clustered (3D) light culling *for opaque shading*** — opaque is 2D tiles only
  (§2.1). But the engine does have a real 4×4×4 clustered light grid; it is used
  only by the volumetric fog (§9.1). The machinery exists, the opaque path just
  does not use it.

---

## 22. Sources

**Primary — source code and shipped data**

- CRYENGINE Community Edition 1.0 (3 Oct 2025), MIT-licensed patch over 5.7 LTS,
  Pterosoft — [github.com/Pterosoft/-CRYENGINE-Community-Edition-](https://github.com/Pterosoft/-CRYENGINE-Community-Edition-).
  Redistributes the full `Engine/Shaders` tree and `Engine/Config`. **[SRC57], [CFG]**
- CRYENGINE 5.6.7 full engine source, public GitHub mirror —
  [github.com/derplayer/CRYENGINE-5.6.7](https://github.com/derplayer/CRYENGINE-5.6.7). **[SRC567]**
- Crytek's own repository landing page confirming source moved private with 5.7 LTS —
  [github.com/CRYTEK/CRYENGINE_ReadMe](https://github.com/CRYTEK/CRYENGINE_ReadMe)

**Talks and papers** — all from the Internet Archive's
[`crytek_presentations`](https://archive.org/details/crytek_presentations) collection
unless noted

- [GDC14] Nicolas Schulz, *Moving to the Next Generation — The Rendering Technology of Ryse*, GDC 2014
- [SIG14-GC] Axel Gneiting, *Real-Time Geometry Caches*, SIGGRAPH 2014
- Nicolas Schulz & Theodor Mader, *Rendering Techniques in Ryse: Son of Rome*, SIGGRAPH 2014 Advances — [advances.realtimerendering.com/s2014/](https://advances.realtimerendering.com/s2014/)
- [SIG13] Tiago Sousa, *CryENGINE 3 Graphics Gems*, SIGGRAPH 2013
- [SIG13-SH] Sousa / Andreev / Kasyan, *Playing with Real-Time Shadows*, SIGGRAPH 2013
- [C3] Tiago Sousa, *Rendering Technologies of Crysis 3*, GDC 2013
- [GDCE13] Pierre-Yves Donzallaz, *Shining the Light on Crysis 3*, GDC Europe 2013
- [SIG11] Ivo Zoltan Frey, *Spherical Skinning with Dual Quaternions and QTangents*, SIGGRAPH 2011
- Sousa, *Anti-Aliasing Methods in CryENGINE 3*, SIGGRAPH 2011
- Kaplanyan, *Light Propagation Volumes in CryEngine 3*, I3D 2010 — historical (§5.5)
- Mittring, *Finding Next Gen — CryEngine 2*, SIGGRAPH 2007

**Crytek official**

- [How we made Neon Noir — Ray Traced Reflections in CRYENGINE](https://www.cryengine.com/news/view/how-we-made-neon-noir-ray-traced-reflections-in-cryengine-and-more) **[CE]**
- [Voxel-Based Global Illumination (SVOGI)](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756816/pages/25535599) **[CE]**
- [Terrain.Layer Shader](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756816/pages/29449280) **[CE]**
- [CRYENGINE 5.7 Long Term Support is here](https://www.cryengine.com/news/view/cryengine-5-7-long-term-support-is-here) and [Hotfix: 5.7.1 LTS](https://www.cryengine.com/news/view/hotfix-today-cryengine-5-7-1-lts-is-here) **[CE]**
- [News archive](https://www.cryengine.com/news/archive/update) — no engine release post after 19 May 2022 **[CE]**

**Third party**

- Digital Foundry, *Hunt: Showdown 1896 — PS5/Xbox Series X|S Tech Review: A CryEngine Revamp For Current-Gen* — [youtube.com/watch?v=sTSHrtgsQvM](https://www.youtube.com/watch?v=sTSHrtgsQvM) **[3P]**
- Crytek developer interviews on the 5.11 upgrade — [Shacknews](https://www.shacknews.com/article/141007/hunt-showdown-1896-mammons-gulch-dev-interview), [PlayStation LifeStyle](https://www.playstationlifestyle.net/2024/08/15/hunt-showdown-1896-interview/) **[3P]**
- [CryEngine 5.7 Community Edition Released](https://gamefromscratch.com/cryengine-5-7-community-edition-released/), GameFromScratch **[3P]**
