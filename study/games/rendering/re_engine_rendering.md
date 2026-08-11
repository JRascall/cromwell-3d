# RE ENGINE rendering — reference notes

How Capcom's in-house engine draws what it draws, read for the same reason as
[`source2_rendering.md`](../valve/source2_rendering.md) and
[`rdr2_atmospherics.md`](rdr2_atmospherics.md): the transferable part is
architectural. RE ENGINE is the third reference in this study folder and it is
the one whose *problem* most resembles ours — see §1.

Capcom publish unusually well. Ten years of CEDEC talks and three CAPCOM Open
Conferences are online as slide decks, most with an English edition, and they
carry formats, resolutions and per-pass millisecond costs. Everything here is
from those decks unless marked otherwise.

| tag | source |
|---|---|
| **[CEDEC16]** | CEDEC 2016 — *Rendering technology behind Resident Evil 7*. The foundational architecture talk. |
| **[CEDEC18]** | CEDEC 2018 — *Graphics optimisation in recent titles* (RE:2, DMC5). |
| **[COC22]** | Open Conference RE:2022 — *Efforts toward GPU-driven rendering*. |
| **[COC23]** | Open Conference RE:2023 — *Is Rendering Still Evolving?*, *Advances in Ray Tracing*, *New Rendering Features Rundown*, *RE4 Hair*, *RayTracingLensFlare*. |
| **[GDC26]** | GDC 2026 — *Implementing Real-Time Path Tracing in RE ENGINE for Resident Evil Requiem and PRAGMATA*. |
| **[NV]** | NVIDIA developer-blog Q&A with Capcom on the same work. |
| **[GDC19]** | GDC 2019 — *Optimization Techniques: RE2 / DMC5*. 110 slides, official English, distributed via GPUOpen. |
| **[tool]** | Observed from shipped tooling — Autodesk's Dragon's Dogma 2 tools piece, the RELit modding docs, engine enums and material files read by community tooling. Descriptive, not Capcom's engineering word, and the material-file archaeology in particular is unverified. |
| **[3P]** | Third-party technical analysis — Digital Foundry, ComputerBase, TechPowerUp — and shipped graphics menus. Observation of the output, never a claim about the implementation. |
| **[inferred]** | Our reading, not Capcom's. |

> **Caveat on the numbers.** These decks are Japanese slide decks read through
> machine extraction and translation. The structure and the technique names are
> reliable; individual millisecond figures and buffer formats are transcriptions
> and should be re-checked against the deck before anything depends on them.
> Every figure below is linked to the deck it came from for exactly that reason.

**Which title is the reference.** RE ENGINE spans RE7 (2017, PS4-era deferred)
to Resident Evil Requiem and PRAGMATA (2025–26, full path tracing). Those are
different renderers wearing one name. This project targets desktop realtime on
**GL 4.3 through raylib — compute available, no ray tracing** — so the
**RE7 / RE:2 / DMC5 generation is the primary reference** and the RT/PT line is
read for its *decisions* rather than its code. §6 is deliberately short for that
reason. The one exception is §5, where a 2023 technique turns out to be cheaper
for us than it was for Capcom.

> Earlier revisions of this paragraph said GL 3.3 and "no compute shaders".
> That was wrong when it was written: `CMakeLists.txt` forces
> `OPENGL_VERSION "4.3"` for cubemap arrays, so compute, SSBOs and indirect
> draw have been core in this context all along. §9 works through what that
> actually buys. The barrier is raylib's API surface, not the GL version.

---

## 0. Status of this document against the code

**Checked against the source on 2026-08-11.** This section exists because the
ranked recommendations in §11 are only useful if it is clear which of them have
since been acted on, and because one of them had already been acted on without
this document noticing.

| §11 item | state in the renderer |
|---|---|
| 11.1 world SDF — AO, soft shadows, leak rejection | **not built.** Nothing samples a world distance field. `assets/shaders/common/sdf.glsl` is the MSDF *text* decode and is unrelated — do not mistake its presence for progress here. |
| 11.2 four-direction probe storage | **not built.** Indirect diffuse is still the two-lobe analytic hemisphere in `common/environment.glsl`, scaled by a single hand-tuned `uAmbientIntensity` of 0.42. There is no bounce anywhere in the renderer. |
| 11.3 64³ grading LUT after the tone curve | **not built.** `common/filmic.glsl` is Hable with a fixed exposure uniform and a `pow(1/2.2)` encode. No LUT, no dither. |
| 11.4 relit cubemaps | **largely moot** — see below. |
| 11.5 keep two shadow maps, do not composite | **honoured by construction.** `common/shadow.glsl` has the baked path answer or decline (`bakedSunVisibility` returns −1) and hand the fragment to the shadow-map path. The two never composite, so Capcom's 0.1 ms-per-map copy has no analogue here. |
| 11.6 do not build OIT | **honoured, and the positive half is still open.** No OIT, correctly. But the recommendation was *"sort a handful of transparent draws back to front"*, and there is no such sort: `StaticsMesh` splits opaque from transparent and draws the second group after the first, in whatever order the sub-meshes happen to sit. Premultiplied output and the `ONE / ONE_MINUS_SRC_ALPHA` blender are in place. Invisible so far because the map has one pane per opening and they rarely overlap; the first stacked panes will show it. |
| 11.7 hash-map auto-instancing | **not built, deferred by decision.** `PropSet` still loads no props. |

Two entries need more than a table cell.

**§11.4 is largely moot, and for a reason worth recording.** Capcom need the
`luma(ProbeDiffuse) * luma(cube(r,mip)) / luma(cube(n,lowestMip))` renormalisation
because their local cubemaps are *baked offline* and are therefore wrong under
any lighting they were not baked for. `ReflectionProbeSet` captures at runtime
instead — a round-robin cursor over (probe, face) pairs, one face per frame, so
a twelve-room board fully re-settles in about 1.2 s — which means our probes are
never stale to a *lighting* change, only latent by about a second. We bought the
same correctness with a scheduler rather than with a shader trick. The technique
is still worth knowing if the capture ever becomes too expensive to run
continuously.

**§3.1's 32-light shadow cache has no analogue here, because there are no local
lights.** The renderer has exactly one directional sun. No point lights, no spot
lights, no light grid, no froxels — `BlastFlashes` is an overlay, not a light.
That is the single largest capability gap against any RE ENGINE generation, and
it is not a shadow problem: it is the reason every interior is lit by sky
ambient and whatever sun reaches through a window.

---

## 1. The one thing that matters most

**RE ENGINE is organised around what can be cached, and around making cache
repair cheap enough to do every frame.** Not around what can be baked — around
what can be *incrementally re-baked*.

That through-line is visible in every deck, ten years apart:

- Shadow maps of static geometry are **cached per light** and only recomputed
  when a dynamic object enters that light's frustum. **[CEDEC16]**
- Local cubemaps are baked once and **relit at runtime** from a probe, so the
  bake survives lighting changes it was not baked for. **[CEDEC16]**
- Signed-distance clipmaps are updated **differentially against a per-grid
  checksum**: a full update is >20 ms, the differential update is <1 ms.
  **[COC23]**
- Directional shadow maps up to 32K² are baked and **quadtree-compressed** to
  2–70 MB so the cache is affordable to *keep*. **[CEDEC18]**

Compare this project's own history — and note that the comparison has changed
since this section was first written. An earlier revision said that
[`source2_rendering.md`](../valve/source2_rendering.md)'s direct-sun lightmap
bake was sunk by staleness under destruction, and drew the lesson that Capcom's
answer is to make invalidation local and differential rather than to delete the
cache. **Both halves of that were wrong, and correcting the second one makes the
lesson sharper rather than weaker.**

**The differential re-bake is already built.** `SunBaker::bakeRegion` bakes the
blast's own cells plus everything those cells were shading — `collectShadowShaft`
marches each changed cell along the sun direction, which with exactly one sun is
a *line* rather than a hemisphere — and `refreshGeometry` carries surviving
texels across a stable `(cell, face)` key, so patch slots can move without the
whole map needing a re-bake. That is Capcom's principle, implemented, and the
sibling document's own measurements confirm it works: **zero stale texels at
every blast site tested.**

