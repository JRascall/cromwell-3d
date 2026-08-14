# Unreal Engine 5's high-fidelity path — the whole feature inventory, read against cromwell

**Engine version: 5.8**, released June 2026, which is what `dev.epicgames.com`
serves as current documentation. 5.7 is November 2025 and 5.6 is June 2025;
where a feature changed maturity between those, the version is named. Read and
written **14 August 2026**.

Source tags: **[EPIC]** = Epic's own documentation or release notes, quoted or
paraphrased from the pages listed in §14. **[SRC]** = read out of this repo's
own source. **[inferred]** = my reasoning, not anybody's claim.

## 0. What "high fidelity" actually names

There is no single switch called *high fidelity*. What the phrase refers to in
practice is **the set of features a newly-created UE5 project turns on by
default**, plus the scalability level those defaults target. That set is:

| On by default in a new project | Since |
|---|---|
| Lumen dynamic GI **and** Lumen reflections | 5.0 [EPIC] |
| Mesh Distance Fields (Lumen's software-tracing dependency) | 5.0 [EPIC] |
| Virtual Shadow Maps | 5.0 [EPIC] |
| Nanite (on new static meshes, per-asset) | 5.0 [EPIC] |
| Temporal Super Resolution as the anti-aliasing method | 5.0 [EPIC] |
| Substrate materials | **5.7** [EPIC] |
| MegaLights | opt-in; **production-ready in 5.8** [EPIC] |

Two things follow from that list that are easy to miss:

1. **Turning Lumen on turns other things off.** "Lumen Global Illumination
   replaces Distance Field Ambient Occlusion (DFAO). Lumen Reflections replace
   Screen Space Reflections. When Lumen is enabled for a project, precomputed
   static lighting contributions are disabled and all lightmaps are hidden."
   [EPIC] The high-fidelity path is not additive — it is a *different* lighting
   pipeline that displaces the baked one.
2. **A project upgraded from UE4, or from pre-5.7, keeps the old paths.**
   Substrate and Lumen both stay off on upgrade specifically so lighting does
   not change under an existing project [EPIC]. So "what does UE5 give you" and
   "what does a given UE5 game use" are different questions.

The quality *ladder* is separate from the feature list, and it is where the
budgets live. Lumen targets **8 ms at 30 fps and 4 ms at 60 fps, at 1080p**, for
GI and reflections on opaque *and* translucent materials plus volumetric fog;
each scalability level "costs approximately half of the level above it";
`Epic` = 30 fps console, `High` = 60 fps console, `Medium` = Switch 2 and
mid-spec PC, `Low` **disables Lumen entirely** [EPIC]. And the 4K story is
explicitly an upscaling story: "Lumen relies on Temporal Upsampling with
Unreal Engine 5's Temporal Super Resolution (TSR) for 4k output... This gives
better final image quality than running Lumen natively at 4k resolution with
significantly lower quality settings." [EPIC]

**That last sentence is the single most important structural fact in this
document**, and §12 comes back to it: the high-fidelity path is designed around
rendering at roughly a quarter of the output pixels and reconstructing the rest
temporally. Every cost quoted below is a 1080p cost. A renderer that does not
have a temporal upscaler is not comparing like with like.

## 1. Geometry — Nanite

**What it is.** "Nanite is Unreal Engine's virtualized geometry system which
uses an internal mesh format and rendering technology to render pixel scale
detail and high object counts. It intelligently does work on only the detail
that is visible on-screen and no more. Nanite's data format is also highly
compressed and supports fine-grained streaming with automatic level of detail."
[EPIC]

What it buys, in Epic's own framing: frame budgets "no longer constrained by
polycounts, draw calls, and mesh memory usage"; direct import of ZBrush sculpts
and photogrammetry; high-poly detail *instead of* baking to normal maps; LOD
chains disappear as an authoring task [EPIC].

What it does not buy — Epic's own caveat, worth quoting because it is the part
marketing drops: "practical limits still remain. For example, instance counts,
triangles per mesh, material complexity, output resolution, and performance
should be carefully measured for any combination of content and hardware."
[EPIC]

**The 5.7 extension — Nanite Foliage** (experimental), which is three systems
rather than one [EPIC]:

| System | What it does |
|---|---|
| **Nanite Assemblies** | the repeated "parts" of a plant, instanced across it, so a tree stores one leaf and a transform list rather than a million unique triangles |
| **Nanite Voxels** | "near pixel-sized aggregate voxels that preserve triangle details, animation, and material properties" at distance — the LOD tail, in a representation triangles cannot reach |
| **Nanite Skinning** | wind and motion as skinning rather than World Position Offset |

The doc is unusually explicit about what it is *rejecting*, and each rejection
is a general lesson [EPIC]:

- **Alpha-masked cards** — "introduces a lot of overdraw in addition to having
  to execute a potentially expensive mask function", and keeps foliage flat.
- **Full triangle representation** — "sub-optimal cluster culling and poor
  simplification in the distance", plus disk size.
- **WPO for wind** — "adds per vertex calculations and forces sub-optimal
  cluster bounds since there's no way to know how a material will move a vertex,
  meaning the calculations have to be more conservative."

That third one is the transferable one: **a material that moves vertices makes
every bound conservative, and conservative bounds defeat the culling that the
whole system is built on** [EPIC + inferred]. Any engine that adds vertex
animation to a culled representation inherits this problem, at whatever scale it
culls at.

**What Nanite needs from the hardware**, and this is the part that decides
whether it is even available to cromwell: Nanite is a **compute-based software
rasteriser writing a visibility buffer via 64-bit atomics**, with a hardware
raster fallback for large triangles. GL 4.3 has neither 64-bit image atomics nor
the shader model to make the cluster culling pipeline affordable [inferred —
Epic document the requirement as SM5+/DX12/Vulkan-class, not as a GL feature
level]. §13 treats this as a hard blocker rather than a cost.

## 2. Lighting — Lumen, MegaLights, and what they displaced

### 2.1 Lumen

"Lumen is Unreal Engine's fully dynamic global illumination and reflections
system... Lumen renders diffuse interreflection with infinite bounces and
indirect specular reflections in large, detailed environments at scales ranging
from millimeters to kilometers." [EPIC]

Two tracing back-ends: **software ray tracing** against mesh distance fields
(hence the forced `Generate Mesh Distance Fields` dependency) and **hardware ray
tracing** against the real BVH, which is higher quality and more expensive
[EPIC].

**Lumen Lite (5.8, beta)** is the interesting recent move: "a new medium-quality
setting for Global Illumination using Irradiance Fields with Probe Occlusion...
twice as fast as Lumen high quality — which targets 60fps on PlayStation 5,
while maintaining the art direction for games that rely on Global Illumination.
This path is the new default for current-generation handheld consoles." [EPIC]
Mechanically it is `r.Lumen.FinalGatherMethod 0` — an **Irradiance Field final
gather**, world-space radiance cache probes placed around pixels and
pre-integrated, in place of the screen-probe gather [EPIC].

That is the same shape as Frostbite's GIBS probe grid
([`frostbite_rendering.md`](frostbite_rendering.md)) and RE ENGINE's tetrahedral
probe network ([`re_engine_rendering.md`](re_engine_rendering.md)): **when the
budget halves, everyone lands on probes with an occlusion term.** Three engines,
independently, and it is the rung cromwell's reflection-probe work is already
standing on [inferred].

### 2.2 MegaLights

"MegaLights is a whole new direct lighting path in Unreal Engine 5 enabling
artists to place orders of magnitudes more dynamic and shadowed area lights than
they could ever before... it also reduces the cost of unshadowed light
evaluation, making it possible to use expensive light sources, such as textured
area lights, on consoles." [EPIC]

Mechanism: "a stochastic direct lighting technique, which solves direct lighting
through importance sampling of lights. It traces a fixed number of [rays per
pixel]" — so **the cost is bounded by the ray budget, not by the light count**,
and the light count becomes a sampling-quality question instead of a loop-length
question [EPIC]. Shadowing is either ray tracing (default) or VSMs, and Epic are
blunt about the VSM trade: VSMs shadow from real Nanite geometry rather than a
ray-tracing proxy, but carry "CPU, memory and GPU time overhead... on a per-light
basis for preparing shadow map depths in advance" [EPIC].

Maturity: beta in 5.7 (adding directional lights, Niagara particle lights,
translucency, hair strands), **production-ready in 5.8** with, per the release
notes, a specific optimisation worth naming — "No longer traces multiple rays
towards the same light if it's not in a penumbra. This greatly improves
performance (0.3-1ms on console, depending on the scene)." [EPIC]

**The idea to steal is not the technique, it is the inversion** [inferred]:
clustered forward makes cost proportional to lights-per-cluster; stochastic
sampling makes cost proportional to *rays per pixel* and pushes light count into
noise, which a denoiser then eats. The first is the right answer at cromwell's
scale; the second is what the ceiling looks like.

### 2.3 What is still there underneath

The baked and semi-baked paths did not go away, and for a project that does not
want a 4 ms GI bill they are the alternative [EPIC]: **GPU Lightmass** (path-traced
baked lightmaps + volumetric lightmaps for dynamic objects), **Distance Field
Ambient Occlusion** and distance-field soft shadows, the **Sky Light** with
real-time or captured cubemap modes, and **Sky Atmosphere** as an analytic
participating-medium sky.

## 3. Shadows — Virtual Shadow Maps

Goals, verbatim, because they are a good statement of the problem: "Significantly
increase shadow resolution to match highly detailed Nanite geometry; plausible
soft shadows with reasonable, controllable performance costs; provide a simple
solution that works by default with limited amounts of adjustment needed;
replace the many Stationary Light shadowing techniques with a single, unified
path." [EPIC]

The implementation, with the numbers [EPIC]:

- **Virtual resolution 16384 × 16384** per light, with **clipmaps** raising it
  further for directional lights.
- Split into **128 × 128 pages**, "allocated and rendered only as needed to
  shade on-screen pixels **based on an analysis of the depth buffer**".
- Pages are **cached between frames** unless invalidated by a moving object or a
  moving light.

Two consequences Epic state directly: Nanite does not support the old stationary
-light shadow techniques (pre-shadows, per-object inset shadows), so a Nanite
project must use movable lights with VSMs [EPIC]; and **the cost model becomes
"how much of the shadow map got invalidated this frame", not "how many casters
are there"** [inferred] — which is exactly the cache-and-repair economy RE ENGINE
built its shadow system around.

**5.8 adds "Prefiltered Distant" (experimental)**: "drawing distant shadow
casters into the clipmap at much lower resolution, while using prefiltering and
temporal reprojection to produce a smooth result that often matched ground truth
better than SMRT" [EPIC]. Note the claim — *lower resolution matched ground truth
better* — which is the same observation cromwell's shadow header already makes
from the other direction: a one-texel staircase cannot be filtered away, so the
fix is either smaller texels or a penumbra wide enough to swallow the wobble
[SRC: `lighting/ShadowMap.hpp`].

## 4. Materials — Substrate

"Substrate is Unreal Engine's approach to authoring materials, which replaces
the fixed suite of shading models and blend modes, such as Default Lit and Clear
Coat, with a more expressive and modular framework. Certain abstractions of the
non-Substrate (or legacy) material system are done away with by Substrate — it
replaces them with **measured properties of matter**." [EPIC]

The model: materials are **"slabs of matter"**, "a principled BSDF
representation, parameterized by physical quantities with well-defined units",
and a material is "a graph of slabs on which operations are performed (like
mixing and layering)" [EPIC].

