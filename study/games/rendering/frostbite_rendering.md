# Frostbite rendering — reference notes

How EA's in-house engine draws what it draws, read for the same reason as
[`re_engine_rendering.md`](re_engine_rendering.md) and
[`rdr2_atmospherics.md`](rdr2_atmospherics.md): the transferable part is
architectural.

Frostbite has been talked about publicly since 2010, and **most writing about it
describes a renderer two or three generations old**. The 2014 PBR course notes
and the 2015 volumetrics talk are still the first hits for "Frostbite rendering",
still cited as current, and both are now **eleven and ten years old**. This
document is organised so the recent material leads and the classics are placed as
background with an explicit verdict on each — *still current*, *superseded*, or
*unknown*.

| tag | source |
|---|---|
| **[SIG26]** | SIGGRAPH 2026 Advances in Real-Time Rendering, **21 July 2026** — *Speeding Up Path Tracing via ORCA (Online Radiance Cache Acceleration)*, Jon Greenberg, **EA SEED**. The newest EA rendering publication. **Research in Halcyon, not shipped Frostbite** — see §0.3. |
| **[SIG24]** | SIGGRAPH 2024 Advances, **30 July 2024** — two Frostbite talks: *Shipping Dynamic Global Illumination in Frostbite* (Diede Apers) and *Flexible and Extensible Shader Authoring in Frostbite with Serac* (Simon Taylor). **The single most load-bearing source here.** The GI talk's PDF is the slides *plus full speaker notes*, which is where the numbers are. |
| **[SIG21]** | SIGGRAPH 2021 Advances — *Global Illumination Based on Surfels*, Henrik Halén (SEED), Andreas Brinck, Kyle Hayward (Frostbite), Xiangshun Bei (DICE LA). The GIBS foundation talk; [SIG24] is its three-years-later update and supersedes its numbers. |
| **[SIG15]** | SIGGRAPH 2015 Advances — *Towards Unified and Physically-Based Volumetric Lighting in Frostbite* (Sébastien Hillaire) and *Stochastic Screen-Space Reflections* (Tomasz Stachowiak). Both Electronic Arts / Frostbite. |
| **[PBR14]** | SIGGRAPH 2014 *Physically Based Shading in Theory and Practice* course — *Moving Frostbite to PBR*, Sébastien Lagarde & Charles de Rousiers, **v3.2 course notes, 122 pages**. Note the venue: this was the **PBR course, not Advances**, which is why it does not appear in Advances year listings. |
| **[SIG16-PBR]** | SIGGRAPH 2016 PBR course — *Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite*, Sébastien Hillaire. **Cited but not retrieved** — every claim tagged with it is second-hand and marked as such (§8.1, §9). |
| **[SIG10]** | SIGGRAPH 2010 Advances — *Destruction Masking in Frostbite 2 using Volume Distance Fields*, Per Einarsson & Robert Kihl (DICE). Recorded only to date the destruction gap in §7.5. |
| **[EA]** | EA engineering blog posts under ea.com/technology. Written for a wider audience than a SIGGRAPH deck but sourced to named engineers and carrying real budgets. |
| **[BIN]** | **Our own read of the shipped Battlefield 6 executable.** A different evidence class from everything above: not what EA said, but what the binary contains. Scope and limits in §0.2. Full listing in [`frostbite_bf6_shader_programs.txt`](frostbite_bf6_shader_programs.txt). |
| **[3P]** | Third-party technical analysis and shipped graphics menus. Observation of output, never a claim about implementation. |
| **[inferred]** | Our reading, not EA's. |

---

## 0. Method, and what this document is not

### 0.1 No frame capture was taken

The strongest available evidence would have been a RenderDoc capture. It was not
taken, deliberately.

**Battlefield 6 ships EA Javelin kernel-level anti-cheat** — `EAAntiCheat.
GameServiceLauncher.exe`, `EAAntiCheat.Installer.exe` and an
`EAJavelinInstaller_installscript.vdf` sit in the install root, and the game is
launched through that service. Injecting a graphics debugger into a process
guarded by a kernel anti-cheat is the exact behaviour such a system exists to
detect, and the cost of being wrong is the user's account, not a wasted
afternoon. **The user was asked and chose to skip the live capture.**

The consequence is specific and worth stating up front: **this document has
algorithms and structures for the current generation, but very few current-
generation resolutions, formats and per-pass costs.** Where a millisecond figure
appears below it is almost always from a *talk*, and it belongs to whatever title
and platform that talk was about. The 2015 volumetrics numbers are PS4 numbers
from 2015 and are recorded as history, not as a description of BF6.

> **If this is ever revisited:** Battlefield 6 has a separate single-player build
> under `SP/` with its own `bf6.exe` and **no `EAAntiCheat.*` files beside it**.
> That is a plausible lower-risk capture target, but Javelin installs as a
> system-wide service and we did not verify that it stays out of the SP process —
> so "plausible" is as far as this goes. Older anti-cheat-free Frostbite titles
> (Star Wars Battlefront II, 2017, Frostbite 3; Bad Company 2, 2010, Frostbite
> 1.5) are safe to capture but are the wrong generation for this document's
> purpose.

### 0.2 What [BIN] evidence can and cannot support

Everything tagged [BIN] comes from a **plain string dump of the retail
`bf6.exe`** — a 195 MB executable, build stamped 2025-08-11, yielding 125,465
unique strings. No process was injected into, no code was run, no packaged game
data was decrypted. Frostbite leaves shader-program reflection names and its
settings-key names in the binary, so **1,069 `ShaderProgram_*` entry points** and
several thousand configuration keys are readable directly.

This is strong evidence of a very particular and limited kind:

| [BIN] supports | [BIN] does **not** support |
|---|---|
| A pass by that name was **compiled into this build** | That it **runs** in any given frame, on any platform, or at all |
| The **structure** of a system — that light culling has four passes named `Pass1_TransformVertices` … `Pass4_TriangleWindingPrepass` | The **order** those passes execute in, or what feeds what |
| That a **knob exists** — `LightCullZBinBits`, `CascadeCount` | The **value** that knob ships with |
| Which **techniques are present**, e.g. that both a surfel GI path and a legacy probe-grid path are compiled in | Which one a given **title or quality level** actually selects |
| Naming that **corroborates or contradicts** a published claim | Anything about **resolution, buffer format or cost** |

One trap avoided: the render DLL contains the **complete DXGI format enum table**
(BC1–BC7, ASTC, ETC2/EAC, R11G11B10_FLOAT, R16G16B16A16_FLOAT, D32_FLOAT …).
That is the platform layer's list of *supported* formats — it says nothing about
which format any particular buffer uses, and the ETC2/EAC entries tell you
Frostbite targets mobile, not that Battlefield 6 does. **No buffer format in this
document is sourced from that table.**

### 0.3 "Frostbite does X" is usually the wrong sentence

Frostbite is not one renderer. [SIG24] is explicit that it is deliberately not
one, and the Serac talk's opening is the clearest statement EA have published on
this. Simon Taylor, on two screenshots from the same game:

> "This slide shows 2 screenshots from FC 24: one of gameplay on Switch, one from
> a replay mode on PC. They look very different, and are rendered very
> differently – they use different shading modes (forward vs deferred),
> completely different shadowing solutions, etc.
>
> The upshot of all this variety is that there is no 'one-size-fits-all' solution
> in Frostbite – the engine needs to be flexible and extensible to allow our game
> teams to achieve the look they are going for and hit their performance
> targets." — [SIG24]

**Forward and deferred, in one title, across platforms.** Any claim of the form
"Frostbite is a deferred engine" is wrong at the granularity that matters. The
titles the engine was serving as of the 2024 talk, in EA's own list: Battlefield
2042, Dead Space, Madden, UFC, Need for Speed Unbound, Dragon Age: The Veilguard,
EA Sports FC. [SIG24]

Accordingly, **every figure below is tagged with its title, year and platform.**
A number from College Football 25 on Xbox Series X is not a "Frostbite" number.

---

## 1. Lighting

### 1.1 Tiled deferred, plus Z-binning, plus a geometric culling stage

**Status: tiled deferred is confirmed current in Battlefield 6 (2025), and the
culling in front of it is considerably more elaborate than the published record
describes.**

The classic statement is 2015, from the volumetrics talk, and it is a parenthesis
rather than a claim:

> "we are reusing the forward light tile list culling result to accelerate the
> scattered light evaluation (remember, **Frostbite is a tile based deferred
> lighting engine**)." — [SIG15]

That is still the shape in 2025. [BIN] shows a `DeferredLightTile_*` family as
the lighting core:

| entry point [BIN] | what the name says it does |
|---|---|
| `DeferredLightTile_CullCS` | the tile cull |
| `DeferredLightTile_CullRefineUsingBoundingSphereCS` | second-pass refine against a sphere bound |
| `DeferredLightTile_CullRefineUsingShadowCS` | refine using the light's shadow map — cull a light out of a tile its shadow proves it cannot reach |
| `DeferredLightTile_LightingCS` | the shading pass |
| `DeferredLightTile_LightingCS_Reference` | a reference implementation kept beside the fast one |
| `DeferredLightTile_BuildIndirectLightingBuffersCS` | where GI is injected — see §1.5 |
| `LightTileMinMaxDepthCS`, `LightTileMaterialClassifyCS` | depth bounds per tile; material classification per tile |