**And the bake is not switched off because re-baking is expensive.** Its verdict
is a *quality* verdict — read
[The bake, and why it is shelved](../valve/source2_rendering.md#the-bake-and-why-it-is-shelved).
Re-bake cost is the third and least of three candidate explanations, behind
"only cell faces are baked, so a frame mixes baked and shadow-mapped shadows
with unmatched softness and different edge character" and "a five-face-per-cell
patch cannot represent sub-cell geometry". Reducing that to "re-baking is
expensive" misread the document it was citing.

So what does RE ENGINE actually have to say to us here? Something more specific.
Our differential update is differential in the right *shape* and still costs
36 ms at a real grenade radius, rising to 1.6 s at the stress radii, because
repairing it means path-tracing rays again. Capcom's equivalent figure is
**>20 ms full, <1 ms differential** — three orders of magnitude tighter — and
they get that because the thing they repair is a **distance field**, updated by
hashing grid cells against a checksum. A chamfer sweep is not a ray trace.

The transferable rule is therefore not "make invalidation differential", which
is done. It is: **cache geometry rather than lighting, because geometry is the
thing whose repair is cheap, and lighting can then be derived from it every
frame.** That is §5 and §11.1 arrived at from a second direction, and it is the
strongest argument in this document for building the SDF.

---

## 2. The pipeline — RE7, and what the remakes changed

**Deferred, four render targets, one lighting model everywhere.** **[CEDEC16]**

> All shaders use one identical lighting model: **Lambert + Cook-Torrance
> (GGX)**. **[CEDEC16]**

That is stated as a goal, not an implementation detail: static background and
dynamic character must be interchangeable under the same lights, so there is one
BRDF and no "character shader".

### G-Buffer layout **[CEDEC16]**

| RT | contents | format |
|---|---|---|
| RT0 | Emissive (optional) | R11G11B10Float |
| RT1 | BaseColor (sRGB) + Metallic/Translucency in alpha | R8G8B8A8Unorm |
| RT2 | Normal XY, Roughness Z, misc A | R10G10B10A2Unorm |
| RT3 | Occlusion (7 bit) + SSS (3 bit), + velocity | R10G10B10A2Unorm |

Two details are worth more than the layout itself:

- **Translucency shares the alpha channel with metalness.** One scalar
  interpolates between perfect diffuse and full translucency, which is how
  leaves and lampshades are lit without a second shading model. **[CEDEC16]**
  This is the same economy as Source 2's `F_TRANSMISSIVE_BACKFACE_NDOTL`
  ([`source2_rendering.md`](../valve/source2_rendering.md) §12.1) but folded into the
  standard material rather than gated behind a feature flag.
- **Subsurface gets three bits.** Not a mode, not a separate pass input — a
  three-bit selector packed next to occlusion. Skin in a title whose entire
  reputation rests on faces is a *material index*, not an architecture.

### Frame budget, RE7, 1080p/60 **[CEDEC16]**

| pass | cost |
|---|---|
| common / wrinkle maps | ~0.85 ms |
| G-Buffer + decals | ~2.7 ms |
| shadows + async lighting | ~3.0 ms |
| lighting (direct, SSR, SSS) | ~3.5 ms |
| transparent | ~3.5 ms |
| post (bloom, DOF, tonemap, colour correct, lens distortion) | 1.8–2.5 ms |
| UI | 0.3 ms |

Worth internalising that **transparency costs as much as all direct lighting**,
in a game with very little glass. That is the number to remember before building
the sorted transparent pass [`source2_rendering.md`](../valve/source2_rendering.md) §12.4
lists as missing.

### 2.1 What Capcom have never published about the remakes

Stated up front, because the absence is itself a finding and because it is the
reason the rest of this document leans so hard on RE7-era material.

**There is no published RE ENGINE G-Buffer layout later than RE7's, in any
language.** The four RE:2023 rendering decks contain no channel table, no bit
depth, and no statement of the shading model or BRDF after CEDEC 2016. Nor is
there any RE2R- or RE3R-specific rendering talk beyond CEDEC 2018 and GDC 2019,
**any RE4R lighting talk at all**, any cascade count or per-cascade shadow
resolution for any remake, any per-pass GPU millisecond figure for any of the
three, or any published frame breakdown of an RE ENGINE game.

> **A trap worth flagging.** Capcom R&D's deck archive contains a CEDEC 2017
> Monster Hunter: World talk with a full **six**-render-target G-Buffer table,
> and it is tempting to read it as RE ENGINE's next layout. **Do not.** MH:W is
> MT Framework; the deck sits in that archive because the same R&D division
> presented it, and its body never names an engine. The existing warning at the
> bottom of this document's source list says the same thing about MH:W's
> rendering talks generally, and this is the specific way someone would get
> caught.

### 2.2 The one shading change Capcom credit to RE4R

*Is Rendering Still Evolving?*, slides 33–37 **[COC23]**, official English, and
the only thing in any Capcom publication attributed to RE4R's general shading.

The problem: normal mapping produces shading normals that face **away** from the
viewer, which then sample the environment map **below the horizon** and go
black. The fix **uses the geometry normal to correct the shading normal**, is
toggleable per shader, and "is used by many shaders". Slide 35: *"Corrected
invisible normals to be visible when shading — **Introduced in Resident Evil
RE:4**."*

Capcom give no algorithm name, no formula and no citation, and acknowledge the
technique has drawbacks without listing them. The candour on slide 34 is worth
quoting because it is unusually honest about why a known artefact survives for
six years: *"We, too, have been aware of this problem for a long time, but have
ignored it. However, we are indeed in the middle of the ninth generation, so we
wanted to do something about it."*

**Directly relevant to us**, and cheap. `pbr.fs.glsl` already keeps the geometric
normal apart from the mapped one — it has to, because the shadow lookup is
biased along the geometry's own orientation — so both vectors are in hand at the
point `environmentSpecular` is called. Nothing currently stops a
strongly-mapped surface reflecting the ground lobe of `skyIrradiance`. This is a
few lines in a function we already have, and it is the kind of small correctness
fix whose absence reads as "the materials look cheap" rather than as a bug.

### 2.3 Subsurface scattering

**Screen-space blurring**, stated in the GDC 2026 path-tracing deck: *"RE ENGINE
implements subsurface scattering using screen-space blurring."* **[GDC26]**

That is a 2026 statement about Requiem, and Capcom published nothing on SSS
between RE7 and the remakes — so read it as establishing the *family* (a
screen-space blur, not an integrated BSSRDF) rather than as a measurement of
RE4R. RE4R exposes an SSS on/off graphics toggle and cuts SSS entirely on PS4
**[3P]**, which is consistent with a separable screen-space pass that can simply
be skipped.

---

## 3. Shadows — three systems, and one of them is ours

### 3.1 The shadow cache

Shadow maps live in a **`Texture2DArray`, default 1024×1024×32, D32Float or
D16Unorm** — 32 shadow-casting lights in one array. **[CEDEC16]**

The cache rule is simple and stated plainly: **while a light does not move, its
shadow of the static scene is cached**; the moment the light moves, caching
stops. Dynamic objects are composited in **only when they are inside that
light's frustum**. **[CEDEC16]**

The interesting part is the cost they were fighting:

> Assuming one shadow-map copy costs 0.1 ms — copying 32 of them costs
> 3.2 ms. **[CEDEC16]**

So they push the copy onto async compute or DMA, and skip it entirely when
nothing dynamic is in frustum. **[CEDEC16]**

**This corroborates [`source2_rendering.md`](../valve/source2_rendering.md) §9.4 and
improves on it.** That section proposes a static map A (board-locked, redrawn on
destruction only) and a dynamic map B (tightly focused, every frame), shaded with
`min(A, B)`. Capcom arrived at the same static/dynamic split from the same
pressure. The improvement is negative knowledge: **they pay a copy because they
composite into one map, and the copy is their dominant cost.** Our `min(A, B)`
plan avoids the copy entirely by keeping two maps and combining in the shader.
That is the right call, and Capcom's 3.2 ms is the evidence for it. Do not
"simplify" later by merging the two maps.

### 3.2 Baked, compressed directional shadow maps — "Sparse Shadow Tree"

For static sun shadow over large worlds, RE:2/DMC5 bake the directional light's
depth at **16K² or 32K²** and compress it with a **quadtree over Morton-ordered
blocks**, where a leaf stores depth plus DDX/DDY gradients at 16-bit precision
and interior nodes store child indices. Shipping assets are **2.15 MB to
70.7 MB** LZ4-compressed, and the shadow pass drops **36 ms → 20 ms** on a
GTX 1070 Ti. **[CEDEC18]**

**Capcom's own name for this is `Sparse Shadow Tree`**, which the CEDEC 2018
talk never says and the RE:2023 SDF talk does — closing a loop that was open in
this document for a while. It matters because of the sentence it appears in,
which is the clearest statement Capcom have made about why they moved on
**[COC23]**:

> "In the past, RE ENGINE used a lot of pre-baked assets, which assumed that
> light sources and objects would not move — **Sparse Shadow Tree, Light Map,
> etc.** In order to support games where the time of day changes, new methods
> were needed — Ray Tracing or Signed Distance Field."

**And they chose the distance field, on cost.** The speaker notes say real-time
ray tracing was considered and rejected because "it is difficult to take full
advantage of ray tracing with the performance of today's prevalent hardware".
That is a shipped engine, with hardware RT already in four of its titles,
choosing an SDF over rays for the general case — which is a considerably
stronger endorsement of §5 than a document written by someone who *cannot* ray
trace could otherwise claim.

Storing a *gradient* per leaf rather than a constant is the trick: a shadow depth
buffer is piecewise-planar almost everywhere, so a plane fit per block compresses
far better than a value per texel. **[inferred]** Not directly useful to us —
our board is 24×24, a 4096² map already gives ~110 texels per tile, and nothing
needs compressing — but the plane-fit observation is worth remembering if the map
ever grows.

### 3.3 Softness — and the absence where PCSS should be

**Capcom have published nothing on contact hardening, PCSS, or an area-light
shadow approximation, for any title at any date.** Given how much they publish,
that absence is worth treating as informative rather than as a gap in the
research.

What evidence exists points somewhere else entirely, and it is tooling-derived
rather than Capcom's engineering word. RE ENGINE lights carry a
**`ShadowVariance`** property, which the RELit modding documentation labels
"Shadow Blur" while noting the label is wrong **[tool]**. A **variance**-based
filter with a global softness scale is not PCSS — and light bleeding through
thin occluders is the variance-shadow-map family's signature artefact.

There is a second, weaker signal from the opposite direction. The RE4R hair talk
mentions in passing that its *original* approach to shadowing hair was
screen-space pixel-dither sampling "as with opaque objects" **[COC23]** — which
says, almost incidentally, that **RE4R's opaque geometry takes its shadows
through a dithered stochastic sample**. Capcom never elaborate.

**So our shadow filtering is very likely ahead of the remakes'**, which is a
strange thing to be able to write and worth stating plainly. `common/shadow.glsl`
does a blocker search, sizes the penumbra from the sun's real angular radius in
world units, and filters over a per-pixel-rotated Poisson disc that grows a
second ring past eight texels. Nothing published about RE2R, RE3R or RE4R
describes anything of that kind. The RE remakes look better than this renderer
for reasons that are almost entirely *elsewhere* — indirect light, light count,
material authoring and colour — and reading their shadow edge as the thing to
chase would be chasing the wrong number.

One caveat before that goes to anyone's head: **cinematic shadows in these games
are manually tuned per shot** (§4.5), so the shots that sell the look are not
purely systemic output.

### 3.4 The cache survived the remakes

The RE7 cached-shadow mechanism is still running in all three. The engine's
shadow pass carries a **`CacheCopy`** segment alongside `StaticShadow` and
`DynamicShadow` **[tool]**, and all three remakes expose a user-facing
**"Shadow Cache"** graphics option **[3P]**. Whatever else changed after RE7,
the static/dynamic split and its copy did not.

### 3.5 SDF shadows

This is the important one and it gets its own section — §5.

---

## 4. Indirect light

### 4.1 Irradiance volumes — a tetrahedral probe network

**[CEDEC16]**, and this is the closest published analogue to the probe grid
[`source2_rendering.md`](../valve/source2_rendering.md) §11 commits to.

| | RE ENGINE |
|---|---|
| placement | geometry is **voxelised**, probes seeded from that in two sparse hierarchical stages |
| topology | probes tetrahedralised; the tetrahedral mesh is converted to a **BSP tree** for stackless traversal |
| per-probe storage | radiance in **four directions**, each **R11G11B10Float** |
| lookup accelerator | a uniform **32×32×32 grid** over the BSP tree, plus reuse of the previous frame's tetrahedron ID when the camera barely moved |
| scene size | 9 628 probes |
| build | **80 s** on a Core i7-4790, offline |
| runtime | **1.5 ms** at 1080p, on async compute during the shadow pass |

Three things to take from this.

**Four directions, not spherical harmonics.** Nine SH coefficients per colour
channel is the reflex choice; Capcom store four directional colours at 32 bits
each, 16 bytes a probe. For a lattice of 24×24×9 = 5184 cells that is **83 KB for
the entire world's indirect light** — small enough to hold two copies and blend,
small enough to upload every frame as a 3D texture. Our current ambient is a
two-lobe hemisphere; four directions is the next rung up the same ladder and it
is a much smaller step than SH.

**The placement problem is the expensive half, and we do not have it.** Voxelise
the scene, seed sparsely, tetrahedralise, build a BSP, then build a 32³ uniform
grid *to accelerate queries into the BSP*. That entire apparatus exists because
probes sit at irregular positions in a world made of triangles. Our probes sit
one per lattice cell on a regular grid, so the lookup is an array index — no
tetrahedra, no BSP, no accelerator, no 80-second bake. This is the same shape of
argument as [`source2_rendering.md`](../valve/source2_rendering.md) §14's smoke voxeliser:
**the step every other engine finds hardest is free here because the world is
already a voxel field.** It is now the third time that observation has paid.

**1.5 ms at 1080p is what a probe volume costs to *sample*.** Our §11 estimate of
~1 ms/frame covers only the *tracing*, on CPU, round-robined over 8 frames. The
sampling cost is separate and Capcom's figure is the one to budget against.

### 4.2 Local cubemaps, relit at runtime

Baked cubemaps are stored **256×256 BC6H** on disk and expanded at runtime to a
**512×512 × 8 mip R11G11B10Float octahedral map** with edge-fix filtering.
**[CEDEC16]**

The part worth stealing is what they do about a baked cubemap being wrong when
the light changes. A cubemap baked in daylight, sampled in shadow, makes
everything read as metal — the specular term stays bright while the diffuse term
goes dark. Capcom renormalise it against the probe: **[CEDEC16]**

```
IndirectSpecular = luma(ProbeDiffuse)
                 * luma(LocalCubemap(r, mip))
                 / luma(LocalCubemap(n, lowestMip))
```

Read it as: take the cubemap's *shape* (the ratio of the reflected direction to
the cubemap's own average) and rescale it by the *level* the probe says this
point is actually receiving. The bake supplies directionality; the probe supplies
intensity. Attributed in the deck to [Lazarov 2014].

**Why this matters here specifically.** `source2_rendering.md` §10.3 wants
environment cubemaps and notes every metal currently reflects a smooth blue sky
ramp. It also rules out anything that goes stale under destruction. This
technique is the bridge: a cubemap baked before a grenade is *geometrically*
stale afterwards, but rescaling it by a live probe keeps its brightness correct,
which is the error that reads worst. It is not a fix for a wall that is no longer
there. It is a fix for the far larger and more common error of a reflection being
lit for the wrong time of day or the wrong side of a wall. **[inferred]**

### 4.3 What replaced all of this — and the correction that matters

**An earlier revision of this section said the probe-and-cubemap system was
"retired in favour of ray tracing". That was a misreading of the slide, and the
truth is close to the opposite: probes and local cubemaps were still the
indirect lighting of Resident Evil 4 (2023).** What changed was how probes are
*baked*, not whether they exist.

Here is the slide, verbatim, from *Is Rendering Still Evolving?* slide 14,
"Ray Tracing Applications: **Light Probes**" — the heading alone gives it away.
**[COC23]**, official English edition, speaker notes:

> "Conventionally, RE ENGINE **reuses cube map shots to create Light Probes**.
> However, there was a problem with light penetrating in multi-bounce. This is
> because the result baked in the first pass is interpolated in the second pass,
> and if the interpolation cannot be properly occluded, the result will look
> incorrect. When baked with Ray Tracing, it is just light tracing. It can
> handle occlusion properly, so multi-bounce can be represented correctly."

So the *bake* moved from rasterising cubemaps and interpolating them to tracing
rays. The probe network is the consumer of that bake and is untouched by the
change. Capcom attach **no date and no title** to the switch.

**Direct evidence that probes were alive in RE4R**, from the RE4-specific hair
talk, slide 43, "Indirect Lighting" — again official English **[COC23]**:

> "Compatible with **Light Probes, Local Cubemaps, and IBL** … I will introduce
> the handling of indirect light lighting. Indirect lighting includes Light
> Probes, Local Cubemaps, and IBL."

That is a shipping RE4R shader sampling all three. Corroborated by the RT deck's
own substitution rule: enabling ray tracing "replaces the traditional
approximation functionality — replaces **indirect illumination of opaque
meshes**". RT *substitutes for* the probe path where it is on. Where it is off —
and in RE4R it is off for GI entirely, see §4.5 — **probes are the GI**.

**Light leak is still the named failure mode**, and that part of the original
reading survives intact. It is the same conclusion `source2_rendering.md` §11
reaches about DDGI, and the same reason it argues our lattice can reject leaks
*exactly* rather than statistically. Two independent engines naming leak as the
thing that kills probe GI remains a strong signal that leak rejection is the
part not to skimp on.

But the corrected reading changes what this section is *for*. It is no longer
evidence that probe GI is a dead end which serious engines abandon. It is
evidence that **probe GI was good enough to carry the best-looking game in the
series**, and that the fix for its worst artefact was to trace the bake rather
than to throw the structure away. For a project that cannot ray-trace at all,
that is a much more encouraging fact than the one this section used to state,
and it raises the value of §11.2 accordingly.

### 4.4 Ray tracing per title — and why RE4R is the surprise

*Advances in Ray Tracing*, slide 41 **[COC23]**, official English:

| title | denoiser | ray-traced features |
|---|---|---|
| Devil May Cry 5 Special Edition | v1 | GI, reflections |
| Resident Evil Village | v2 | GI, reflections, AO |
| Resident Evil 2 / 3 / 7 | v2 | GI, reflections, AO |
| **Resident Evil 4** | v2 | **reflections only** |
| Exoprimal | v2 | GI, reflections |

Capcom's speaker note: *"Resident Evil 4 only uses ray tracing reflection for
performance issues."*

**Read that table again, because it is the single most useful fact in this
document for a renderer that cannot ray-trace.** RE4R — the most recent and
generally the best-looking of the remakes — ships with *less* ray tracing than
the three games before it. Its global illumination is the RE7-lineage probe and
cubemap system, its occlusion is ordinary SSAO, and Digital Foundry noticed
independently: *"the ray-traced global illumination of past RE titles has
disappeared in favour of classic screen-space ambient occlusion, with
reflections being the only noticeable RT effect."* **[3P]**

Whatever makes RE4R look the way it does, **it is not ray tracing.** It is a
probe network, parallax-corrected local cubemaps, screen-space occlusion, a
shading-normal correction worth one slide (§2.2), and an enormous amount of
material and lighting art. Every one of those except the last is architecture we
can reach.

Two footnotes that keep this honest. RE2R and RE3R **shipped with no ray tracing
at all** — it arrived in the June 2022 next-gen patch, alongside the DX11→DX12
move **[3P]** — so for their entire original life they ran probes and nothing
else. And RE4R ships **dormant RTGI/RTAO code paths** that mods re-enable
**[tool]**, which says the reduction was a budget decision rather than a removal,
consistent with Capcom's own "for performance issues".

### 4.5 What the RT path actually did, where it ran

Recorded briefly because it bounds the claim above. Inline ray tracing via
`RayQuery` through a single material function rather than DXR hit shaders,
chosen for IHV portability and bindless material access. Trace resolution
**960×540 at 1 ray per pixel**, halved again on console; disocclusion rays at
**120×67, 64 rays**. RT area lights — rectangle, disk and line — with both
direct and indirect area-light shadows, which is the only area-light-with-shadow
statement Capcom have published and is on the RT path only. **[COC23]**

**Direct lighting never moved onto the RT path in this generation.** NVIDIA's
2026 Q&A with Capcom states it plainly looking back: *"Previously, ray tracing
was used only for indirect lighting"*, and that traditional ray tracing *"relied
on shadow maps, which often suffered from limited resolution. While this was
less noticeable in cinematics due to **manually tuned shadows**, the lighting
quality could feel compromised during gameplay."* **[NV]**

That last clause is worth keeping. **The best-looking shadows in these games are
partly hand-tuned per cinematic shot**, which is a content answer to a technical
problem and not one a systemic renderer can copy.

### 4.4 Street Fighter 6 — ray tracing used as a *baker*

SF6 bakes **direct lighting into static lightmaps using ray tracing**, alongside
ambient occlusion, on selected stage sections. **[COC23]**

Worth noting because it inverts the usual framing: hardware ray tracing here is
not a runtime feature, it is a faster and more correct VRAD3. A fighting game has
fixed stages and a fixed camera, so the bake is never invalidated — the opposite
of our situation, and a clean illustration that the bake-versus-realtime question
is decided by *whether the world changes*, not by which technique is better.

---

## 5. Signed distance fields — the one to actually build

RE:2023 introduces a **per-mesh SDF, baked offline by ray tracing outward from
voxel centres**, stored as **R8Unorm with BC4 compression** — about **30% of the
VRAM of 16-bit float** — with values normalised 0–1 against a stored min/max
pair. **[COC23]**

They use it for four things, in descending order of payoff:

**1. Shadows to ~4 000 m.** Three compute shaders — **tile classification →
instance culling → draw shadow** — with a **fixed draw-call count regardless of
instance count**. Runs on async compute behind the ordinary shadow-map pass.
PS4: **2.5 ms** in a wide open area with 10 000 instances and up to half-screen
coverage (measured with async *off*), **<0.5 ms** in forest or city where the
distance is occluded. **[COC23]**

**It is a hybrid, not a replacement: cascade shadow map near, SDF far.** And the
consequence Capcom report is the one that would not have been guessed —
offloading the distance *improved the near-view shadows too*, because the
cascades no longer have to stretch to cover long range. A split that was made
for reach paid off in quality at the other end.

**2. Ambient occlusion.** 3 rays per thread with a 4×4 deinterleave = **48
directions**, marched ~2 m (capped near 3 m, past which the ray count is
insufficient and colour blotches appear), at **540p — half width and half
height of 1080p**. **Combined with SSAO, ~1 ms on PS4.** Penumbra-based partial
occlusion by march distance rather than a binary hit. **[COC23]**

**Temporal accumulation is deliberately avoided**: *"the image is completed
within a single frame and the result is always stable."* Worth noting against
the reflex to reach for a temporal filter — Capcom spent the sample budget on
getting a clean single frame instead, and this project has no motion vectors to
build a temporal filter on anyway.

**3. Light-leak prevention in GI.** **[COC23]** — cf. §4.3.

**4. Exposed to user shaders**, for distance-driven effects and distance-based
colour painting. **[COC23]**

The engineering they had to do to make it work is almost entirely about *many
meshes*: instance classification by AABB-size bit sets to skip candidates before
marching; step-distance limiting to mitigate overlapping instance fields; up to
four **128×64×128 clipmap** textures created at runtime to resolve overlap
(rebuilt differentially — **>20 ms full, <1 ms differential**, the figure §1
turns on); LOD0-only baking; no joint support. **Over 1 000 SDF textures are
resident at once**, which is why the whole thing needs bindless. 16-bit float
was tried and rejected at "several hundred MB of VRAM", which is what drove the
R8+BC4 choice. **[COC23]**

**One rejected approach is worth more to us than the accepted ones.** Capcom
tried a **BVH** to find nearby instances and went back to a **uniform grid**: it
"proved a costly way to search for structures, and tended to be slower than
simpler methods such as the uniform grid" — with the honest caveat *"it may have
been poorly implemented"*. A shipped engine choosing a flat grid over a tree for
a spatial query is the same conclusion `CLAUDE.md`'s performance rules reach
from measurement, and it is reassuring for a codebase whose entire spatial layer
is flat arrays indexed by arithmetic.

**Which title ships this is not stated, and the honest answer is probably not a
remake.** The SDF section names no game at all; its framing is "an open world
game with time variation" and its screenshots are unlabelled terrain, which
points at Dragon's Dogma 2 — i.e. **after** RE4R. Treat SDF as the
*post*-remake direction rather than as part of what makes the remakes look good.
**[inferred]**

### 5.1 Why this is cheaper for us than it was for Capcom

**Every one of those costs is an artefact of building an SDF out of instanced
rigid meshes. We do not have instanced rigid meshes. We have one voxel lattice.**

| RE ENGINE's problem | ours |
|---|---|
| bake an SDF per mesh by ray tracing from voxel centres | a **distance transform of the occupancy grid** — a standard two-pass chamfer sweep, no rays |
| classify and cull thousands of overlapping instance fields | there is **one field**, world-space, always |
| four runtime clipmaps to resolve instance overlap | none — no overlap exists |
| no joint support, LOD0 only, VRAM pressure from unique meshes | one texture, fixed size |

Sizing it: 4 samples per cell over a 24×24×9 lattice is **96×96×36 = 331 776
samples**, **324 KB at R8**. That is a rounding error against a 4096² shadow map.
A full chamfer distance transform of that grid is a few milliseconds of
single-threaded CPU work at worst; a *local* one after a grenade touches only the
blast box dilated by the maximum encoded distance, which is exactly Capcom's
differential clipmap update reduced to its simplest possible case.

### 5.2 What it would buy — including a third answer to the shadow problem

`source2_rendering.md` §9 frames dynamic-world sun shadows as a choice between a
bake (shelved) and shadow maps (§9.4's two-map plan). **SDF sphere tracing is a
third option that neither Source 2 nor RDR2 offered, and it sidesteps every
defect §9.1–9.3 lists:**

- No projection to refit, so **no texel crawl** (§9.1 does not exist).
- Penumbra falls out of the minimum cone ratio along the ray, in **world units**,
  so it does not change with zoom (§9.2 does not exist) and needs no PCSS blocker
  search (§9.5 comes free).
- **No re-render on destruction** — one local distance-transform update. The
  objection that shelved the bake does not apply, because the thing being
  invalidated is geometry, not lighting, and it is orders of magnitude smaller.
- No depth bias, no acne, no peter-panning, no cascades.

And AO comes off the same structure: **SDF AO sees geometry that is offscreen**,
which our SSAO cannot, and it needs no depth prepass. Capcom run both together —
SDF AO for the medium scale, SSAO for the contact scale — which is the right
split and roughly free to copy.

**The honest costs.** Sphere tracing per pixel in a fragment shader on GL 3.3 is
the real expense; Capcom's figures are compute shaders on async, which we have
neither of. Half-resolution with a blur, as they do for AO, is the mitigation and
it is already the shape of our SSAO pass. Soft shadows also want several cone
widths' worth of steps. This is a real experiment with a real chance of being too
slow at 1080p on GL 3.3, and it should be measured before it is designed around.
But it is the highest-leverage thing in this document, and the reason is that
**the expensive half of the technique — building and maintaining the field — is
nearly free for a game whose world is already a voxel grid.**

---

## 6. The ray-tracing line, read for its decisions

We cannot run any of this. It is recorded because the *ordering* of what Capcom
ray-traced, and why, is informative.

**The arc.** DMC5 SE (RT v1: GI, reflections) → RE Village and the RE remakes
(v2: GI, reflections, AO) → v3 (denoiser rework, mirrors, area lights) → RE
Requiem and PRAGMATA (full path tracing, direct lighting included). **[COC23]**,
**[GDC26]**

**Indirect first, direct last.** For five years ray tracing did indirect
illumination only; direct lighting stayed on shadow maps. Path tracing is
described as the point where *"direct lighting runs through the path tracer
instead of shadow maps"*, and the stated motivation is removing the visual
discontinuity between gameplay and cinematics. **[NV]** The GI-before-shadows
ordering matches `source2_rendering.md` §10's ranking, arrived at independently.

**RT GI cost, PS5, v3** **[COC23]**:

| stage | cost |
|---|---|
| tracing (diffuse + specular) | 1 583 + 609 µs |
| ray generation and sorting | 122 + 69 µs |
| shading (diffuse + specular) | 596 + 148 µs |
| spatial denoise | ~512 µs |
| **total ray tracing** | **~4.7 ms** |

**Path tracing cost, Dec 2025** **[GDC26]**: 8.78 ms at 1080p on an RTX 4070 Ti,
20.09 ms at 4K; 24.20 / 67.93 ms on an RTX 3060.

**The denoiser is most of the work.** The v3 talk is largely about GI denoising
under sparse diffuse samples: a moment buffer carrying geometry change against a
guided colour buffer carrying light-field change, disocclusion rays at 120×67
upscaled to 960×540, guided-direction accumulation over up to 500 frames,
spherical-harmonic projection for the final upscale. Machine learning was
evaluated and rejected as too expensive for current consoles. **[COC23]**

**Sampling structures worth knowing even without RT** **[GDC26]**:

- Punctual lights are culled into a **world-space 16×128×128 3D texture holding
  light IDs as a bitmask**, giving O(1) light sampling. If this project ever
  grows past a handful of local lights, a world-space light grid over the lattice
  is the same idea and the lattice is already the grid.
- Emissive polygons are sampled by **Walker's alias method**, pre-generated into
  a buffer (4 096 samples/frame, 32 RIS candidates) because doing it at runtime
  was too expensive.
- **Screen-space alpha test**: instead of a per-ray texture fetch for cutout
  geometry, project the ray hit onto the rasteriser's depth buffer and test
  there — *"an inverse shadow map"*. **[GDC26]**

**IBL had to be excluded from RIS candidates indoors**, because a high-intensity
environment map produces extreme variance where little of it is visible.
**[GDC26]** A sealed interior receiving strong sky light is a pathology in
Capcom's sampler for the same reason it is an artefact in our analytic ambient —
different symptom, same underlying error.

---

## 7. Transparency, hair, and what OIT actually costs

RE4's hair is **strand-rendered**, split by projected width: wide segments go
through **hardware rasterisation** as camera-facing quads, thin ones through a
**software rasteriser** for correct transparency. At 4K most strands take the
hardware path; at 8K nearly all do. **[COC23]**

Shading is **single scattering plus a dual-scattering approximation**, with a
**128³ forward-scattering voxel map** approximating hair opacity from the light,
**exponential shadow maps** for receiving, and casting done by drawing strands at
**20% density** into the shadow map. **[COC23]**

Transparency is **Multi-Layer Alpha Blending, 8 layers**, sorted front-to-back
with a 64-bit `InterlockedMax`, falling back to 32-bit with degraded correctness
where 64-bit atomics are unavailable. **[COC23]**

Costs, PS5, 1920p checkerboard, 23 115 strands **[COC23]**:

| | cost |
|---|---|
| strand setup | 0.478 ms |
| software rasterisation | 2.890 ms |
| hardware rasterisation | 0.285 ms |
| lighting, no shadow map | 1.32 ms |
| lighting, with shadow map | 2.25 ms |
| guide-hair optimisation | 0.12 ms (≈1/10) |

**What to take.** Not the hair — this project has no hair and a tactical camera
looking down at a board never will. Take two structural facts.

First, **the software rasteriser exists because MLAB needs per-pixel ordered
insertion**, and it costs 10× the hardware path. Order-independent transparency
is not a shader you switch on. `source2_rendering.md` §12.4 lists "a sorted
transparent pass" as a missing prerequisite for glass; for our content —
architectural panes, no overlapping stacks of translucent surfaces — a
**back-to-front sort of a handful of draws** is correct and MLAB is enormous
overkill. That is worth knowing before someone reaches for OIT.

Second, the **guide-hair optimisation at ~1/10 the cost**: light a sparse subset
and interpolate. The same idea is available anywhere lighting is smoother than
geometry.

---

## 8. The colour pipeline — small, cheap, and directly actionable

RE:2023's HDR grading rework is the most immediately copyable thing in this
document. **[COC23]**

The chain: **BT.709 linear → AP1 (via OpenColorIO) → ACEScc/ACEScct grading LUT
→ RRT + ODT → display (sRGB or HDR10)**. Grading is authored in **DaVinci
Resolve** against a live 1080p AP1 feed off a capture card, so artists grade the
running game.

The performance move is the point: **RRT + ODT cost 2.2 ms at 1080p on PS4
evaluated analytically, and 0.4 ms when baked at runtime into a 64³
R10G10B10A2Unorm 3D LUT.** The gamut conversion (0.2 ms) is folded into the
exposure/grading pass. **[COC23]**

UI is composited **after** RRT/ODT into an R8G8B8A8_sRGB working buffer, with a
compute shader blending opaque and alpha UI against the scene through conversion
matrices. **[COC23]** Doing it the other way round — UI through the tonemapper —
is the standard mistake and it makes menus shift colour with scene exposure.

**Directly applicable.** `tonemap.fs.glsl` currently applies Hable and writes 8
bits, with a fixed exposure uniform and no dither (§10 of the Source 2 doc lists
both). Adding a **64³ 3D LUT sampled after the tone curve** is a small change
that costs ~nothing, gives a real grading control surface, and is exactly what
Capcom ship. It also composes with the dither item rather than competing with it.

---

## 9. GPU-driven rendering, bindless, visibility buffer

Recorded for completeness. The original note here read "almost none of it is
reachable from GL 3.3" — **that is no longer true, and was already untrue when
written**: `CMakeLists.txt` forces `OPENGL_VERSION "4.3"` for cubemap arrays, so
compute shaders, SSBOs and `glMultiDrawElementsIndirect` have been core in this
context all along. §9.1 works through what that actually buys and what it costs.
The remaining barriers are raylib's API surface and the reason to want any of it
at this scale, not the GL version.

The first of those is now dealt with. `render/gpu/GL.hpp` declares the entry
points rlgl does not wrap — `glMemoryBarrier` above all, without which a compute
write races the draw that reads it — and `render/gpu/ComputeShader.hpp` puts a
dispatch and its barrier in one scope so they cannot drift apart.
`render/gpu/ComputeSelfTest.hpp` proves the chain end to end
(`xcom --compute-selftest <report>`). **Compute is available and verified; what
is missing is a pass that wants it.**

- **Intermediate draw commands**: per-thread contexts build platform-neutral
  commands with 96-bit priority-encoded headers, sorted once per frame, then
  translated to native API calls. Commands are reused across frames when state
  is unchanged. **[COC22]**
- **Two-stage culling**: a PreZ pass builds a Hi-Z, then a compute shader tests
  AABBs against it and decrements `InstanceCount` in the indirect argument
  buffer — no frame latency. **[COC22]** RE:2/DMC5 do the occlusion test against
  a **256×128 MSAA 4× depth buffer** — note how tiny that is — and cut a pass
  from ~2000 µs to ~1000 µs on an R9 Fury X. **[CEDEC18]**
- **Background meshes auto-split into 256-triangle batches** with per-batch AABBs
  for finer culling, then re-merged after the test. **[CEDEC18]**
- **Partial Z-prepass** — applied only to geometry near the camera, with
  alpha-tested geometry forced through it. **[CEDEC18]**
- **Bindless materials** cut Xbox One command-list time 10.3 ms → 8.2 ms, but
  *cost* GPU time until explicit scalarisation brought a 12-texture shader from
  124 VGPRs back to 72 (against 68 non-bindless). **[COC23]** The lesson is that
  bindless is a CPU win paid for in occupancy unless you scalarise.
- **Mesh shaders / meshlets** at 64–128 triangles, enabling >40% vertex buffer
  compression and 128-triangle culling granularity against the previous 768.
  **[COC23]**
- **Visibility buffer with deferred texturing** is the stated direction: store
  primitive and instance IDs, reconstruct vertex data from bindless resources,
  merge dissimilar meshes and materials into a single draw. **[COC23]**
- **VRS was tried and shelved** — Tier 1 at 2×2 makes polygon edges visibly
  low-resolution; a software VRS via MSAA and material-ID distribution looked
  better but the per-attachment manual resolve cost more than it saved.
  **[COC23]**

The cheapest transferable item is the **partial Z-prepass**: we already run a
depth+normal prepass for SSAO, and restricting a *shading* early-Z benefit to
near geometry is a free observation. The one with real upside is instancing,
below.

### 9.1 How instances come to exist — and what adopting it would take

§9 records how RE ENGINE *culls* instances and never says how instances come to
exist. That turns out to be the more interesting half, because the answer is the
exact inverse of Source 2's, and because it is the part within reach.

#### What Capcom actually do

**Instancing is discovered at runtime, not authored.** `同一描画可能インスタンス
をハッシュマップ管理` — "identical drawable instances managed in a hash map."
Objects hashing to the same draw state are merged into one instanced submission,
every frame. There is no instanced-mesh component, no foliage actor, no
compile-time bake. You place objects; the renderer notices they match. **[COC22]**

**Per-instance data lives in structured buffers**, moved off constant buffers
specifically to allow the per-instance payload to grow: **[COC22]**

```
World Matrix
Previous World Matrix      <- per-instance motion vectors
Joint Offset
Previous Joint Offset
Shader Parameters
```

**Indexing is two-part** — `メッシュ固有オフセットと SV_InstanceID で引く`, a
mesh-specific offset plus `SV_InstanceID` through a lookup table into the
structured buffer. That indirection is what lets one instanced draw cover
instances whose data is not contiguous. **[COC22]**

**Culling is a decrement, not a removal.** `Indirect バッファの InstanceCount を
CS で制御`, and `インスタンス数 0 であれば描画されない`. A fully culled batch
keeps its draw command in the buffer and draws zero instances, which is why no
readback and no frame of latency are needed. **[COC22]**

**Bindless closes the loop**: a `BindlessMaterialHandle` in the instancing
management buffer lets instances with *different materials* share one draw —
exactly the constraint that normally breaks automatic batching. **[COC23]**

> Same caveat as the rest of this document: machine extraction and translation of
> Japanese decks. The structure and technique names are reliable; check the slide
> before depending on wording.

#### Against Source 2, which solves the same problem backwards

See [`source2_rendering.md`](../valve/source2_rendering.md) for the Source 2 side.

| | Source 2 | RE ENGINE |
|---|---|---|
| when instancing is decided | map compile | every frame |
| authored as | `prop_static` flags → aggregate / clutter tiers | nothing; objects are just objects |
| per-instance data | baked vertex streams, or `m_fragmentTransforms` | structured buffer, offset + `SV_InstanceID` |
| culling unit | fragment bounds, PVS cluster, node tree | AABB vs Hi-Z, decrement `InstanceCount` |
| mixed materials in one draw | no | yes, via bindless handle |
| cost of moving one rock | invalidates the bake | free |

Source 2 pushes the work to the compiler and ships a format that already knows
the answer. RE ENGINE pushes it to the frame and rediscovers it constantly.
**For a destructible board the RE side is the right one** — we have no compile
step to bake into, and geometry moves. **[inferred]**

#### What we would need

Today [`PropSet`](../../../src/game/render/scene/PropSet.cpp) draws one instance per call:
`instance.model->draw(instance.transform, material)`. Every crate is its own draw.
That is fine at current prop counts and stops being fine the moment grass,
brushes and rocks are scattered.

The pieces, in dependency order:

| tier | what | needs | rlgl support |
|---|---|---|---|
| 1 | hash-map batching + instanced draw | GL 3.3 | `DrawMeshInstanced` |
| 2 | per-instance payload in an SSBO | GL 4.3 | `rlLoadShaderBuffer` |
| 3 | compute culling → `InstanceCount` | GL 4.3 | **none** |

**Tier 1 — the batching itself.** Pure CPU work and the whole point of the RE
design. Hash `(mesh, material, render state)`, bucket the transforms, one
instanced draw per bucket. raylib ships `DrawMeshInstanced`, which binds the
matrix array to `SHADER_LOC_MATRIX_MODEL` as four `vec4` attributes at divisor 1.

One caution: it calls `rlLoadVertexBuffer` … `rlUnloadVertexBuffer` **every
call**, so each batch creates and destroys a VBO per frame. Acceptable to prove
the idea; a persistent per-bucket buffer is the fix, and that means our own draw
rather than raylib's.

**Tier 2 — the payload.** Once instances want more than a matrix — per-instance
tint, a lightmap UV offset, a wind phase — the attribute path runs out and the
data belongs in an SSBO indexed the way Capcom index it: a per-batch uniform
offset plus the instance id.

Simpler for us than for them, and worth being explicit about because the obvious
worry does not apply. Capcom need the offset because one indirect submission
covers instances scattered through a shared buffer. With one `DrawMeshInstanced`
per bucket, **each bucket is its own draw starting at instance 0**, so
`gl_InstanceID` indexes that bucket's data directly and no offset is needed at
all. See tier 3 for where that stops being true.

**Tier 3 — the GPU culling, and where this gets expensive.** Compute
(4.3), `glMemoryBarrier` (4.2), atomics (4.2) and `glMultiDrawElementsIndirect`
(4.3) are all core in our context. Three things are not:

- **`gl_InstanceID` does not include `baseInstance`**, unlike D3D's
  `SV_InstanceID` — so the moment several buckets share one multi-draw, the
  shader can no longer tell which bucket it is in. `gl_BaseInstanceARB` needs
  `ARB_shader_draw_parameters`, core only at 4.6. Below that the workaround is a
  divisor-1 vertex attribute holding the index, because attribute *fetches* are
  offset by `baseInstance` even though `gl_InstanceID` is not. This is a tier 3
  problem only; tiers 1 and 2 never meet it.

- `glMultiDrawElementsIndirectCount` is **4.6**. Without it every batch's command
  stays in the buffer and is dispatched with `InstanceCount` possibly zero — which
  is precisely what Capcom describe, so this costs nothing but is worth knowing.
- **rlgl wraps none of it.** No indirect draw, no memory barrier. Tier 3 means
  calling GL directly, and `CMakeLists.txt` deliberately fences the one existing
  bypass to `ReflectionProbeSet.cpp` with "the day that stops being true is the
  day this renderer owns a GL backend it did not mean to own." Tier 3 is that day.

#### The honest recommendation

**Tiers 1 and 2 are worth building when scatter lands; tier 3 is not.** **[inferred]**

Tier 1 collapses a scatter field to one draw per mesh-material pair and needs no
GL past what we already have. Tier 2 is a small extension of it. Both stay inside
rlgl.

Tier 3 buys per-instance culling of batches the CPU has already decided are
visible — and on a fixed-camera 24×24 board with a known view volume, CPU-side
frustum culling per bucket gets most of that for none of the architectural cost.
Capcom need GPU culling because they have open worlds and a camera that goes
anywhere. Revisit only if a scatter field is large enough that per-bucket CPU
culling shows up in a profile.

#### 9.1.1 The actual shape of the work, read off this codebase

Deferred until grass lands — see §11.7. Written down now because the expensive
part was *finding* it, and that cost should not be paid twice. The blocker is
not the C++, which is an afternoon. It is the shaders.

**Every vertex shader takes the model matrix as a UNIFORM.** `pbr.vs.glsl`,
`depth_only.vs.glsl` and `prepass.vs.glsl` all read `uniform mat4 mvp` (already
premultiplied by the model matrix), plus `matModel` and `matNormal`. That is
raylib's non-instanced path. `DrawMeshInstanced` instead binds the transform
array to `SHADER_LOC_MATRIX_MODEL` as four divisor-1 **attributes**. The two are
incompatible, so instancing means a variant of each shader:

```glsl
in mat4 instanceTransform;     /* 4 attribute slots, divisor 1 */
uniform mat4 mvp;              /* view * projection ONLY, no longer per-object */
...
gl_Position = mvp * instanceTransform * vec4(vertexPosition, 1.0);
```

plus one wiring line per shader, because raylib overloads that slot rather than
adding one:

```cpp
shader.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocationAttrib(shader, "instanceTransform");
```

**`matNormal` is the sharp edge.** raylib builds the inverse transpose on the
CPU per draw, and an instanced draw has no per-object CPU step to build it in.
Either pay `transpose(inverse(mat3(instanceTransform)))` per vertex, or observe
that scatter is rotation + uniform scale + translation and use
`normalize(mat3(instanceTransform) * n)`, which is exact for that case and
nearly free. Taking the cheap path buys a constraint that has to be written
down: **no non-uniform scale on instanced props**, or normals go quietly wrong
and it reads as a lighting bug.

**Three passes, not one.** Props go through `drawLit` *and* through `draw` for
the shadow map and the prepass. All three shaders need the variant. Miss one and
the scatter casts no shadow, or drops out of SSAO — both of which look like
content problems rather than shader problems.

**The batch key is `(Mesh.vaoId, MaterialLibrary::Handle)`.** `ModelAsset::drawLit`
sets `setMaterialFactors`, `setMaterialOptions` and `setMaterialTransmission`
before every `DrawMesh`, so bucketing by material amortises those uniform
uploads as well as the draw.

**One deliberate deviation from Capcom.** RE ENGINE rehashes every frame because
its objects move. `PropSet` is render-only and its placements come from a
manifest, so the buckets can be built **once at load** with a rebuild hook. Same
structure, none of the per-frame cost. Revisit if props ever participate in
destruction.

Ordered, with the unverifiable steps first because the later ones cannot be
judged without them:

| # | step | effort |
|---|---|---|
| 0 | `assets/models/props.txt` with enough scatter to measure — it does not exist, so `PropSet` currently loads nothing | ½ day, mostly authoring |
| 1 | instanced variants of the three vertex shaders + attrib wiring, behind a switch so one-off props and soldiers keep the current path | **1–2 days, where the bugs are** |
| 2 | the bucket map in `PropSet` — invert the loop, one `DrawMeshInstanced` per bucket | ½ day |
| 3 | measure with `gl::TimerQuery`: draw calls and GPU ms, before and after. If it does not move, stop | 1 hour |
| 4 | tier 2 payload SSBO indexed by `gl_InstanceID`, when tint or wind needs it | ½ day |
| 5 | persistent per-bucket buffers, replacing `DrawMeshInstanced`'s per-call VBO churn | 1 day |

Note what is absent: **no compute anywhere in steps 0–5.** The GL escape hatch in
`render/gpu/GL.hpp` is not needed until tier 3, which §9.1 argues against. The
two pieces of work are independent.

---

## 10. Volumetrics — what Capcom have not published

**There is no Capcom deck on RE ENGINE's volumetric fog.** Ten years of talks
cover shadows, probes, SDFs, hair, ray tracing, colour and culling, and skip the
participating medium entirely. What exists is tool-level description:

- Fog volumes are **box-bounded**, can be given **wind-driven flow and
  gradients**, and can be made to **conform to terrain**. **[tool]**
- Cloud shadows are **not** cast for real. A `ShadowProjectionTexture` projects
  cloud patterns to attenuate sunlight — explicitly because real cloud shadowing
  "would consume excessive processing power". **[tool]**
- Lights carry a **Volumetric Scattering Intensity** parameter, per light.
  **[tool]**
- Rain has a full lifecycle — falling, wetting, puddle ripples, drying — gated
  indoors by a **`DepthOcclusion`** shielding system first shipped in RE7.
  **[tool]**

Per-light volumetric scattering intensity strongly implies a froxel injection
pass, since that is the parameter such a pass needs and no other architecture
wants it. **[inferred]** But it is an inference, and no format, resolution or
cost is public.

**Conclusion for this project: nothing changes.**
[`rdr2_atmospherics.md`](rdr2_atmospherics.md) remains the blueprint for the
froxel volume that `source2_rendering.md` §13 says we need. RE ENGINE has nothing
to add here, and the honest statement is that Capcom chose not to talk about it.

The one genuinely transferable idea is the **cloud-shadow cheat**: a projected
attenuation texture instead of a real occlusion query, chosen deliberately over
the correct method. Our equivalent question — whether smoke should darken the
ground it sits over via the froxel volume or via a projected mask — has the same
answer for the same reason. **[inferred]**

---

## 11. What this project should take, ranked

Ordered by change-on-screen per unit of work, against the current renderer.

> **The ranking below predates the remake research in §2–§4, and that research
> moves it.** Kept in place rather than renumbered, because the *arguments* for
> each item are unchanged and worth reading; what changed is their order.
>
> Three findings do the moving. **RE4R is the best-looking title in the series
> and it uses less ray tracing than its predecessors, no SDF, and probe-plus-
> cubemap indirect** (§4.4) — so the thing separating our frame from theirs is
> not a technique we cannot reach. **The SDF names no shipping title and is
> framed around open worlds with a moving sun** (§5), which is Dragon's Dogma 2's
> problem and not ours; our world is 24×24 with one sun the player rarely moves.
> And **nothing in this renderer has more than one light at all** (§0), which no
> amount of shadow or SDF work addresses.
>
> Revised order, on those grounds:
>
> 1. **Local lights.** Not in this list at all, because RE ENGINE has had them
>    since RE7 and it never occurred to the original pass to rank something so
>    basic. One directional sun is the ceiling on every interior in the game.
> 2. **§11.2, four-direction probes** — promoted from second to first among the
>    listed items, on RE4R's evidence that this class of indirect is enough.
> 3. **§11.3, the grading LUT** — cheap, shipped, artist-facing.
> 4. **§11.1, the SDF** — still the most interesting item and still worth
>    building, but demoted: its headline uses are AO (we have SSAO) and
>    long-range shadows (we have a bounded board). Its best argument here is
>    §1's — that geometry is the right thing to cache — not Capcom's range.
>
> The original text of each item follows unchanged.

### 11.1 Build the SDF. It is the largest single win. — §5

One R8 3D texture over the lattice, ~324 KB, maintained by a local distance
transform after each destruction event. Then, in order of payoff:

1. **SDF ambient occlusion** alongside the existing SSAO — sees offscreen
   geometry, which SSAO structurally cannot. Half resolution, blurred, exactly as
   Capcom do it. This is the cheapest step and it is independently useful.
2. **SDF soft shadows** by sphere tracing — a third answer to §9 that has no
   projection, no crawl, no bias, no cascade, and no re-render on destruction.
   Measure it before committing; fragment-shader sphere tracing on GL 3.3 is the
   risk.
3. **Exact leak rejection for the probe grid**, which §11 of the Source 2
   document already plans to do by lattice query — the SDF is the same
   information in a form the shader can sample and interpolate.

Note how this composes with the probe-grid decision rather than competing: the
grid supplies indirect light, the SDF supplies visibility. §1's "cache repair"
argument applies to the SDF, not to the shelved lightmap — the SDF caches
*geometry*, which is small, cheap to update locally, and does not go subtly wrong
the way a stale lighting cache does.

### 11.2 Four-direction probe storage — §4.1

R11G11B10Float × 4 = 16 bytes per probe, 83 KB for the whole lattice. A modest
step up from the two-lobe hemisphere ambient and a much smaller one than SH2,
with a shipped precedent and a known sampling cost (1.5 ms at 1080p, though that
is over a tetrahedral network we do not need).

**The remake research strengthens this item more than any other.** When this was
written, §4.3 claimed the probe system had been retired in favour of ray tracing,
which made four-direction storage look like a stepping stone toward something
better. It had not been retired: it is what Resident Evil 4 shipped its indirect
lighting on, in 2023, with ray tracing switched off for GI entirely (§4.4). The
structure being copied here is not a legacy technique on its way out — it is the
one carrying the best-looking game in the series.

And the expensive half is still free for us. Capcom's 80-second bake, their
tetrahedralisation, their BSP and their 32³ accelerator grid all exist because
probes sit at irregular positions in a world made of triangles. Ours sit one per
lattice cell, so the lookup is an array index. **We would be adopting the part
that works and skipping the part that cost them the effort**, which is the third
time that argument has come up in this document and the most valuable instance
of it.

### 11.3 A 64³ grading LUT after the tone curve — §8

Cheap, shipped, artist-facing, and it fits beside the dither and exposure items
already on the list. 2.2 ms → 0.4 ms is Capcom's measurement of doing the
expensive part once into a LUT rather than per pixel.

### 11.4 Relit cubemaps, when cubemaps happen — §4.2

`luma(ProbeDiffuse) * luma(cube(r,mip)) / luma(cube(n,lowestMip))`. Four lines,
and it is the difference between a cubemap that is only correct under the
lighting it was baked in and one that tracks the probe grid. Given the probe grid
is already the plan, this is nearly free.

### 11.5 Keep two shadow maps, not one composited map — §3.1

Not new work; a confirmation. `source2_rendering.md` §9.4's `min(A, B)` avoids
the 0.1 ms-per-map copy that dominates Capcom's shadow cache. Do not consolidate.

### 11.6 Do not build OIT — §7

Sort a handful of transparent draws back to front. MLAB and a software
rasteriser are what a game with 23 000 hair strands needs. Note also that RE7
spent as much on the transparent pass as on all direct lighting, in a game with
very little transparency — budget accordingly.

### 11.7 Hash-map auto-instancing, when grass lands — §9.1

**Deferred by decision, not by doubt.** Ranked last because nothing on screen
needs it yet: `PropSet` currently loads no props at all, so there is nothing to
batch. The trigger is grass — the moment grass, brushes or rocks are scattered,
the one-draw-per-instance loop becomes the bottleneck, and RE ENGINE's answer —
hash `(mesh, material, state)`, bucket, one instanced draw each — is both the
cheapest fix and the one that needs no GL we do not already have. Tiers 1 and 2
of §9.1; not tier 3.

**§9.1.1 has the worked plan** — the three shaders that block it, the `matNormal`
constraint, the batch key, and six ordered steps with effort. Read that before
starting rather than re-deriving it; finding it was most of the cost.

Note this is the *only* item in this document where RE ENGINE's approach beats
Source 2's for our case, and it wins for a specific reason: we have no map
compile to bake placement into, and destruction moves geometry.

### Explicitly not worth pursuing here

| | why |
|---|---|
| Bindless, mesh shaders | Not reachable *at any GL version we would ship*: bindless textures are an ARB extension that never went core, mesh shaders never came to GL at all. Not a 3.3-vs-4.3 question. |
| Visibility buffer | Reachable at 4.3 in principle. The draw-call and overdraw pressure that motivates it does not exist on a 24×24 board. |
| Ray tracing / path tracing of any kind | Not reachable. Read §6 for the ordering decisions only. |
| Baked, quadtree-compressed shadow maps | Solves a large-static-world problem we do not have. |
| Shell fur | No fur. |
| Ray-traced lens flare | Delightful, and it needs scene bloom (which we lack) to mean anything. Revisit after §10.5 of the Source 2 document. |
| Variable rate shading | Capcom tried it and shelved it. |

---

## 12. Reference table — RE ENGINE costs in one place

Useful as calibration for what shipped techniques actually cost. Platform varies;
read the tag.

| system | cost | platform | source |
|---|---|---|---|
| irradiance volume sampling | 1.5 ms @1080p | PS4-era | **[CEDEC16]** |
| probe network build | 80 s | i7-4790, offline | **[CEDEC16]** |
| shadow map copy | 0.1 ms each, 3.2 ms × 32 | PS4-era | **[CEDEC16]** |
| whole RE7 frame | ~15–16 ms @1080p60 | PS4-era | **[CEDEC16]** |
| baked shadow pass | 36 → 20 ms | GTX 1070 Ti | **[CEDEC18]** |
| GPU occlusion cull | ~2000 → ~1000 µs | R9 Fury X | **[CEDEC18]** |
| SDF shadows | 2.5 ms open / <0.5 ms occluded | PS4 | **[COC23]** |
| SDF AO (+SSAO) | ~1 ms @540p | PS4 | **[COC23]** |
| SDF clipmap update | >20 ms full, <1 ms differential | PS4 | **[COC23]** |
| RRT+ODT analytic → LUT | 2.2 → 0.4 ms @1080p | PS4 | **[COC23]** |
| shell fur, 16 layers, MDI | 1.0 ms @1080p | PS4 | **[COC23]** |
| RT lens flare ghosts | 4.3 → 0.4 ms @960×540 | PS5 | **[COC23]** |
| ray-traced GI total (v3) | ~4.7 ms | PS5 | **[COC23]** |
| strand hair raster + lighting | ~3.7 + 2.25 ms | PS5 | **[COC23]** |
| full path tracing | 8.78 ms @1080p | RTX 4070 Ti | **[GDC26]** |

---

## Sources

Capcom R&D publish their decks at **[docswell.com/user/CAPCOM_RandD](https://www.docswell.com/user/CAPCOM_RandD)**
— roughly 68 decks across three pages, most CAPCOM Open Conference talks in both
Japanese and English editions. The conference sites
([RE:2019](http://www.capcom.co.jp/RE2019/), [RE:2022](https://www.capcom.co.jp/RE2022/),
[RE:2023](https://www.capcom-games.com/coc/2023/en/)) index the talks with video,
but the RE:2023 site rejects automated fetches — go through docswell.

Read for this document:

- **[CEDEC16]** [「バイオハザード7」を実現するレンダリング技術](https://www.docswell.com/s/CAPCOM_RandD/ZJLVPJ-cedec2016) — the architecture talk. G-Buffer, shadow cache, irradiance volumes, relit cubemaps, frame budget.
- **[CEDEC18]** [最新タイトルのグラフィックス最適化事例](https://www.docswell.com/s/CAPCOM_RandD/ZXYVJG-cedec2018) — RE:2 and DMC5. Culling, baked shadow compression, partial Z-prepass, SIMD.
- **[COC22]** [GPU駆動レンダリングへの取り組み](https://www.docswell.com/s/CAPCOM_RandD/Z2GP4Z-2022-07-15-121419) — intermediate commands, Hi-Z culling, indirect draw, and the hash-map automatic instancing of §9.1.
- **[COC23]** [Is Rendering Still Evolving?](https://www.docswell.com/s/CAPCOM_RandD/524Y6M-RE2023) ([JP](https://www.docswell.com/s/CAPCOM_RandD/ZRXJP2-RE2023)) — bindless, scalarisation, visibility buffer, mesh shaders, VRS, and why RT replaced probe GI.
- **[COC23]** [Advances in Ray Tracing](https://www.docswell.com/s/CAPCOM_RandD/K24Y66-RE2023) ([JP](https://www.docswell.com/s/CAPCOM_RandD/5RXJP4-RE2023)) — the v3 GI denoiser, per-title feature matrix, PS5 costs.
- **[COC23]** [New Rendering Features Rundown (HDR Grading, Shell Fur, Signed Distance Field)](https://www.docswell.com/s/CAPCOM_RandD/KENVMJ-RE2023) — §5 and §8 come almost entirely from here.
- **[COC23]** [Resident Evil 4 Hair Discussion](https://www.docswell.com/s/CAPCOM_RandD/KVVYN3-RE2023) — strands, MLAB, dual scattering.
- **[COC23]** [RayTracingLensFlare](https://www.docswell.com/s/CAPCOM_RandD/ZNR832-RE2023) — lens-element ray tracing, PS5 costs.
- **[COC23]** [RE ENGINE's Past and Future](https://www.docswell.com/s/CAPCOM_RandD/ZGXVYD-RE2023) and [RE ENGINE Philosophy](https://www.docswell.com/s/CAPCOM_RandD/5RXJ42-RE2023) — engine-level context.
- **[GDC26]** [Implementing Real-Time Path Tracing in RE ENGINE for 'Resident Evil Requiem' and 'PRAGMATA'](https://www.docswell.com/s/CAPCOM_RandD/5DM2NL-gdc2026-implementing-real-time-path-tracing-in-re-engine) — ReSTIR GI, streaming RIS, SER, DLSS RR, BVH management, RTX costs.
- **[NV]** [Q&A: How Capcom Brought Path Tracing to RE ENGINE](https://developer.nvidia.com/blog/qa-how-capcom-brought-path-tracing-to-re-engine-across-pragmata-and-resident-evil-requiem/) — corroborates **[GDC26]**, and states the direct-lighting transition plainly.

- **[GDC19]** [Optimization Techniques: RE2 / DMC5](https://gpuopen.com/download/gdc-2019-s4-optimization-techniques-re2-dmc5.pdf) — official English, downloadable, and not on docswell. Confirms the partial Z-prepass, and states the **Depth Bounds Test is used "in RE ENGINE … for decals and light shafts"** to skip pixels fully occluded by a wall.
- **[REAC25]** [RE ENGINE Meshlet Rendering Pipeline](https://enginearchitecture.org/downloads/REAC_2025_Capcom.pdf) — official English, 71 slides. Post-remake (DD2 / MH Wilds era); visibility-buffer deferred. Read alongside §9.

### How to read a docswell deck

Recorded because finding it out cost real time, and because every figure in this
document depends on it. **Docswell embeds each deck's full slide text *and the
speaker notes* in the page HTML**, even though downloads are disabled
(ダウンロード不可). `curl` on the slide URL followed by tag-stripping yields the
verbatim text — and **the speaker notes are where nearly all the numbers and all
the candid remarks live**, not the slide bodies. A vision transcription of
*Is Rendering Still Evolving?* was cross-checked against the raw text and
matched exactly.

Correction to an earlier note in this file: **the RE:2023 site does not reject
automated fetches — it rejects the default agent string.** `curl -A "Mozilla/5.0
… Chrome/120"` on `https://www.capcom-games.com/coc/2023/en/session/` returns the
full English session index. Its slide links point back to the same docswell
decks, so there is no extra *technical* material there, but the session abstracts
and speaker names are official English.

**The highest-value unmined source**, if this document is ever extended: RE:2019
session M4-1, **「レンダリング技術の進化」 (Evolution of Rendering Technology)**,
Hitoshi Mishima, 29:43 — [youtube.com/watch?v=Gz0k91MVjys](https://www.youtube.com/watch?v=Gz0k91MVjys).
Video only, no slides, no captions. It is the same engineer who gave the RE7 and
RE:2023 rendering talks, speaking in 2019 on how rendering evolved from RE7
through RE2R and DMC5 — which is exactly the gap §2.1 records as unpublished. It
would need someone to watch and transcribe it.

**[tool]** — descriptive, not Capcom's engineering word:

- [Ideas and Ingenuity: Behind Capcom's In-House RE ENGINE Tools For Dragon's Dogma 2](https://blogs.autodesk.com/media-and-entertainment/2025/04/10/ideas-and-ingenuity-behind-capcoms-in-house-re-engine-tools-for-dragons-dogma-2/) — atmosphere, volumetric fog boxes, the cloud-shadow projection cheat, rain lifecycle, wind.
- [RELit for RE Engine games](https://framedsc.com/GeneralGuides/relit.htm) — the light parameter set as exposed by modding tools: bounce intensity, min roughness, AO efficiency, illuminance threshold, per-light volumetric scattering intensity, and the shadow bias family.

**Not found, and worth stating.** No Capcom publication on RE ENGINE's volumetric
fog implementation; no published G-Buffer layout later than RE7; no figure for
SDF voxel resolution per mesh. Monster Hunter: World's well-known CEDEC 2017–18
rendering talks are in the same docswell catalogue but are **MT Framework, not
RE ENGINE** — do not cite them here.