The line that matters for an engine designer is the next one: "**Because of
their principled representation, Substrate Materials can be simplified according
to the capacity of a platform in order to trade visual quality for
performance.**" [EPIC] That is the entire argument for a parameterised material
model over a fixed enum of shading models — **a principled parameterisation can
be automatically degraded; a hand-written shading model cannot** [EPIC +
inferred].

Two G-buffer formats, and the default is the *cheap* one: "For new projects that
have Substrate enabled by default, **Blendable GBuffer** format is used to
prioritize speed over visual fidelity... Some template projects, such as
Automotive and Architectural, use the **Adaptive GBuffer** by default in order to
favor visual fidelity over performance." [EPIC] Even inside Epic's
highest-fidelity framework, the games default to the fast path.

Migration is handled by compiling rather than converting: legacy materials "will
work out-of-the-box but aren't automatically converted to Substrate nodes...
[they] are converted to Substrate at compile time. This simplifies the project
migration, avoids the need for asset modification or resaving, and helps with
cooking costs." [EPIC]

Adjacent material machinery: **material layers and layer blends**, **MaterialX**
import through the Interchange framework (an ILM/ASWF interchange format for
material graphs) [EPIC], and **custom primitive data** for per-instance material
parameters without a material instance each.

## 5. Textures — the two virtual texturing systems