Two things sit in front of that, and neither appears in any EA publication we
found.

**Z-binning.** `LightCull_ZBin_Populate` and the setting `LightCullZBinBits`
[BIN]. A 2D tile grid plus a depth binning pass is the standard way a tiled
renderer gets clustered-shading behaviour without a full 3D cluster grid. So the
honest description of Battlefield 2025 is **tiled with Z-binning** — which is
functionally clustered, and is neither the plain 2D tiling of the 2015 talk nor a
froxel cluster grid. [BIN] + [inferred] for the interpretation.

**A software rasteriser that culls lights by their actual geometry.** This is the
surprise. [BIN] shows a four-pass `LightCull_*` pipeline that transforms
vertices, clips triangles, bins them, and rasterises them into a bitset:

```
LightCull_Pass1_TransformVertices
LightCull_Pass1_TransformVertsGenerateClipflags
LightCull_Pass1_CullAndClip
LightCull_Pass1_CullAndClip_WalkClippedTriangles
LightCull_Pass1_GenerateIndirectArgsFromResults
LightCull_Pass2_ExtractTriangleKeys
LightCull_Pass2_TriangleBinning
LightCull_Pass2_FillOffsets
LightCull_Pass2_RearrangeTriangles
LightCull_Pass3_GenerateBitsetRasterOnce
LightCull_Pass4_DeferredBitsetTriangleClipping
LightCull_Pass4_TriangleWindingPrepass
```

with settings `LightCullGeometricEnable`, `LightCullGeometricRefineEnable`,
`LightCullGeometricPrePassAsyncEnable`, `LightCullGjkEnable`,
`LightCullShadowmapRefineEnable`, `LightCullFrustumExpandDistance` and a debug
mode enumerating `Pass1_0_SourceTriangles` / `Pass1_1_CulledTriangles` /
`Pass1_2_ClippedTrianglesOnly` / `Pass1_3_AllTriangles` / `Pass2_TriangleBinning`
/ `Pass3_Bitset`. [BIN]

**What this is for** [inferred]: culling a light by its bounding sphere
over-counts badly for spot lights, long thin lights and any light whose useful
influence is a shaped volume. Rasterising the light's *actual hull geometry* into
a per-tile bitset gives a far tighter light list, at the cost of a small GPU
geometry pipeline. `LightCullGjkEnable` suggests a GJK convex-intersection path
as an alternative or a pre-filter. The presence of `_PrePassAsyncEnable` and
`_TriangleRefinePassAsyncEnable` says this is expected to overlap on the async
queue.

**This is the single most reimplementable idea in the document for a project with
many local lights, and EA appear never to have published it.** See §10.

**A light BVH.** `LightTreeBuilder_LeafNodes`, `LightTreeBuilder_InternalNodes`,
`LightTreeView_Debug` [BIN]. A light hierarchy is what you build when you need to
*importance-sample* lights rather than loop over them — the standard requirement
for ReSTIR-style reservoir sampling. It corroborates [SIG24]'s description of
reservoir sampling in the GI path (§1.5), and is the structure that makes
"choose one light out of thousands" affordable.

### 1.2 Practical light counts

**Not published, and not recoverable from [BIN].** No EA source we found states a
light count for any recent title. The 2015 volumetrics performance scene is the
only concrete figure and it is a *test scene*, not a shipping budget:

> "Sun + shadow cascade / 14 point lights / 2 with regular & volumetric shadows /
> 6 local fog volumes / All with density textures" — [SIG15], PS4, 900p

Do not read 14 as a Frostbite light budget. It is the scene Hillaire measured a
volumetrics cost against in 2015.

### 1.3 Photometric units

**Status: [PBR14], 2014. No later source contradicts it, and no later source
confirms it either — treat as probably still the foundation, unverified.**

Frostbite moved to physical light units in 2014 and the course notes give the
reasoning at length:

> "To introduce correct lighting ratios, we have adopted physical units for our
> lights in Frostbite." — [PBR14] §4.3

> "**Coherent light units** : All light should be expressed in the same units in
> order to achieve" [consistent results] — [PBR14]

The unit system is photometric, not radiometric — lumen, candela, lux, nit —
anchored on the SI definition of the candela and the CIE V(λ) photometric curve,
with the notes stating the conversion as "1 watt of a green 555nm light is 683
lumens". [PBR14] §4.3

Light types supported, 2014: **"punctual lights, photometric lights, area lights,
emissive"** surfaces. [PBR14] §4. IES profiles are the "photometric lights" of
that list.

[BIN] confirms area lights survive into 2025 as a first-class entity with their
own shadow and volumetric behaviour — `AreaLightShadowEnable`,
`AreaLightShadowLevel`, `AreaLightCastVolumetricShadowmapEnableLevel`. It says
nothing about whether the 2014 unit system is intact.

### 1.4 The BRDF

**Status: [PBR14], 2014. Unknown whether still current — see §5.**

### 1.5 Indirect lighting — GIBS is the current answer

**Status: current as of July 2024 [SIG24], and compiled into Battlefield 6
(2025) [BIN]. This is the most important section in the document.**

Frostbite carries **four** diffuse GI systems, which is itself the point:

> "Frostbite ships a lot of games, each with their own unique requirements. For
> this reason, Frostbite has several GI systems available depending on the game's
> needs. **Flux** provides high quality but fully precomputed diffuse global
> illumination while **Enlighten** provides partial dynamic updates. **GiGrid**
> is used to stream irradiance volumes in the form of probes for both systems.
>
> However, using these systems can be very costly to author during production. We
> are also limited in capability in the cases of; large and open worlds, dynamic
> environments with destruction, and procedural or user generated content."
> — [SIG24]

**GIBS** — Global Illumination Based on Surfels — is the fourth, introduced to
lift those restrictions. All four are visible in [BIN]: `GiGridProbes*` shaders
sit beside `Gibs*` and `Surfel*` ones in bf6.exe, with SH-L1 probe payloads in
three compressions (`_SHL1_DSC`, `_SHL1_LogLuv`, `_SHL1_R11G11B10F`).

#### 1.5.1 What GIBS is

Two cooperating systems, both ray-traced, both fully dynamic:

> "All inputs to our GI system are fully dynamic, we can move meshes, change
> materials and add lights at runtime. No authoring is required to run GIBS
> initially" — [SIG24]

- **Surfels** — surface elements spawned from the GBuffer, persisting across
  frames, which "**decouple the ray tracing rate from the shading rate, which is
  quite important for performance**" [SIG24]. They light static and transformable
  geometry.
- **Clipmap probes** — an octahedral irradiance clipmap "**very similar to
  DDGI**" [SIG24], sampled by anything that does not go through deferred
  lighting: transparents, crowds, characters.

Per frame: build the TLAS, spawn surfels from the GBuffer, ray-trace surfels and
a slice of probes, then "apply both the surfel and the probe lighting … by
producing an indirect light buffer that we plug into our **Deferred Tiled
Lighting** system." [SIG24] — which is exactly the
`DeferredLightTile_BuildIndirectLightingBuffersCS` entry point in [BIN].

#### 1.5.2 The 2024 probe budget, in full

This is the most concrete published Frostbite budget of the current generation.
All figures [SIG24], stated as engine-wide targets rather than per-title:

| quantity | value | source wording |
|---|---|---|
| clipmap levels | up to 3 | "we need up to 3 clips to have coverage everywhere in the level" |
| probe count target | **100k maximum** | "we need about 100k probes, which is what we target as a maximum" |
| probe storage budget | **100 MB** | "To match the memory budget we have for our Surfel system, we need to fit our probes in 100mb storage" |
| **per probe** | **~1 KB** | "which means we have about 1kb available per probe" |
| variance depth atlas | **14×14** incl. 2×2 border | "we settled on a 14x14 resolution for the variance depth, including the 2x2 border" |
| irradiance atlas | **6×6** incl. border → 4×4, **16 directions** | "a 6x6 resolution for irradiance, this gives us 4x4 with 16 directions" |
| remaining per-probe bytes | 96 | "used for filtering data and other misc. data" |
| ray budget | **100k rays/frame** | "we have 100k rays that we can dispatch" |
| probes updated per frame | **~6k** | "comes down to about 6k probes we can raytrace each frame" |

The reasoning behind the atlas split is worth keeping, because the naive
allocation is the wrong one:

> "If we had just one atlas we could fit 16x16 which includes the 2x2 border that
> is required for bilinear filtering. However, we need an atlas for both
> irradiance and depth, so we could use a 12x12 and 10x10 atlas but we found that
> **the depth resolution is more significant**." — [SIG24]

**Depth beats irradiance for a given byte budget.** The probe's job is as much
"can this point see me" as "how bright am I", and visibility is the part that
leaks when it is under-resolved.

The update scheme falls out of the ray budget arithmetic:

> "we have 100k rays that we can dispatch, which means we can only afford 1 ray
> per probe per frame. However, since we do inline convolution of our octahedral
> irradiance, we need at least 16 rays for this to work properly. This leaves us
> with having to amortize updates over the total probe count" — [SIG24]