| | **Runtime Virtual Texturing (RVT)** | **Streaming Virtual Texturing (SVT)** |
|---|---|---|
| Where texels come from | generated by the GPU at runtime | cooked, streamed from disk |
| Good for | procedural or composited layered materials, landscape blending, splines, decals | large artist-authored textures, UDIM, virtual-texture lightmaps |
| Shared property | larger textures than memory allows; texel data cached on demand | same |

[EPIC] Both exist to make memory a function of *what is on screen* rather than
of what is in the level — the same trade Broken Arrow buys wholesale from
Granite (27 GB of streaming virtual texture, half its install:
[`broken_arrow.md`](../flight/broken_arrow/broken_arrow.md)).

## 6. Volumes and atmosphere

- **Sky Atmosphere** — analytic Rayleigh/Mie sky, physically parameterised.
- **Exponential Height Fog + Volumetric Fog** — "computes participating media
  density and lighting at every point in the camera frustum to support varying
  densities and **any number of lights** that affect the fog" [EPIC], i.e. the
  froxel volume [`rdr2_atmospherics.md`](rdr2_atmospherics.md) describes.
  Controlled globally by the height fog component and locally by particle
  systems.
- **FSSS — Fog Screen Space Scattering (5.8, experimental)** — "approximates
  multiple light scattering within participating media — making dense fog, smoke,
  and dust appear blurrier and more integrated with the scene" [EPIC].
- **Volumetric Clouds** — raymarched layer with its own material domain.
- **Heterogeneous Volumes + Sparse Volume Textures** — VDB import (static or
  animated sequences), volume-domain materials, rendered in real time *and* by
  the path tracer. Epic's own import recommendation is worth keeping: split the
  attributes so "density [is] an 8-bit unorm and pass all other data through the
  16-bit float attribute" [EPIC].

## 7. Characters, hair, water

**Groom / hair strands** — real strand geometry with simulation, and MegaLights
gained hair-strand support in 5.7 [EPIC]. Note that BF6 reaches the same place by
a different road — a compute software rasteriser for strands, at 6.5 ms/3 ms and
~400 MB, and *outside* the upscaled path
([`frostbite_rendering.md`](frostbite_rendering.md) §7), which is the cost
context Epic's docs do not give.

**Water** — the Water plugin (water bodies, waves, a landscape-integrated
system) plus the Single Layer Water shading model.

**MetaHuman Crowd (5.8, experimental)** — "scale to thousands of characters"
[EPIC] — and **Mutable** (production-ready in 5.8), which is runtime mesh and
texture composition for character customisation, with "parallel updates to boost
generation throughput for crowds, reduced game thread workload" [EPIC].

## 8. The image chain

| Stage | What UE5 offers |
|---|---|
| Anti-aliasing / upscaling | **TSR** (default), TAAU, FXAA, MSAA (**forward renderer only**), **SMAA** (experimental, 5.7) [EPIC] |
| Vendor upscalers | DLSS / FSR / XeSS as plugins, outside the engine's own path |
| Dynamic resolution | consoles since 5.0; **PC (DX12 and Vulkan) as of 5.8** [EPIC] |
| Post stack | bloom (standard and FFT convolution), depth of field, motion blur, auto-exposure and **local exposure**, lens flares, chromatic aberration, film grain, colour grading with OCIO, tonemapping |
| Reference renderer | **Path Tracer** — "progressive, hardware-accelerated rendering mode... physically correct and compromise-free global illumination, reflection and refraction", sharing the ray-tracing architecture, integrated with Sequencer and Movie Render Queue [EPIC] |
| Offline output | **Movie Render Graph**, production-ready in 5.8, with a new accumulation depth-of-field system [EPIC] |