One clipmap level per frame, probes prioritised within it — moved probes high,
probes seeing no nearby geometry low — and the top 6k updated. Update latency is
then compensated by driving probe hysteresis from the same Multi-Scale Mean
Estimator used for surfels. [SIG24]

[BIN] corroborates the whole structure: `ClipmapPriorityGroupCount`,
`ClipmapPriorityGroupRayAssign`, `ClipmapUpdateBitmask`,
`ClipmapClearIrradianceDepth`, `ClipmapCrossProbeDilation`,
`GibsRayResolveProbesBlendDepthCs` / `…BlendIrradianceCs`.

#### 1.5.3 Measured cost — Skate, Xbox Series X, 1440p, 2024

The one end-to-end current-generation cost breakdown EA have published. Camera
flythrough, **XBSX at 1440p with no upscaling**, 60 fps target, ~100k live
surfels, ~25k probes across 3 clipmaps, 200k rays/frame maximum. [SIG24]

| stage | baseline | after optimisation |
|---|---|---|
| misc. compute (not RT) | ~0.5 ms | ~0.5 ms (unaffected) |
| apply surfels to screen (quarter rate → 1440p) | ~0.5 ms | ~0.5 ms (unaffected) |
| probe update incl. ray tracing | 0.7 ms | −34% trace time |
| **surfel spawn + ray trace** | **1.64 ms** | −60% |
| **total average** | **~3.2 ms** (peak 4 ms) | **2.5 ms** (never above 3 ms) |

The optimisations, cumulatively: offline BLAS + selective rebraiding −0.35 ms;
ray limiting + shadow-map sampling −0.33 ms. [SIG24]

For scale against the previous generation of the same system:

> "this is already a significant improvement over the 6ms average we had in 2021,
> although those timing were on PS5 and in a different game." — [SIG24]

**That caveat is EA's own and it is exactly right** — the 2021 6–7 ms figure and
the 2024 3.2 ms figure are different games on different consoles. They are not a
clean before/after.

Memory, lowest spec: "on XBSS we fit within **500mb for the entirety of GIBS
including all raytracing resources**." [SIG24]

#### 1.5.4 College Football 25 — the 60 Hz compromise, and why it is instructive

CFB25 shipped July 2024, current-gen consoles only, 130+ stadiums each with
variable time of day, and "fully relies on GIBS for all indirect diffuse
lighting." [SIG24] It runs 30 fps outside gameplay and 60 fps during, with only a
**2 ms** GIBS budget at 60.

The solution is a design constraint turned into a rendering one:

> "all stadium geometry and lighting is determined once during level load. This
> allowed them to expend more surfel and ray budget during the 30hz preload
> sequence and halt any irradiance updates when going into 60hz gameplay. During
> gameplay, **GIBS is in a frozen state** and we only apply the surfel and probe
> lighting to the screen each frame, which allows us to stay under 2ms." — [SIG24]

A fully dynamic GI system, run to convergence during loading and then frozen.
[inferred] The transferable lesson is that the *apply* cost and the *update* cost
are separable, and a game that knows its lighting is static after load can pay
only the first. Note also why CFB is expensive in the first place: "ray tracing
instances have a lot of overlap and we generally do not have any lower-detail
meshes available to fallback on … This means that **every ray we trace is
expensive**." [SIG24]

#### 1.5.5 Ray tracing craft — the parts worth stealing

The acceleration-structure and traversal work in [SIG24] is the most reusable
engineering in the talk.

**What is in the BVH is not what is on screen.** "for the sake of estimating
irradiance, we only consider **opaque, and non alpha-tested** geometry … we use a
**simplified diffuse albedo** instead of evaluating the entire material graph …
simplified down to **one color per mesh-material subset**." Vegetation is absent
from the ray-traced view entirely. [SIG24]

**Offline BVH work, with measured wins.** Automatic splitting of large geometry
and merging of small; mesh data rotated to align with local axes; precomputed
BLAS offline where the platform allows — "**we observe a 15% improvement to ray
traversal** when using this for static geometry" — and selective rebraiding
driven by per-mesh offline heuristics for "**another 5% improvement on top**".
[SIG24]

**Ray shortening with a probe fallback.** Primary rays are nominally unbounded
(capped at 1000 m), which wrecks wavefront occupancy: "This results in bubbles of
execution by single rays causing their wavefront to be long-running while all
other lanes might be inactive". The fix is to cap iterations and shorten rays to
~20 m, falling back to the nearest clipmap probe when the ray reaches its end
without a hit. Biased, and better anyway:

> "Doing this is certainly biased since our probes do not have infinite
> resolution (and are storing irradiance at this point), but we actually get a
> **significant improvement to our convergence rate** on top of limiting the
> number of ray intersection we need to evaluate." — [SIG24]

**Shadow maps instead of shadow rays.** For secondary rays, N light candidates
are reduced by reservoir sampling to one, and then: "When the selected light
source has a shadow map available, **we can tap that instead of tracing a ray**,
resulting in a significant reduction of ray intersection requests." [SIG24] —
worth 34% of probe trace time.

**Inline tracing in compute, indirect args, async overlap.** "We moved our ray
tracing calls to compute and use **inline tracing much like dxr1.1**. Each ray
tracing dispatch uses indirect arguments … We always overlap our BLAS and TLAS
building in async with our gbuffer laydown." [SIG24]

Scale: "We dispatch **over 50 compute shaders each frame, and only 6 of those are
issuing ray intersections**." [SIG24]

#### 1.5.6 What GIBS tried and abandoned

Recorded because negative results are the expensive part to rediscover. All
[SIG24]:

- **Ray binning** — a win in 2021, no longer: "This is no longer the case due to
  having fewer rays with less redundancy as well as more optimized traversal."
  Compaction by ray property (e.g. needs-shadow-ray) *is* still worth it.
  ([BIN] still shows `Surfel_BinSort_RayBinning` / `_RayReorder` /
  `_BinSummation` compiled in — present in the binary, described as no longer
  paying off. A good illustration of §0.2's limits.)
- **Double-buffered probe atlases** — 1 ray/probe/frame accumulating into a back
  buffer then convolving to a front buffer. Dropped for the memory cost of
  double-buffering depth and the per-frame copies; inline convolution won.
- **Stochastic surfel apply via ReSTIR** — "in the end, the **naïve approach is
  still faster at halfres (quarter rate) with a cheap spatial upscale**,
  especially when using scalarized loads for all surfel data to reduce
  bandwidth."

**Scalarisation** is the recurring optimisation: "Evaluating surfels involves
loading a lot of structured data, for which we get a significant reduction in
data traffic by utilizing the scalar memory path where possible", targeted at
RDNA "which is the gpu architecture used on our target console platforms."
[SIG24]

#### 1.5.7 Three bugs worth knowing about

All [SIG24], all of the kind that costs a week to diagnose:

1. **Shading normals spawn too many surfels.** A tree with high normal variation
   drove the coverage heuristic to over-spawn, creating high-density acceleration-
   grid cells. Reconstructing a geometric normal from depth failed on
   unreconstructable depth planes. Fix: **export the vertex-interpolated normal
   during GBuffer laydown** — fewer surfels, same coverage. ([BIN] shows GBuffer
   permutations named `GBuffer_HighEnd_Gibs`, `GBuffer_LowEndExtended_Gibs`,
   `…_Gibs_MotionVectors` — the extra GIBS channel is a compile-time GBuffer
   variant.)
2. **Ray-offset bias is per-surfel, not global.** Surfels are reconstructed from
   the view matrix and depth buffer, "the depth-buffer is not linear with respect
   to world-space units, we end up with a **reconstruction error that is unique
   per surfel**." A global conservative offset let rays escape through a garage
   ceiling and leak light. Fix: store the error per surfel.
3. **Missing surfel coverage on newly revealed surfaces.** Previously patched
   with a per-grid-cell average fallback; now the probes provide the initial
   estimate instead, "since probes have coverage effectively everywhere, and we
   don't need extra storage or gpu cycles for a fallback anymore."

---

## 2. Shadows

**Status: no EA publication covers the current shadow pipeline. Everything below
is [BIN] structure plus one [EA] blog post about hair. This is the largest gap in
the public record — see §10.**

### 2.1 What is in Battlefield 6

| system | evidence [BIN] |
|---|---|
| **Cascaded sun shadows** | `CascadeCount`, `DebugDrawCascades`, `AccumDirectionalShadow`, `AccumulateSunShadow`, `CompensateSunShadowHeightScale`, `DetailDisplacementForShadowsMaxCascadeIndex` |
| **Local-light shadow atlas** | `LocalLightShadowAtlasSlotCount`, `LocalLightShadowAtlasSlotResolution`, `MinLocalShadowAtlasBorder`, `LocalLightShadowMoveInPlace`, `LocalLightShadowBuildMinMaxCS` |
| **Quality tiers** | `LocalLightShadowResolutionLow` / `Medium` / `High` / `Ultra` — four discrete rungs, values not in the binary |
| **Static/distant shadow cache** | `DistantShadowCacheEnable`, `DistantShadowCacheCoalesceTime`, `DistantShadowCacheDisableBakeEvents`, `DistantShadowCacheForceMode`, `AccumDistantDirectionalShadowCache`, `ShadowCacheTileDebug` |
| **Ray-traced contact shadows** | `ContactShadowsSun_RG`, `ContactShadowsLocalLight_RG`, `ContactShadows_HG`, `ContactShadows_MS` — `_RG`/`_HG`/`_MS` are DXR raygen / hit group / miss shaders |
| **Screen-space contact shadows** | `DirectionalScreenSpaceContactShadowsEnable`, `DeferredScreenSpaceShadow` — a cheaper non-RT path alongside |
| **Shadow denoising** | `AMD_Denoiser_Shadows_Prepare`, `_TileClassification`, `_FilterSoftShadowsPass0/1/2` — AMD's FidelityFX shadow denoiser, integrated rather than written in-house |
| **Transparent shadows** | `DirectionalTransparentShadow`, `DeferredScreenSpaceShadowExtractTranslucency` / `…ClearTranslucency` / `…TileTranslucency` |
| **Particle shadows** | `ParticleVertexShadowsCs`, `…AccumCs`, `…ClearCs` — per-vertex particle shadowing |
| **Cloud shadows** | `DeferredCloudShadow`, `VolumetricCloudShadowCS`, `VolumetricCloudShadowBlurCS`, `VolumetricCloudsShadowmapResolution`, `VolumetricCloudsShadowIterationCount`, `VolumetricCloudsShadowmapBlurSamples`, `VolumetricCloudsShadowTemporalCoefficient` |
| **Blob shadows** | `BlobShadow` — the cheap fallback still exists |

**Two structural notes.** First, the shadow cache is a *distant* cache with
coalescing and bake events — i.e. static geometry's shadow is rendered once into
a cached atlas and re-rendered only on invalidation, which is the standard answer
to "the far cascade re-renders the whole world every frame". Second, contact
shadows are **ray-traced with a tile-compaction front end**
(`ContactShadowsLightTileCompaction_CS`, `ContactShadowsLightTileSum_CS`) — the
rays are only spawned for tiles that need them.

**Filtering — unknown.** No PCF/PCSS/VSM/EVSM naming survives in the binary and
no publication states it. The presence of an AMD *soft shadow* denoiser implies
stochastic ray-traced soft shadows for at least some lights [inferred], but the
filter used on the ordinary shadow-map path is not recoverable. **Do not guess
this from the 2011-era Battlefield 3 material.**

### 2.2 Shadows for hair — "hero shadows"

The one shadow topic EA have written about recently, and it is specific to strand
hair. From the Veilguard post-mortem, quoting James Power, Senior Rendering
Engineer at BioWare:

> "Any given Strand Hair object, which has tens of thousands of individual thin
> hair strands, requires high quality shadow maps in order to have good coverage
> of the hair strands in the resulting shadowmap texture. Wide angle lights,
> distant lights, and non-shadowcasting lights do not provide adequate coverage …
> the hair would occupy a low amount of pixels in the shadowmap. …the fidelity
> would be poor, resulting in flat shading lacking detail near the edges of the
> hair where a fine gradient of light transmission is expected.
>
> To solve this, **hero shadows are rendered for every Strand Hair object and
> every light that lighting artists designated as important to the shot**. These
> hero shadows are generated at run time, using a **tightly fitting light frustum
> that is adjusted to each hair's bounding box** … When applying shadows to the
> hair, we test to see if a shading point is in the hero shadow or the regular
> shadow (since the hair will not be in both) and composite the final results."
> — [EA], 12 November 2024

Hero shadows are expensive, so **scalable hair decimation** was added: fewer
strands rendered into the shadow pass, "thus reducing the cost of hero shadows.
This enables lighters to use more of them, and support them for both 30 FPS and
60 FPS targets." [EA]

[BIN] confirms this shipped and is engine-side, not a BioWare fork:
`StrandHairHeroDomShadow` (the "Dom" reading as deep-opacity/dominant shadow
[inferred]), `StrandHairShadow`, `StrandHairTransparentShadow`,
`StrandHairSunShadowMask`.

---

## 3. Volumetric lighting and light shafts

**Status: the 2015 architecture is confirmed alive in 2025 — cascaded clip-space
froxel volumes with temporal integration. The 2015 *numbers* are PS4 numbers and
should not be quoted as current.**

### 3.1 The 2015 design [SIG15]

Frustum-aligned 3D textures — froxels — aligned to the light tile grid so the
tile cull can be reused:

> "This volume is also aligned with our screen light tiles. This is because we
> are reusing the forward light tile list culling result to accelerate the
> scattered light evaluation … Our volume tiles in screen space can be smaller
> than the light tiles (which are **16x16 pixels**)." — [SIG15]

| parameter | 2015 value, PS4 [SIG15] |
|---|---|
| default volume tile | **8×8** pixels (4×4 also supported) |
| depth slices | **64** |
| 720p grid | **160×90×64**, ~7 MB per RGBA-F16 texture |
| 1080p grid | **240×135×64**, ~15 MB per RGBA-F16 texture |
| light tile | 16×16 |
| scattering model | single scattering only |
| phase function | single-lobe Henyey-Greenstein (Schlick approximation available) |
| extinction | **wavelength-independent** — "deliberately chosen … to have cheaper volumes" |

The pipeline is three stages over clip-space volumes: **material properties**
(a "Vbuffer analogous to screen Gbuffer but in Volume") → **froxel light
scattering** (one sample per froxel) → **final integration** along depth. [SIG15]

**Two encoding decisions with reasons attached**, both still good advice:

> "Extinction is simply copied over from the material. You will see later why
> this is important for visual quality in the final stage (**to use extinction
> instead of transmittance for energy conservative scattering**). Extinction is
> also **linear** so it will be better to temporally integrate it instead of the
> non linear transmittance value." — [SIG15]

**Temporal integration** with Halton-jittered samples along the view ray, and the
critical detail that the jitter must be shared:

> "Jitter scattering AND material samples in sync … The material and scattered
> light samples are jittered using the same offset (to soften evaluated material
> and scattered light)" — [SIG15]

Notably, sun scattering samples the ordinary cascaded shadow maps and relies on
the temporal filter rather than a softer shadow representation: "We could use
exponential shadow maps but we do not as our **temporal up-sampling is enough to
soften the result**." [SIG15]

Volumetric shadow-map ray-marching costs, **PS4, 2015, 32³ volumetric shadow
maps**: spot 0.04 ms, point 0.14 ms; default quality 0.03 ms, high quality
0.25 ms. [SIG15]

> **The main performance table in the 2015 deck is an image**, not text, so the
> per-configuration millisecond grid for the 900p PS4 scene could not be
> recovered by text extraction. Only the scene description (§1.2) and the
> volumetric-shadow figures above survive. Flagged rather than smoothed over.

### 3.2 What survives into Battlefield 6 [BIN]

The architecture is recognisably the same one, ten years on:

| entry point / setting | reading |
|---|---|
| `DrawDebugVolumetricCascadedVolumes` | **cascaded** froxel volumes — an extension beyond the single clip-space volume of 2015 |
| `VolumetricGenerateExtinctionVolumeCS` | the material/extinction Vbuffer stage |
| `VolumetricLightVoxelCS` | the scattering stage |
| `VolumetricShadowCS` | volumetric shadow maps, still present |
| `VolumetricTaaCS` | the temporal integration, now its own compute pass |
| `VolumetricVoxelizeParticlesCS`, `VolumetricEmitterVoxelizationEnable`, `VolumetricEmitterVoxelizationMode`, `VolumetricEmitterVoxelCascadeExtinctionScale` | particle voxelisation into the volume — the "unified" half of the 2015 title, still unified |
| `DrawDebugVolumetricParticipatingMediaTemporalFilter` | the temporal filter is still called out separately |
| `PunctualLightCastVolumetricShadowmapEnableLevel`, `LocalLightCastVolumetricLevel`, `AreaLightCastVolumetricShadowmapEnableLevel` | per-light-type volumetric opt-in, tiered by quality level |

**Grid dimensions and formats for BF6 are not recoverable without a capture.**
The 2015 figures above must not be carried forward.

### 3.3 God rays specifically

**Not separately implemented, as far as the evidence goes.** There is no
light-shaft, god-ray or sun-shaft entry point in the 1,069 recovered names
[BIN]. The 2015 talk frames shafts as an *outcome* of the unified volumetric
system rather than a distinct effect — the pre-2015 state it was replacing was
"basic fog, **screen space light shafts** and particles", and the goal was "the
possibility to have occluded scattering resulting in **volumetric shadows and
light shafts**." [SIG15]

So: **god rays in modern Frostbite are volumetric shadowing in the froxel grid,
not a separate radial-blur pass.** [SIG15] + [BIN] + [inferred] for the
"therefore no separate pass" step. Two caveats: `SkyPhysicalForwardScattering`
and `VolumetricCloudsOccludeLensFlare` exist [BIN], so *some* shaft-adjacent
behaviour has dedicated code; and a radial-blur shaft could be authored as a
post-process graph rather than a named program.

---

## 4. Materials and PBR

**Status: [PBR14] is 2014 and is the only full description EA have published. It
is certainly not current in its entirety — the specialised models below are
absent from it and demonstrably ship. Treat the *core BRDF* as probably intact
and the *material set* as superseded.**

### 4.1 The 2014 BRDF [PBR14]

> "**Specular term fr**: a specular microfacet model with a Smith correlated
> visibility function and a **GGX** NDF.
> **Diffuse term fd**: the **Disney diffuse** term with energy renormalization."
> — [PBR14] §3

Two choices with stated reasoning:

- **Height-correlated Smith.** Citing Heitz's 2014 result that "the Smith
  visibility function is the correct and exact G term" for the microfacet model.
  [PBR14]
- **Renormalised Disney diffuse.** The notes are explicit that the original is
  not energy-conserving — "One important caveat of the Disney diffuse model is
  its **lack of energy conservation**" — and Frostbite adds a renormalisation
  factor, with the plotted result "not perfectly equal to one, [but] close enough."
  [PBR14] §3.1
- **One roughness for both lobes.** "In Frostbite we use the former model and
  thus use **same roughness term for diffuse and specular** terms." [PBR14]

### 4.2 Parameterisation and ranges [PBR14]

The base "Disney" material for deferred shading:

| parameter | note |
|---|---|
| **Smoothness**, not roughness | "We chose to use smoothness instead of roughness"; remapped into perceptually linear smoothness as `1 − αlin` |
| smoothness remap | analysed against alternatives; `(1 − Smoothness)⁴` chosen as "a very close match" to Burley's `(1 − Smoothness)²` while differing from Crytek's Ryse `(1 − 0.7·Smoothness)⁶` |
| **Reflectance** | remapped, capped at **16%** — "goes approximately from 8% for ruby to 17% for diamond. We chose to limit the function to 16%" |
| micro-specular occlusion | encoded in the **lower part** of the reflectance/diffuse attribute range, for non-metals |

### 4.3 Specialised models — present, undocumented

[PBR14] describes a Disney-style base material and mentions "a set of input
parameters: diffuse, smoothness, thickness, etc." It does **not** document cloth,
skin, eye, hair or clear-coat models.

[BIN] shows at least two shipping in Battlefield 6:

- **Subsurface scattering**, tile-classified and tiled-blur based:
  `SubSurfaceScatteringGenerateTilesCS`, `…GenerateClearTilesCS`,
  `…TiledBlurCS`, `…TiledBlur`, `…Blur`, `…LightMerge`, `…TileClear`,
  `…ModifyIndirectArgs`. A screen-space separable blur with tile classification
  and indirect dispatch — SSS is only paid for on tiles that contain skin.
- **A hair BSDF**, pre-integrated offline into lookup textures:
  `StrandHairPreIntegratedBsdfLongitudinalCs`,
  `StrandHairPreIntegratedBsdfAzimuthalCs`,
  `StrandHairPreIntegratedBsdfAverageColorCs`, and — the detail that says someone
  did this properly — `StrandHairPreIntegratedBsdfWhiteFurnaceTestCs` and
  `StrandHairWhiteFurnaceTestCs`. **A white furnace test compiled into the
  shipping binary** is an energy-conservation validation harness. The
  longitudinal/azimuthal split is the standard Marschner-family factorisation.
  [BIN] + [inferred] for the Marschner reading.

  [EA] confirms this was new work for Veilguard: "Strand Hair technology in
  Dragon Age: The Veilguard features a **new hair lighting model with improved
  light transmittance and visibility calculations**." (12 Nov 2024)

**Cloth, eye and clear-coat models: no evidence either way.** No named entry
point, no publication.

### 4.4 Specular occlusion

[PBR14] treats micro-specular occlusion as a packed material channel (§4.2). No
current source describes a screen-space or ray-traced specular occlusion term,
though `RaytraceAoCreation` / `RaytraceAoTrace` / `RaytraceAoResolve` and an
inline variant `RaytraceAoTraceInline` exist in BF6 [BIN], as does
`Raytrace_Occlusion_HG` / `_MS`. Whether AO is applied to the specular lobe is
not recoverable.

---

## 5. Shader authoring — Serac

**Status: current, [SIG24]. Not a rendering technique, but it is the thing that
makes the per-title variance in §0.3 possible, and it is the best-documented
recent Frostbite system after GIBS.**

Frostbite has **180,000 lines of HLSL** [SIG24]. Serac is the language layered
over it, replacing "a system of bespoke C++ code generation" whose problems the
talk describes bluntly:

> "Lots of handwritten C++ code-printing-code … Endless ifs and switches,
> hardcoded enums controlling everything, etc. **This is not glue code either,
> this is part of skinning calculations.**" — [SIG24]

with the consequences: hard to see what shader would be generated, rebuild-
everything iteration times, and "very limited extensibility, owing to the use of
for example hardcoded enums."

The stated problems with raw HLSL at engine scale, which is the reusable part of
the talk [SIG24]:

| problem | why it bites |
|---|---|
| **Global everything**, no private/protected | can't change your own code without risking unrelated callers |
| **Centralised definitions** — packed cbuffers, VS→PS interpolators live in one entry-point signature | "any additions to these must be done in the central location, which is **incompatible with extensibility**" |
| **No link between shader and dispatch code** | resources bound by string or register index |
| **No extensibility mechanisms** | — |
| **Node graphs** | not HLSL's fault, but HLSL "does not have any native mechanisms for interacting with such graphs" |

Two acknowledged inspirations: **Spire** (He et al., SIGGRAPH 2016) for declaring
a whole pipeline and for swappable `interface` blocks; and Bungie's **TFX**
(GDC 2017) for the decisive architectural choice — wrap HLSL rather than replace
it, since "Frostbite has 180,000 lines of HLSL code, and porting to Spire would
require them all to be rewritten." [SIG24]

[BIN] shows Serac names surviving into the shipped binary, e.g.
`Serac:GiPacker_Gibs_Surfel.PackerSelector` — a selector-typed packer, which is
the interface-swapping mechanism visible in the wild.

---

## 6. Textures and streaming

**Status: virtual texturing confirmed present in BF6 [BIN]; no current
publication describes it.**

| evidence [BIN] | reading |
|---|---|
| `VirtualTextureQualityLevel`, `ForceVirtualTextureMaxLevel`, `ResetVirtualTexture` | a live VT system with quality tiers |
| `TextureComputeVtCullingEnable`, `ShaderProgram_VtGenerationCulling2` | tile generation is GPU-culled |
| `TextureVtIndirectionJobEnable`, `TextureVtPreUpdateJobEnable` | CPU jobs for the indirection texture and pre-update |
| `worldToVirtualTextureUv`, `worldPosToVirtualTextureUVs`, `patchUvToVirtualTextureUv`, `terrainVtIndir`, `terrainVtPreUpdate` | **terrain is a primary VT client** |
| `ExtendedVirtualTextureChannelCount_3Channels` / `_4Channels` | selectable channel count |
| `VirtualTextureShaderBuildMode_BuildSurfaceShaders` / `_BuildExpressionShaders` / `_BuildBoth` | two shader flavours for VT page generation |

**Runtime GPU block compression** is compiled in for the whole BC family plus
ASTC 4×4: `TextureCompress_CS_BC1/BC1_HiQ/BC1_sRGB/BC2/BC3/BC4/BC5/BC6S/BC6U/
BC6U_HiQ/BC7` and `TextureCompress_CS_ASTC4x4[_sRGB]`. [BIN] Compressing on the
GPU at runtime is what a VT system does to its freshly composited pages
[inferred] — the ASTC path is presumably for mobile-class targets, consistent
with the ETC2/EAC entries in the platform format table.

**Decals**: only `DecalCopyBufferCs` survives as a named program [BIN], with 287
decal-related strings overall. Not enough to describe the system.

**Detail maps, blend layers, wetness, damage layers**: `DetailDisplacementFor
ShadowsEnable` and `DetailDisplacementForShadowCacheEnable` [BIN] confirm a
detail-displacement system that participates in shadowing. Nothing else
recoverable. **No current EA publication covers material layering.**

Storage, for context: the Battlefield 6 install is **143 GB**, payload in
Oodle-compressed `.cas` content-addressed archives (`oo2core_9_win64.dll`,
`oo2ext_9_win64.dll`) behind `.toc` indices, with **DirectStorage**
(`dstorage.dll`, `dstoragecore.dll`) for streaming. [BIN]

---

## 7. Physics and its rendering consequences

**Status: this is a strength of the [BIN] evidence — the simulation systems are
almost entirely GPU compute and therefore fully named. No current publication
describes any of them.**

### 7.1 Cloth — "Warp"

Battlefield 6 ships a GPU cloth solver under the codename **Warp**, with 45
compute entry points. [BIN] The structure is legible from the names alone:

| stage | entry points |
|---|---|
| integration | `WarpClothIntegration`, `WarpClothClearTransientBuffers` |
| **two-level solve** | `WarpClothConstraintSolveCoarseConstraints`, `WarpClothConstraintSolveFine`, `WarpClothInterpolateCoarseToFine`, `WarpClothRestrictFineToCoarse` |
| collision | `WarpClothConstraintSolveMeshCollision`, `…MeshCollision_v2`, `WarpClothColliderGridUpdate`, `WarpClothMergeMeshColliders`, `WarpClothMeshColliderTriFiltering`, `WarpClothMeshColliderTriangleSorting`, `WarpClothCreateColliderMeshAdjacency` |
| aerodynamics | `WarpClothAerodynamicsGridUpdate` |
| skinning / rigid blending | `WarpClothSkinning`, `WarpClothWrapping`, `WarpClothComputeRigidTransforms`, `WarpClothCreateRigidRegionIdsTexture`, `WarpClothCreateHybridWrappingFactorsTexture` |
| normals | `WarpClothComputeMeshGeometryNormals` |
| partitioning | `WarpClothAnalyzePartitions`, `WarpClothRetargetConstraints` |

**The coarse/fine restriction-and-interpolation pair is a multigrid solver**
[inferred] — solve a coarse cloth, restrict the fine mesh onto it, interpolate
back. That is a substantially more sophisticated structure than the
position-based-dynamics-with-N-iterations that most engines ship, and it is the
standard answer to PBD's poor convergence on stiff, high-resolution cloth.

**Rendering consequence, explicit in the names**:
`WarpClothComputeMeshGeometryNormals` — normals are recomputed on the GPU after
deformation, in the same compute graph, not on the CPU and not skipped.

### 7.2 Strand hair simulation

Simulation and rendering are one system, 89 entry points [BIN]. The simulation
half: `StrandHairIntegration`, `StrandHairConstraintSolve`,
`StrandHairAerodynamicsGridUpdate`, `StrandHairColliderGridUpdate`,
`StrandHairGridUpdate`/`GridCount`/`GridSort`/`GridBucketOffset`/
`GridTransferDensity`/`GridTransferVelocity` — a binned spatial grid with
density and velocity transfer, i.e. hair-hair interaction handled as a
grid-transfer step rather than pairwise [inferred]. Plus
`StrandHairRotateOnTeleport` and `StrandHairSaveInitialSimPoints`, which are the
practical details that stop hair exploding on a camera cut.

**Measured cost** [EA], Veilguard on PS5/XSX, quoting James Power:

> "Running hair simulation costs are also done on the GPU in compute, and change
> dramatically depending on the asset, but tend to hover around **2ms with some
> spikes to nearly 5ms** depending on complexity of the hair and whether we are
> loading/teleporting new assets. **This cost does not scale with resolution.**"

### 7.3 Strand hair rendering — a compute software rasteriser

Confirmed by both evidence classes, which is the strongest position in this
document.

> "Strand Hair is **not rendered like traditional objects** are within Frostbite.
> The technology utilizes a **bespoke compute software rasterizer** and is
> composited into the frame and blended with other opaque and transparent objects
> when resolved." — [EA], 12 Nov 2024

[BIN] shows the rasteriser's internals: `StrandHairRasterizerCoarseCs` →
`StrandHairRasterizerFineCs`, with `…ClusterBoundsClearCs`, `…ClusterQueueCs`,
`…WorkQueueCs`, `…WorkQueueHistogramCs`, `…WorkQueueSortCs`, `…WorkMaskCs`,
`…ReorderCs`, `…ConservativeZCs`, `…StatsCs`. A **coarse/fine binned software
rasteriser with a sorted work queue** — the same architecture as a modern
visibility-buffer rasteriser, applied to hair strands.

**Budgets and memory** [EA], Veilguard, eight hair assets on screen:

| quantity | value |
|---|---|
| strands per character | up to **50,000**, across 100+ hairstyles |
| max hair length | raised from **63 to 255** points |
| flat GPU memory, 8 followers' hair | **~128 MB** |
| additional dynamic memory | **300–600 MB** by quality and resolution |
| typical XSX / PS5 total | **~400 MB** |
| **render budget, 30 fps** | **6.5 ms** (of 33.3 ms) |
| **render budget, 60 fps** | **3 ms** (of 16.6 ms) |
| simulation | ~2 ms, spikes to ~5 ms; resolution-independent |
| low-spec fallback | card hair (XSS, low PC settings) |

**Hair is not upscaled, and this is a design problem EA call out:**

> "Strand Hair is normally rendered at render resolution and is **unaffected by
> upsampling technology such as NVIDIA DLSS, AMD FSR, or Intel XeSS**. Therefore
> it does not scale as well with other render features when those settings are
> applied." — [EA]

Their fix is an independent hair resolution controller clamped between a min and
max derived from the upscaler and DRS settings. [EA] **Worth internalising: any
system that composites outside the upscaled path becomes the fixed cost that
stops upscaling from buying you anything.**

**Blending hair with transparents** was Veilguard's specific contribution, and
the algorithm is given in full. The software rasteriser "was specifically
designed to favor blending with depth of field, which is an important broadcast
camera technique used in sports games" — fine for FC and Madden, wrong for a
game full of spell effects and volumetric fog. The replacement, quoted in
condensed form from [EA]:

1. Split hair into **opaque** (alpha ≥ cutoff) and **transparent** (below cutoff)
   passes.
2. Render the **depth of the transparent part** first — "mostly this is just the
   ends of the hair strands" — as "a spatial barrier between transparent pixels
   that are 'under' and 'on top' of the strand hair."
3. Render opaque hair, then transparent objects: each tests against that depth,
   and if **under** the hair it renders and **marks a stencil bit**; if on top or
   equal it discards.
4. Render transparent hair (now guaranteed nothing beneath it is unrendered).
5. Render transparent objects **again**, testing the stencil mask for where they
   were skipped, "thus layering the pixels of transparent objects that are on top
   of the hair properly. This results in **perfect pixel blending** with
   transparent objects."

### 7.4 Skinning, blend shapes and deformation

All GPU compute [BIN]: `MeshComputeSkinningCs`, `MeshComputeBlendShapesCs`,
`MeshComputeVertexNormalsCs`, `MeshComputeFaceNormalsCs`,
`MeshComputeTensionCs`, `MeshComputeProceduralChannelsCs`,
`MeshComputePSDApplyCs` / `MeshComputePSDWrapCs` (pose-space deformation),
`MeshComputeAttachmentJointsCs`, `MeshComputeRelativeTriangleAreasCs`, and
cloth-to-mesh bridges `MeshComputeClothBlending`,
`MeshComputeClothComputePointTransforms`,
`MeshComputeClothComputeRigidRegionTransforms`, `MeshComputeClothWrapping`.

**Normal recalculation after deformation is a first-class GPU pass**, in both
face and vertex flavours, and a *tension* pass exists for driving wrinkle maps
from deformation [inferred].

### 7.5 Destruction

**Nothing current.** The only Frostbite destruction publication we found is
**[SIG10]** — *Destruction Masking in Frostbite 2 using Volume Distance Fields*,
Per Einarsson and Robert Kihl (DICE), SIGGRAPH 2010 Advances. Sixteen years old,
Frostbite 2, and it describes masking rather than the simulation. Nothing in
[BIN] names a destruction system. Given Battlefield 6's marketing emphasis on
destruction this is a conspicuous gap in the public record — see §10.

### 7.6 Ragdolls and acceleration-structure updates

No ragdoll-specific evidence either way. On acceleration structures, [SIG24] is
clear that TLAS rebuild is per-frame and overlapped: "We always overlap our BLAS
and TLAS building in async with our gbuffer laydown", with BLAS precomputed
offline for **static** geometry only, worth 15% traversal time. Dynamic and
skinned geometry therefore rebuilds. [SIG24]

---

## 8. Everything else load-bearing

### 8.1 Sky and atmosphere

**Status: a precomputed-scattering physical sky is present in BF6 [BIN]. The
public description is [SIG16-PBR] — Hillaire's *Physically Based Sky, Atmosphere
and Cloud Rendering in Frostbite*, SIGGRAPH 2016 PBR course — which we did not
retrieve in full.**

[BIN] names: `SkyPhysical`, `SkyPhysicalPrecomputeScatteringCS`,
`SkyPhysicalPrecomputeAPCS` (aerial perspective), `SkyPhysicalSkyLighting`,
`SkyPhysicalForwardScattering`, `SkyPhysicalGetHorizonColor`,
`SkyPhysicalSunDisk`, `SkyPhysicalDebugVolumetric`, `SkyVelocity`.

Volumetric clouds are a separate, well-developed system with an unusually
complete set of exposed knobs [BIN]: `VolumetricCloudsSampleCount`,
`…ShadowIterationCount`, `…IBLSampleCount`, `…ReflectionSampleCount`,
`…RenderTargetResolutionDivider`, `…ReflectionRenderTargetResolutionDivider`,
`…ShadowmapResolution`, `…ShadowmapBlurSamples`, `…TemporalCoefficient`,
`…ShadowTemporalCoefficient`, `…EnvColorTemporalCoefficient`,
`…CastShadowInForwardRender`, `…AffectAerialPerspective`,
`…ReceiveAerialPerspective`, `…OccludeLensFlare`, `…Quality`.

**Clouds both cast into and receive aerial perspective**, are shadowed into the
world, contribute to IBL, and are rendered at a divided resolution with a
temporal coefficient — i.e. the standard quarter-res-plus-reprojection cloud
pipeline. [BIN] + [inferred]

> **Do not attribute the 2020 sky/atmosphere paper to Frostbite.** *A Scalable
> and Production Ready Sky and Atmosphere Rendering Technique* (Hillaire, CGF
> 2020) is by the same author but post-dates the move to Epic; it is an Unreal
> lineage technique. The Frostbite sky work is the 2016 course.

### 8.2 Reflections and SSR

**Status: hybrid screen-space + ray-traced, current [BIN]. The screen-space half
descends from [SIG15].**

The 2015 contribution was *Stochastic Screen-Space Reflections* (Stachowiak) —
importance-sampled SSR with a temporal filter. In BF6 the system is much larger
and clearly hybrid [BIN]:

| entry point | reading |
|---|---|
| `ReflectionRayMarch_CS` | the screen-space march |
| `ReflectionRayGenOpaque_RG`, `ReflectionRayGenAlphaTest_RG`, `ReflectionRayGenTransparent_RG` | **DXR raygen** — three ray types |
| `ReflectionRaySorting_CS`, `ReflectionRayDistribution_CS`, `ReflectionRayAlloc_CS` | ray sorting and allocation |
| `ReflectionRateClassification_CS` | **variable-rate** reflections |
| `ReflectionRayLighting_CS`, `ReflectionRayLightingMiss_CS`, `ReflectionRayFog_CS` | shading the hit, the miss, and fog along the ray |
| `ReflectionTransparent_HG`/`_MS`, `ReflectionTransparentBlend_CS`, `ReflectionTransparentFallback_HG` | **transparents receive ray-traced reflections**, with a fallback |
| `ReflectionDefrag_CS` | the reflection resource is an allocator that needs defragmenting |
| `RtReflectionsRayAllocMode_Default` / `_HalfRes` / `_VariableRate` | three allocation modes |
| `ScreenSpaceRaytrace*` (~20 settings) | the SSR path, with async-compute preference, full-res toggle, ocean buffer, camera-cut handling |

A shared denoiser serves these: `DenoiserReprojection_CS`,
`DenoiserAtrousFilter_CS`, `DenoiserHistoryClamping_CS`,
`DenoiserFireflySuppression_CS`, `DenoiserRayReuse_CS`, `DenoiserSkip_CS`. [BIN]

### 8.3 Ray tracing and path tracing

Beyond GI (§1.5), reflections (§8.2) and contact shadows (§2.1), [BIN] shows
`RaytraceAo*` (with `Inline` and `RtPSO` variants — both DXR 1.1 inline and 1.0
pipeline-state paths compiled in) and, notably, a **path tracer**:
`PathTracer_PrimaryRayCS`, `PathTracer_TraceEdgeRG`, `PathTracer_ProccessEdgeRG`
[sic, the typo is in the binary], `PathTracer_Miss`, `PathTracer_ImageOutputCS`,
plus a `PtUpdateVolumetric` setting.

**A path tracer being present in bf6.exe does not mean Battlefield 6 offers path
tracing.** Frostbite very plausibly ships a reference path tracer for validating
the real-time paths — GIBS's `DeferredLightTile_LightingCS_Reference` shows the
same instinct. [inferred] Do not report this as a shipped feature.

### 8.4 TAA and upscaling

[BIN]: `TemporalAAResolve` is the in-house resolve, with debug settings
`DrawDebugTemporalAAEnable`, `…DebugMode`, `…AccumulationCount`, `…MaxDistance`,
and `AllowTemporalAAWhenDeterministicRenderingEnabled`. Alongside it,
`ResolveCheckerboard` (checkerboard rendering), `MsaaResolve` and
`ResolveEqaaDepth` (EQAA — an AMD/console MSAA variant) are all compiled in.

Upscalers, from the install root and the binary: **DLSS** (`nvngx_dlss.dll`,
`nvngx_dlssd.dll` ray reconstruction, `nvngx_dlssg.dll` frame generation, via
Streamline `sl.*.dll`), **XeSS** (`libxess.dll`, `libxess_fg.dll`, `libxell.dll`),
**FSR** (`amd_fidelityfx_upscaler_dx12.dll`,
`amd_fidelityfx_framegeneration_dx12.dll`) — plus FSR3 integrated at the shader
level: `Fsr3Upsampler_PrepareInputs`, `…PrepareReactivity`, `…LumaChange`,
`…Accumulation`. [BIN]

### 8.5 Auto-exposure

Fully named [BIN]. Two methods, selectable:
`AutoExposureMethod_HistogramAverage` and `AutoExposureMethod_MipAverage`, with
`_None` and `_UseGlobalSetting`. The histogram path is parameterised by
`AutoExposureHistogramBinCount`, `…MinValue`, `…MaxValue`, `…MipUsed`. Averaging
runs through **AMD's Single Pass Downsampler** — `AverageLuminanceSPDPass`,
`AverageLuminanceSPDPassRc2`. Camera-side, `CameraExposureMode_Manual`,
`_ManualEV`, `_AutoExposure`, `_UseVisualEnvironment` [BIN] — a manual EV mode
consistent with the [PBR14] photometric pipeline surviving.

### 8.6 Colour pipeline and output transforms

Grading is a **3D LUT** built and interpolated on the GPU: `ColorCubeInterpolate`,
`ColorCubeInterpolateCS`, `ColorCubeGenerateForPost`,
`ColorCubeGenerateForResolve` [BIN] — two generation variants, i.e. grading is
applied at two different points in the frame.

Output transforms are unusually well covered [BIN]: `DisplayMappingHdr10PeakLuma`,
`…PeakLumaFullFrame`, `…MinimumBlackLuma`, and a full **Dolby Vision** metadata
block (`DolbyVisionMetadataL1MaxLuminanceOverride`, `…L1Min…`, `…L2Max…`,
`…L2Avg…`, each with an `…Enable`). `FilmicEffectsStage1` / `Stage2` carry the
tonemap-adjacent work.

### 8.7 Motion blur and depth of field

Motion blur: `MotionBlur`, `MotionBlurExtraction`, `MotionBlurPack`,
`MotionBlurExtractionDebug` — a tile-extraction/packing structure [BIN].

Depth of field: a work-queue compute DOF (`DofWorkQueueCs`,
`DofDispatchArgsCs`), a circular-bokeh variant (15 `Circular*` programs,
`circDofCocVtBlur`, `circDofFar0VtBlur`, `circDofFar1VtBlur`, `circDofVtNear`),
and a **dedicated iron-sights DOF** for weapon aiming: `FinalIronsightsDof`,
`…DofCoc`, `…DofDilate`, `…DofWithCircle`. [BIN] The last is a nicely
Battlefield-specific piece of evidence — a whole DOF variant for looking down a
scope.

### 8.8 Other systems visible in the binary

Worth recording because they show the breadth, all [BIN]:

- **GPU occlusion culling by software rasterisation** —
  `OcclusionRasterizeDrawTrianglesPerPixelCS` / `…PerTriangleCS`,
  `OcclusionRasterizeProcessTrianglesCS`, `…MaxFilterCS`, `…NoFilterCS`,
  `MeshCullOcclusionDownscaleCS`, `MeshCullFullDepthPassPerBoxCS` /
  `…PerPixelCS`, `MeshCullCS`, `MeshCullShadowsCS`,
  `MeshCullGenerateShadowProjectionsCS`.
- **Terrain** — GPU quad tessellation (`TerrainAllocateQuadTessellation`,
  `TerrainPrepareQuadTessellation`, `TerrainQuadIndexTessellation`,
  `TerrainQuadVertexTessellation`, `TerrainTessellation`),
  `TerrainEncodeSurface`, `TerrainDownsampleDisplacement`, VT-backed (§6).
- **Ocean and water** — `OceanHeightExtractionCS`, `OceanHeightScanFillCS`,
  `OceanHeightBoundaryCS`, `OceanTile_Classify`/`_Cull`/`_Copy`,
  `Ocean_UnderwaterFog`, `WaterEWave`, `WaterFoam`,
  `WaterApplyInteractiveDisturbs`, `WaterExclusionTileCullCs`.
- **`SoldierVisibility`** — ten entry points doing luminance/depth extraction,
  background blur, edge init, flood fill and combine. [inferred] A readability
  aid that makes enemy soldiers separate from the background — a gameplay-driven
  render feature, not a fidelity one.
- **`Rime`** — 24 entry points for antialiased path filling, YCrCb texture
  sampling, tiled Gaussian blur and tiled SDR conversion. A **vector-graphics UI
  renderer** [inferred], sitting beside a separate `Twinkle` UI system (68
  programs) and generic `Ui_*` programs (51).

---

## 9. The classic papers — status verdicts

The brief asked for this explicitly. Verdicts are ours [inferred] from the
evidence assembled above.

| paper | year | verdict |
|---|---|---|
| **Moving Frostbite to PBR** [PBR14] | 2014 | **Partly current.** The photometric unit system and the GGX + correlated-Smith + renormalised-Disney BRDF have no published successor and are consistent with what survives in [BIN] (`CameraExposureMode_ManualEV`, area lights). The *material set* is superseded — SSS and a new hair BSDF ship (§4.3) and are absent from the notes. Reliable for the core BRDF; do not treat as a complete description of a 2025 material system. |
| **Towards Unified and Physically-Based Volumetric Lighting** [SIG15] | 2015 | **Architecture current, numbers obsolete.** Cascaded clip-space froxel volumes, particle voxelisation, per-froxel scattering, temporal integration and volumetric shadow maps all survive by name into BF6 [BIN]. The grid dimensions, tile sizes and PS4 costs are ten-year-old figures for a specific platform and must not be quoted as current. |
| **Stochastic Screen-Space Reflections** [SIG15] | 2015 | **Superseded in scope.** SSR survives (`ScreenSpaceRaytrace*`, `ReflectionRayMarch_CS`) but is now one half of a hybrid system whose other half is DXR with ray sorting, variable-rate classification and transparent reflections (§8.2). Lead with the hybrid. |
| **Physically Based Sky, Atmosphere and Cloud Rendering** [SIG16-PBR] | 2016 | **Probably current in outline, unverified.** `SkyPhysicalPrecomputeScatteringCS` etc. [BIN] indicate a precomputed-scattering sky of that family. We did not retrieve the 2016 notes, so we cannot say what changed. |
| **Global Illumination Based on Surfels** [SIG21] | 2021 | **Superseded by [SIG24], by the authors' own framing** — "In these past 3 years we did not introduce any new systems … Instead, we've done incremental improvements". Read 2021 for the system description, 2024 for every number. |
| **Destruction Masking in Frostbite 2** [SIG10] | 2010 | **Obsolete as a description of anything shipping.** Frostbite 2, sixteen years old. |
| **Precomputed GI in Frostbite** (O'Donnell, GDC 2018) | 2018 | **Still one of four options** — this is the Flux/Enlighten/GiGrid lineage [SIG24] explicitly retains for titles that want precomputed quality. Not superseded, but no longer the flagship. |

---

## 10. What could not be found

Confirmed absences, with the searches that support them. Each was looked for in:
the SIGGRAPH Advances course index for **every year 2010–2026**; EA's SEED
publication and news indexes; EA technology blog posts; targeted web search; and
the 1,069 recovered shader names plus ~125k strings from bf6.exe.

| topic | status |
|---|---|
| **The geometric light-culling rasteriser (§1.1)** | **Never published.** No EA talk describes `LightCull_Pass1..4`, the bitset rasterisation, the GJK path or the Z-bin. This is the largest unpublished system found, and arguably the most useful. |
| **Shadow filtering — PCF/PCSS/VSM/EVSM** | **Not published and not recoverable.** No filter name in the binary; no talk since Frostbite 2. We can say *what shadow systems exist* (§2.1) and not *how a texel is filtered*. |
| **Cascade count, resolutions, split scheme** | **Knobs found, values not.** `CascadeCount`, `ShadowmapResolution`, `LocalLightShadowAtlasSlotResolution` exist as settings names; the shipped values live in data, not the executable. A capture would answer this immediately. |
| **Current froxel grid dimensions and formats** | **Not recoverable without a capture.** Only the 2015 PS4 figures exist publicly. |
| **Practical light counts for any recent title** | **Never published.** |
| **Destruction** | **Nothing since 2010.** No current publication, no named system in the binary. Conspicuous given the franchise. |
| **Battlefield 2042 (2021) and Battlefield 6 (2025) rendering talks** | **Appear not to exist.** Neither title has a SIGGRAPH Advances or public GDC rendering talk. BF2042 appears in [SIG24] only as an illustrative screenshot; BF6's only GDC 2026 presence is *Performance Capture in the Trenches*, a cinematics/mocap talk. |
| **Dead Space (2023)** | **No engineering source found.** Appears in [SIG24] as one screenshot illustrating engine variety. Everything else located was [3P] performance analysis and graphics-menu observation, which cannot support implementation claims. |
| **EA at SIGGRAPH 2025** | **Confirmed absent.** The 2025 Advances course had eight talks — Activision ×3, Ubisoft, MachineGames, id Software, HypeHype, NVIDIA, Epic — and **no EA, DICE, Frostbite or SEED content.** |
| **Rendering talks at GDC 2026** | **Confirmed absent for EA.** EA's GDC 2026 sessions are reinforcement learning for FC 26 goalkeepers, BF6 performance capture, and Apex developer support. No rendering session. |
| **Cloth, eye and clear-coat shading models** | **No evidence either way.** |
| **Decals, material layering, wetness and damage layers** | **Not published; only fragments in the binary (§6).** |
| **Virtual texturing details** | **Present but undescribed** — no page size, atlas size, feedback mechanism or compression policy available. |

> **One nomenclature trap.** Battlefield 6's superbundle directories are named
> `glacierflow` and `glaciermp`, and the executable is full of the codename
> `santiago`. **"Glacier" here is an EA bundle/codename and has nothing to do
> with IO Interactive's Glacier engine**, which coincidentally has a 2026
> SIGGRAPH talk of its own (*Smolder*, 007 First Light). Do not let a search
> conflate them.

---

## 11. What is worth taking for this project

Filtered for **GL 4.3 + compute, no ray tracing, desktop, and modest agent
counts** — the constraints in CLAUDE.md. Ranked by value-to-effort.

1. **Z-binning on top of the existing tile cull.** [BIN] The cheapest step from
   2D tiled to effectively-clustered lighting: one extra pass populating depth
   bins, no 3D grid to allocate. `LightCullZBinBits` suggests it is a handful of
   bits per light.

2. **Cull-refine using the shadow map.** [BIN]
   `DeferredLightTile_CullRefineUsingShadowCS` — if a light's shadow map proves
   it cannot reach a tile, drop it from that tile's list. This is the CLAUDE.md
   "cull cheaply before testing expensively" rule applied one level further out
   than usual, and it needs no new data structure.

3. **The froxel volumetrics recipe, whole.** [SIG15] Frustum-aligned 3D texture
   aligned to the light tile grid so the tile cull is reused; store **extinction,
   not transmittance**, because extinction is linear and therefore temporally
   integrable; jitter material and scattering samples **with the same offset**;
   Halton sequence; single-lobe HG phase function; wavelength-independent
   extinction to keep the volume cheap. Every one of those is a decision with a
   stated reason, and the whole thing is compute-shader-sized.

4. **Depth beats irradiance in a fixed probe byte budget.** [SIG24] The 14×14
   depth / 6×6 irradiance split. If we ever build probes, this is the allocation
   to start from rather than discover.

5. **Amortised probe updates with a priority function.** [SIG24] One clipmap
   level per frame; probes that moved get priority, probes seeing no nearby
   geometry get less; update the top N. Then compensate the latency with a
   hysteresis driven by a running statistic. Directly reusable for any cached
   lighting.

6. **Sort-then-test-from-the-best.** [SIG24]'s reservoir sampling of N light
   candidates down to one shadow test is the same rule CLAUDE.md already states
   for target selection. Frostbite pays one shadow test per secondary ray, not N.

7. **Tile classification before expensive per-pixel work.** [BIN] SSS, surfel
   apply and contact shadows all classify tiles first and dispatch indirectly, so
   the expensive shader only runs where it matters. `SubSurfaceScatteringGenerate
   TilesCS`, `SurfelApplyTileClassifyCs`, `ContactShadowsLightTileCompaction_CS`.
   This is a pattern, applied consistently, and it is cheap in GL 4.3 with
   indirect dispatch.

8. **The white furnace test as a shipped artefact.** [BIN] Frostbite compiles an
   energy-conservation validation shader into the retail binary. That is the
   same instinct as CLAUDE.md's "test derived data against its source" — a
   BRDF's energy conservation is checkable, so check it, and keep the checker.

9. **Coarse/fine multigrid for constraint solving.** [BIN] If cloth or soft-body
   ever appears here, Warp's restrict/interpolate structure beats iterating PBD
   on the fine mesh.

10. **What *not* to copy:** GIBS itself. It requires hardware ray tracing, a
    TLAS, 100 MB of probes and 500 MB total on the smallest target, and buys
    dynamic diffuse GI we do not currently need. The *lessons* above transfer;
    the system does not.

---

## 12. Provenance

- Documents retrieved and text-extracted on **2026-08-13**: the [SIG24] GIBS PDF
  (67 pages, slides + full speaker notes), the [SIG24] Serac deck (56 slides),
  the [SIG26] ORCA deck (59 slides), the [SIG15] volumetrics deck (57 slides),
  the [SIG15] SSR deck, and the [PBR14] course notes v3.2 (122 pages).
- SIGGRAPH Advances course indexes swept for EA content, **2010–2026 inclusive**.
- [BIN] evidence read from `bf6.exe`, 195,618,664 bytes, file-stamped
  **2025-08-11**, from the retail Steam install. 125,465 unique strings;
  **1,069 `ShaderProgram_*` names**, listed in full in
  [`frostbite_bf6_shader_programs.txt`](frostbite_bf6_shader_programs.txt).
- **No frame capture was taken** and no process was injected into (§0.1).