TSR's own claims: 1080p internal reaching "near 4K" output, "reducing GPU frame
time by half" against native 4K; less ghosting than UE4's TAA; "reduced
flickering on geometry with high complexity, such as those rendered with
Nanite"; runs on D3D11, D3D12, Vulkan, Metal, PS5 and Xbox Series, with
console-specific shader optimisations [EPIC].

**The Nanite/TSR pairing is not a coincidence** [inferred]: pixel-scale geometry
produces sub-pixel triangles, sub-pixel triangles alias catastrophically, and a
temporal accumulator is the only affordable answer. You do not get to adopt the
geometry half without the image half.

## 9. The plumbing that makes it possible

This is the least-discussed and most transferable third of the list.

**Render Dependency Graph (RDG)** — "an immediate-mode API which records render
commands into a graph data structure to be compiled and executed", giving
[EPIC]:

- scheduling of async-compute fences,
- **allocation of transient resources with optimal lifetimes and memory
  aliasing**,
- sub-resource transitions using **split barriers**,
- parallel command-list recording,
- culling of unused passes and resources,
- API-usage validation, and RDG Insights for visualising the graph and memory
  lifetimes.

**The mesh drawing pipeline** — retained mode, and the caching is the point:
`FPrimitiveSceneProxy` → `FMeshBatch` → `FMeshDrawCommand`, where the last is "a
fully stateless draw description" that can be **cached and merged just above the
RHI level**, exploiting the fact that static meshes change rarely [EPIC]. Note
the decoupling Epic call out explicitly: `FMeshBatch` means "the proxy never
knows what passes it will be rendered in" [EPIC] — which is the same arrow
cromwell's `IGeometrySource` draws, one interface with two methods, and for the
same reason [SRC: `render/ScenePipeline.hpp`].

**PSO precaching** — automatic collection and async compilation of every
pipeline state object that could be used, replacing the "ship a bundled cache
collected by playing the game" workflow, with policy knobs for what happens when
a proxy is created before its PSO has compiled (wait, or substitute the default
material) [EPIC]. This is the machinery behind shader-compilation stutter, and
it is a problem an engine only discovers once it has permutations.

**World Partition / HLOD / streaming** — grid-cell streaming with HLOD proxies,
plus **Fast Geometry Streaming** (experimental, 5.6→5.8) and **World Partition
Insights** (5.8) for per-cell profiling [EPIC].

**Culling** — view frustum, hardware occlusion queries, precomputed visibility
volumes, cull distance volumes, plus HZB culling inside Nanite [EPIC].

**Mesh Terrain (5.8, experimental)** — a mesh-based successor to the heightfield
Landscape [EPIC].

## 10. The comparison table

cromwell's column is read from source on 14 Aug 2026 — the RHI `ScenePipeline`,
which is the path the migration is heading for, not the older raylib renderer
[SRC].

| Area | UE5 high-fidelity | cromwell today | Verdict |
|---|---|---|---|
| Geometry LOD | Nanite: virtualized clusters, auto LOD, streaming, assemblies/voxels/skinning | one mesh, no LOD, no streaming; bounded board | **Blocked on GL 4.3** (§13) and not needed at board scale |
| Draw submission | retained-mode cached `FMeshDrawCommand`s, merged above the RHI | `IGeometrySource` submits per bucket each frame | Same arrow, no caching layer yet. Worth building **when draw count, not fill, becomes the cost** |
| Global illumination | Lumen (SWRT/HWRT) at 4–8 ms/1080p, or Lumen Lite irradiance fields | analytic sky ambient + ground bounce; reflection probes give specular only | The **irradiance-field rung** is reachable; full Lumen is not |
| Reflections | Lumen reflections; SSR on the medium rung | cubemap array, one per room, round-robin capture, parallax-corrected, GGX-prefiltered mip chain | cromwell is roughly at RE ENGINE's relit-cubemap rung. Missing: **SSR for contact detail** |
| Direct lighting | MegaLights: stochastic, ray-budget-bounded, thousands of shadowed area lights | one sun. Clustered forward is planned, not built | **The largest real gap**, and clustered forward is the right next rung |
| Shadows | VSM: 16k virtual, 128px pages, depth-buffer-driven allocation, cross-frame caching, clipmaps | one 4096 map, frustum-fitted sphere, texel-snapped, PCSS | Correct for a bounded board. The **caching idea** is what transfers if the world grows |
| Translucent shadows | per-light, ray-traced through | dedicated transmission pass writing an RGBA8 tint plane at half res | cromwell has a real answer here already |
| Materials | Substrate slabs, principled BSDF, platform-driven simplification, two G-buffer formats | `.mat` data files → UBO blocks; **untextured** — no albedo/normal/roughness maps yet | Textures are the next step; **the slab idea is worth reading before the texture work locks the model in** |
| Textures | RVT + SVT, UDIM, VT lightmaps | none virtual | Not needed until the world stops fitting in memory |
| AO | Lumen (which replaces DFAO); DFAO on the baked path | SSAO with a bilateral blur, half the story of one pass | Adequate |
| Anti-aliasing | TSR (temporal, upscaling, motion-vector driven) | **2× supersample and a resolve** | See §12 — this is the cheapest large win available |
| Volumetrics | froxel volumetric fog, clouds, FSSS, VDB volumes | none | `rdr2_atmospherics.md` already holds the blueprint |
| Post stack | bloom/DOF/motion blur/local exposure/OCIO | tone map + exposure | Cold code; add on demand |
| Frame graph | RDG: transient aliasing, split barriers, async fences, pass culling | hand-ordered pass list in one class | Correct at ~10 passes. **RDG's payoff starts when passes outnumber what one header can hold** |
| Shader pipeline | PSO precaching, permutation control, DDC | `ShaderLibrary`, compiled at load | Fine until permutations multiply |
| Profiling | Insights, GPU visualiser, RDG Insights, per-feature visualisers | F1 panel + F9 Chrome traces, CPU and GPU zones | **cromwell is ahead of where a project this size usually is** |

## 11. What transfers, ranked

1. **Clustered forward for many lights** — already the plan, and MegaLights is
   the evidence for where the ceiling is rather than a reason to change course.
   Cost is bounded by lights-per-cluster; measure that, not the light count.
2. **A velocity buffer and a temporal resolve.** cromwell supersamples 2× and
   downscales — brute force that costs 4× the fill and buys one frame's worth of
   edge quality. Motion vectors plus a temporal accumulator is the same money
   spent across frames instead of within one, and it is the thing every feature
   in §1–§8 quietly depends on (§12).
3. **The irradiance-field/probe-with-occlusion rung.** Lumen Lite, GIBS and RE
   ENGINE's tetrahedral network all landed here. cromwell already owns a probe
   set with a capture schedule — extending it from specular cubemaps to an
   irradiance term is an extension, not a new system.
4. **Substrate's principled parameterisation** — specifically the claim that a
   physically-parameterised material can be *automatically* simplified per
   platform. Worth reading before textures land in `.mat`, because the shape of
   the material model is expensive to change afterwards.
5. **Shadow-page caching as a cost model** — "how much was invalidated" rather
   than "how many casters". Only pays once the world is bigger than one frustum
   -fitted map, but it is what to reach for then.
6. **Cached, stateless draw commands** — the `FMeshDrawCommand` idea, when draw
   submission becomes the cost rather than fill.
7. **RDG's transient-resource aliasing** — the pass list is hand-ordered and
   legible today; this is the answer when it stops being.

## 12. The one structural thing to take away

**UE5's high-fidelity path is not a set of features you can adopt one at a time
— it is a budget architecture.** Nanite produces sub-pixel triangles; sub-pixel
triangles alias; TSR fixes the aliasing *and* pays for Lumen by rendering at
1080p and reconstructing 4K; Lumen's quality ladder is defined in ms at 1080p;
VSM's resolution only makes sense against Nanite's detail; MegaLights' ray
budget is denoised temporally. Epic say the quiet part in the Lumen performance
guide: rendering at a lower internal resolution and upsampling "gives better
final image quality than running Lumen natively at 4k with significantly lower
quality settings" [EPIC].

So the honest reading for cromwell is: **the temporal reconstruction layer is
the prerequisite, not the polish.** Every other feature on this list is priced
in reconstructed pixels. A renderer without motion vectors that adds an
expensive lighting feature pays the full 4K price for it — and that is why item
2 in §11 is ranked above the GI work it looks less exciting than.

## 13. What is blocked rather than expensive

Under the current GL 4.3 target [inferred, from the feature levels the
techniques require rather than from any Epic statement]:

| Feature | Blocker |
|---|---|
| Nanite-class visibility-buffer rasterisation | 64-bit image atomics; not core in GL 4.3 |
| Lumen hardware ray tracing, MegaLights RT shadows, Path Tracer | no ray-tracing API in OpenGL at all |
| VSM page tables | needs sparse/virtual texture support (`ARB_sparse_texture`-class) plus heavy compute |
| Bindless material binding | extension-only on desktop GL |

Lumen's **software** path is the interesting exception: it traces mesh distance
fields, cromwell already has `sdf/DistanceField.hpp`, and distance-field tracing
is ordinary compute. That is the one rung of the GI ladder the current RHI can
physically reach — which is worth knowing before the RHI grows a Vulkan back-end
for reasons that turn out to be about something else.

## 14. Sources

All Epic pages fetched 14 August 2026, serving 5.8 documentation.

- [Unreal Engine 5.8 Release Notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-8-release-notes)
- [Nanite Virtualized Geometry](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine)
- [Nanite Foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)
- [Lumen Global Illumination and Reflections](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine)
- [Lumen Performance Guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-performance-guide-for-unreal-engine)
- [MegaLights](https://dev.epicgames.com/documentation/en-us/unreal-engine/megalights-in-unreal-engine)
- [Virtual Shadow Maps](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine)
- [Overview of Substrate Materials](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-substrate-materials-in-unreal-engine)
- [Temporal Super Resolution](https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-super-resolution-in-unreal-engine)
- [Anti-Aliasing and Upscaling](https://dev.epicgames.com/documentation/en-us/unreal-engine/anti-aliasing-and-upscaling-in-unreal-engine)
- [Virtual Texturing](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-texturing-in-unreal-engine)
- [Volumetric Fog](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-fog-in-unreal-engine)
- [Heterogeneous Volumes](https://dev.epicgames.com/documentation/en-us/unreal-engine/heterogeneous-volumes-in-unreal-engine)
- [Path Tracer](https://dev.epicgames.com/documentation/en-us/unreal-engine/path-tracer-in-unreal-engine)
- [Render Dependency Graph](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)
- [Mesh Drawing Pipeline](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine)
- [PSO Precaching](https://dev.epicgames.com/documentation/en-us/unreal-engine/pso-precaching-for-unreal-engine)
- [Visibility and Occlusion Culling](https://dev.epicgames.com/documentation/en-us/unreal-engine/visibility-and-occlusion-culling-in-unreal-engine)
- [Post Process Effects](https://dev.epicgames.com/documentation/en-us/unreal-engine/post-process-effects-in-unreal-engine)
- [MaterialX](https://dev.epicgames.com/documentation/en-us/unreal-engine/materialx-in-unreal-engine)
- [Unreal Engine 5.7 is now available](https://www.unrealengine.com/news/unreal-engine-5-7-is-now-available) — Substrate production-ready, Nanite Foliage, MegaLights beta, SMAA
- [Tom Looman — UE 5.8 performance highlights](https://tomlooman.com/unreal-engine-5-8-performance-highlights/) — independent summary of the 5.8 maturity moves

## 15. What is not established here

- **No frame capture, no measurement.** Every cost in this note is Epic's own
  published figure at Epic's stated resolution. Nothing here was profiled, and
  §12's argument is a reading of their budgets, not a measurement of them.
- **No Nanite, Lumen or VSM internals** beyond what the public docs describe.
  The Nanite cluster format, Lumen's surface-cache update scheduling and VSM's
  invalidation heuristics are all in the shipping source and none of it was
  read.
- **The GL 4.3 blockers in §13 are inferred**, from what the techniques require,
  not from any statement by Epic about OpenGL — Epic dropped the GL back-end
  years ago and say nothing about it.
- **No shipped-game evidence.** This is what the engine offers, not what any UE5
  title actually enables; §0 notes that upgraded projects keep the old paths, so
  the two diverge badly in practice.
