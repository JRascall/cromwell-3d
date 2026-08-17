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
| **[CEDEC17]** | CEDEC 2017 — Monster Hunter: World rendering and GPU optimisation. **MT Framework, not RE ENGINE** (see §2.1's trap), but the *probe* work in §4.1.1 is by the same R&D division and continues RE7's system directly. |
| **[COC22]** | *Efforts toward GPU-driven rendering.* **Dating disputed — see below.** |
| **[REAC25]** | REAC 2025 — *RE ENGINE Meshlet Rendering Pipeline*. Official English, DD2 / MH Wilds generation. |
| **[COC23]** | Open Conference RE:2023 — *Is Rendering Still Evolving?*, *Advances in Ray Tracing*, *New Rendering Features Rundown*, *RE4 Hair*, *RayTracingLensFlare*. |
| **[GDC26]** | GDC 2026 — *Implementing Real-Time Path Tracing in RE ENGINE for Resident Evil Requiem and PRAGMATA*. |
| **[NV]** | NVIDIA developer-blog Q&A with Capcom on the same work. |
| **[GDC19]** | GDC 2019 — *Optimization Techniques: RE2 / DMC5*. 110 slides, official English, distributed via GPUOpen. |
| **[tool]** | Observed from shipped tooling — Autodesk's Dragon's Dogma 2 tools piece, the RELit modding docs, engine enums and material files read by community tooling. Descriptive, not Capcom's engineering word, and the material-file archaeology in particular is unverified. |
| **[3P]** | Third-party technical analysis — Digital Foundry, ComputerBase, TechPowerUp — and shipped graphics menus. Observation of the output, never a claim about the implementation. |
| **[CAPTURE]** | **Our own RenderDoc frame captures of the shipped game.** A different evidence class from everything else here: not what Capcom said, but what the binary does. It can therefore *contradict* a deck, and where it does, it wins on questions of fact about that build. It cannot speak to intent, to other titles, or to anything outside the captured frame. See §2.5. |
| **[inferred]** | Our reading, not Capcom's. |

> **[COC22] IS MISDATED BY SIX YEARS, AND THIS IS NOW SETTLED.** The deck this
> document has been citing as "Open Conference RE:2022" is
> **Game Creators Conference 2016**. Three independent confirmations: its own
> slide 2 reads 「**GCC 2016はオープンなイベントです**」; its description says
> 「**Game Creators Conference 2016の講演で使用したスライドです**」; and the
> CEDEC 2016 RE7 deck cites it as a *companion talk given that same year*
> (「多くの設計概念は、GCC資料をご確認下さい」). The 2022-07-15 in the docswell
> slug is the **upload** date, and the sidebar linking to RE:2019/2022/2023 is
> channel boilerplate.
>
> **This matters most for §9.1.** Everything there described as "what Capcom
> currently do" about GPU-driven rendering and hash-map auto-instancing is
> **RE7-era, 2016, three years before RE2R** — contemporaneous with RE7's
> development, not a recent statement of practice. The technique is not thereby
> wrong, but "RE ENGINE does this today" is not what the source supports. The
> tag is kept as `[COC22]` only to avoid breaking every cross-reference in this
> file; read it as **GCC 2016** everywhere.

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

### Light culling is CLUSTERED, and the dimensions are published **[CEDEC16]**

Missing from earlier revisions of this document, and it is the most directly
applicable thing in the whole RE7 talk for a renderer planning Forward+.

> 「**32ｘ32ピクセルを1タイル、深度方向を16分割して使用**」 — 32×32 pixel tiles,
> the depth split into 16 slices. A tile's min and max depth become an AABB along
> the frustum, tested against each light; spot lights fall out as intersections
> against several planes. Run on async compute where possible.

The cluster structure, exactly:

| | |
|---|---|
| cluster grid | **60 × 34 × 16 at 1080p**, 32 bit per cluster |
| cluster word | 24 bit light-list offset + 8 bit light-list mask |
| light list | up to 8 consecutive 32-bit words, one bit per light index |
| reconstruction | mask + bit → light index, per **[Humus 2015]** |
| ceiling | **512 lights** |
| depth slicing | logarithmic, α = 16 |
| light types | directional, point + IES, spot + IES |

Two-stage: GPU occlusion culling decides whether a light draws shadows at all,
then clustered shading uses the depth buffer.

**Why this is worth having in front of us.** 60 × 34 × 16 is 32 640 clusters
carrying 32 bits each — 130 KB — to index 512 lights at 1080p. Our lattice is
24 × 24 × 9 = 5 184 cells. A *world-space* cluster grid over the lattice is
smaller than RE7's screen-space one by a factor of six, needs no depth-slice
maths and no per-frame rebuild as the camera moves, and is indexed by the same
arithmetic every other grid in this codebase already uses. **The hard part of
Forward+ is the cluster structure, and the lattice is one.** That is the fourth
time this document has reached that conclusion from a different direction.

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
depth, and no statement of the shading model or BRDF after CEDEC 2016 — and
neither does REAC 2025, which publishes only the *visibility* buffer's format
(`R32G32Uint`, or a 64-bit atomic; 1 bit signature / 24 bit instance / 7 bit
triangle). Nor is there any RE2R- or RE3R-specific rendering talk beyond
CEDEC 2018 and GDC 2019, **any RE4R lighting talk at all**, or any published
frame *breakdown* of an RE ENGINE game.

**One correction to an earlier revision of this line.** It used to say no
per-pass GPU figure exists for any remake. That was too strong: GDC 2019's
testing-environment slide pins the whole deck to **"RE2 (2/15 patch), 1080p,
mainly Radeon RX 480, partially R9 Fury X"**, which makes every number in it an
RE2 number — including a per-stage culling-and-GBuffer table and a whole-frame
figure. See §2.4. What remains absent is a *complete* breakdown: there is no
shadow, lighting, post, transparent or UI figure for RE2 anywhere.

**One gap that has since closed: the cascade count.** REAC 2025 states it
outright for the DD2 / MH Wilds generation — **"3 Cascade Shadow with
AsyncCompute"**, and elsewhere **"3 cascade shadow maps and 1 spotlight shadow
map"**. **[REAC25]** Still nothing for the remakes, but this is the only cascade
figure Capcom have ever published and it is worth having: three, async, plus one
spotlight. Note how modest that is next to the engine's `ShadowResolution` enum
topping out at 4096 and `SparseShadowTreeResolution` reaching 64K.

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

### 2.4 The two remakes, stage by stage

Assembled because they are the titles worth learning from, and written with each
claim marked by how well it is actually attested. **Nobody can give you an
RE ENGINE frame the way they can for Doom** — Capcom have never published a frame
breakdown of any title in any language. What follows is the most that the sources
support, and it stops where they stop.

**These are not one renderer.** RE2R (2019) and RE4R (2023) are four years and a
console generation apart. RE2R shipped with no ray tracing and no SDF machinery;
RE4R has RT reflections, SDF types in its runtime, sheen/velvet materials and the
normal-correction fix. Reading them as a single pipeline would flatten the most
informative part.

#### Resident Evil 2 (2019) — the purest rasterised RE ENGINE

**The attribution anchor that makes this section possible** **[GDC19]**: the
testing-environment slide reads *"RE2 (2/15 patch) • 1080p • Mainly Radeon RX480,
partially Radeon R9 Fury X"*. DMC5 appears in that deck only as a shipped title —
**no DMC5 measurement appears in it at all** — so every figure below is RE2.

**Frame time**: **15.84 ms → 12.09 ms** at 1080p on an RX 480, a stated *"24%
frame time saving"* across the optimisation work. (The deck contradicts itself by
0.04 ms — 12.13 elsewhere — unexplained.)

**The only published per-pass figures for any remake**, µs, RE2 / 1080p / RX 480
/ DX12 / Radeon GPU Profiler:

| configuration | culling | GBuffer | total |
|---|---|---|---|
| frustum culling | 115.5 | 2 295.4 | 2 410.9 |
| + occlusion culling | 618.3 | 1 976.9 | 2 595.2 |
| + auto split | 829.4 | 1 764.4 | 2 593.8 |
| + partial Z-prepass | 823.1 | 1 560.6 | 2 383.7 |
| + reduced resource barriers | 285.4 | 1 592.5 | 1 877.9 |

**Two things in that table cut against the tidy narrative, and they are the most
useful part of it.** Auto-split made the total *slightly worse* — the culling
cost ate the GBuffer saving. And **the barrier reduction is what actually paid**,
cutting culling by 65% while GBuffer rose. Capcom say so themselves: GPU
occlusion culling alone moved the frame 15.84 → 15.61 ms, *"At this point not
gain performance"*, and *"Not as much geometry culled as hoped"*. It only became
worthwhile once the barriers were fixed.

**Pass order — and this is as far as the sources go.** Culling → partial
Z-prepass → GBuffer (decals inside or adjacent) → … and then nothing. There is no
RE2R pass list, timeline or capture in any published deck; the RGP screenshots in
GDC 2019 carry their pass names only in the images. The engine's registered pass
*names*, from 2016, are `GBuffer, DeferredLighting, ShadowCast, PostProcess, Etc`.

**The culling chain**, in order **[GDC19]**, and it is the best-documented part of
RE2R:

1. A `VisibleBuffer` — a `ByteAddressBuffer`, one element per mesh, `0xffff`
   visible / `0x0000` not — which **doubles as the `ExecuteIndirect` CountBuffer**,
   so culling and dispatch share one structure.
2. **Frustum, in compute, one dispatch for the entire scene.**
3. **Occlusion, by rasterising the AABBs — one instanced draw for all of them** —
   into a **256×128 MSAA 4× depth buffer** built from *artist-authored simplified
   occluder geometry*, configured never to be read through an SRV.
4. The test is inverted and costs almost nothing: `[earlydepthstencil]`, and the
   pixel shader writes `0xffff` only if it survives. If EarlyZ kills the fragment,
   the compute stage's verdict stands. `WaveCompactValue` collapses writes to the
   same address within a wave.
5. **Sub-pixel occludees are handled by inflating the AABB's vertices in the
   vertex shader** — half a pixel outward from the box centre — because without
   conservative rasterisation a small projected box tests as nothing.
6. Large meshes are **auto-split into 256-triangle batches** with a per-batch
   AABB, because *"large meshes are always visible"*.
7. Instancing indices are compacted with a wave prefix count, then **adjacent
   indirect commands are merged**, because *"almost all draws fall below 768
   indices"* and many tiny batches perform badly.

**Partial Z-prepass**: *"Limiting Z-prepass to meshes close to the camera"*, and
**no distance threshold is published in either language**. Its summary notes it is
*"very effective indoors"* — the one place a technique is characterised in a way
that favours RE2 over DMC5.

> **A correction to a claim this document carried.** "Alpha-tested geometry forced
> through the Z-prepass" is stated of **RE7**, not RE2, and only in the Japanese
> deck — presented as the *prior* practice the partial Z-prepass departs from.
> Do not attribute it to RE2R.

**Depth Bounds Test** is used *"for decals and light shafts"*, killing pixel
shaders for decal volumes entirely hidden behind a wall. Savings are chart-only,
no data labels, in both decks.

**Two negatives worth as much as the positives.** RE2R on PC shipped with **no
async compute** — *"Used for Consoles • Implementation was incompatible for PC"* —
and no SM6.0, *"Not enough time to ensure stability"*. And DX12 beat DX11 by
2.15 ms but beat **DX11-with-AGS by only 0.06 ms**: nearly the entire win was
vendor intrinsics, not the API.

### 2.5 RE2R measured — six frame captures **[CAPTURE]**

Everything above is testimony. This section is measurement: six RenderDoc
captures of the shipped Steam build on DX12, taken 2026-08-13 across the Mizoil
gas station, the shop interior, a street vista, an alley, the RPD main hall and a
cutscene. **Where this contradicts a deck, it wins on questions of fact about
this build** — and it contradicts one.

Method note for anyone repeating it: the game must be launched *by Steam through*
`renderdoccmd`, via launch options, or Steam's DRM relaunch produces an unhooked
process and nothing captures. Confirm the hook by checking `renderdoc.dll` is
loaded in the game process, not by looking for the overlay.

**Caveat on resolution.** These frames render at **1129×635** and upscale to
1920×1080 — a ~59% resolution scale was set in the graphics options. Absolute
buffer sizes below scale with that; the *architecture* does not.

#### The G-Buffer, at last

**Identical in all six captures**, same resource ids, same formats — so this is
the layout, not a per-scene artefact:

| target | format |
|---|---|
| 3473 | `R10G10B10A2_UNORM` |
| 3474 | `R8G8B8A8_SRGB` |
| 3475 | `R10G10B10A2_UNORM` |
| 3479 | **`R16G16B16A16_SNORM`** |
| depth | `D32S8_TYPELESS` |

**This is not RE7's layout.** RE7 was four targets at 16 B/px —
`R11G11B10F` emissive, `R8G8B8A8_SRGB` basecolor, and two `R10G10B10A2`. RE2R
keeps three and replaces one with a **64-bit signed target**, taking the
G-Buffer to **20 B/px**. Capcom have published nothing on this since 2016.

A second, much smaller pass follows the geometry pass in every capture (19–81
draws against 656–1642) binding `R11G11B10_FLOAT` **plus** basecolor and one of
the `R10G10B10A2` targets, but *not* 3473 or 3479. Its position and shape match
RE7's "GBuffer + Decal" adjacency, and writing basecolor and normals while
leaving velocity alone is what a decal pass would do. **[inferred]** — the
channel assignments are not yet read off the shader.

Worth noting what the SNORM target probably buys. RE7 stored velocity in an
*unsigned* `R10G10B10A2` and had to encode it as `sqrt(abs(v))` with a separate
sign bit. A signed 16-bit-per-channel target needs none of that. **[inferred]**

#### Shadows — RE7's architecture, doubled, and observed running

Two `2048×2048` `R32_TYPELESS` depth arrays of **32 slices** exist in every
capture (3346 and 3356). **Only 3346 is ever bound as a render target. 3356 is
never drawn into.**

That is the shadow cache, caught in the act. RE7 described exactly this — a
`Texture2DArray` at "default 1024×1024×32", a static cache, and a copy between
them — and the engine's own `ShadowCastSegment` enum carries a `CacheCopy` value
between `StaticShadow` and `DynamicShadow`. **RE2R runs RE7's mechanism at twice
the resolution.**

Draws into the shadow array, which do *not* track visible light count:

| scene | shadow draws |
|---|---|
| gas station forecourt | 165 |
| street vista | 710 |
| alley, one visible streetlight | 750 |
| RPD main hall, a dozen sconces | 763 |
| shop interior, one zombie | 952 |

#### The Sparse Shadow Tree is not in RE2R

**No texture of any kind reaches 8192 in either dimension in any of the six
captures** — including the outdoor street vista with a long view down a city
block, which is precisely where a 16K–32K baked directional map would be used.
The largest textures in every frame are ordinary 4096² BC7 art.

§3.2 argued from Capcom's own DMC5-framed wording that the Sparse Shadow Tree
was probably not RE2R's. **That inference is now measurement.**

#### The shaders name everything — and this is the real haul

RE2R ships **SM6.0 DXIL with full reflection**, so the disassembly carries
Capcom's own resource and field names. The G-Buffer pixel shader's entry point is
literally `PS_GBuffer`.

**The G-Buffer, named by Capcom.** The deferred lighting shader binds it as:

| SRV name | our measured format |
|---|---|
| `BaseColorMetallicSRV` | `R8G8B8A8_SRGB` |
| `NormalXNormalYRoughnessMiscSRV` | `R10G10B10A2_UNORM` |
| `VelocityXVelocityYOcclusionSubSurfaceSRV` | `R16G16B16A16_SNORM` |

That maps onto RE7's published layout exactly, and **confirms the inference above**:
the new signed 16-bit target is velocity + occlusion + subsurface. RE7 packed
those into an *unsigned* `R10G10B10A2` and needed a `sqrt(abs(v))` encoding with a
sign bit; a signed target needs none of it. Also bound: `GIDSRV` (`Texture2D<int>`,
a material/GBuffer id) and `AmbientBRDF` — a split-sum environment BRDF LUT.

**Light culling is clustered, and still is.** `LightCullingVolumeSRV` is a
`Texture3D<int>` and `LightCullingListSRV` a `ByteAddressBuffer<int>` — a cluster
volume plus a light-index list, which is precisely RE7's published scheme
(§2, "Light culling is CLUSTERED"). `LightInfo` counts them separately:
`PunctualLightCount`, `AreaLightCount`, forward variants of each, and
`RT_PunctualLightCount` / `RT_AreaLightCount`. **RE ENGINE has area lights on the
raster path**, not only under ray tracing. `IESLightTableSRV` is a
`Texture1DArray<float>`, confirming the IES profiles RE7 described.

**Shadows: two arrays, three cascades, SDSM, and eight rotated taps.**

```
Texture2DArray<float>  StaticShadowMapSRV
Texture2DArray<float>  ShadowMapSRV
SamplerComparisonState LinearCompare
cbuffer ShadowSamplingRotation { float4 ShadowSamplePoints[8]; }
```

The static cache and the dynamic map are **both bound to the lit pass and
combined in the shader**. That is exactly the `min(A, B)` two-map arrangement
[`source2_rendering.md`](../valve/source2_rendering.md) §9.4 proposed and §11.5
here argues for — **Capcom do it too, and do not composite into one map at shading
time.** Worth correcting the impression §3.1 leaves: the `CacheCopy` step builds
the cache, but the *shading* reads two arrays.

From `LightInfo`, the directional light carries:

```
Cascade_Translate1..3   Cascade_Bias1..3   Cascade_Scale1..3
CascadeDistance (float4)   SDSMEnable   SDSMDebugDraw
DL_Variance   DL_Bias   DL_ArrayIndex   DL_TranslucentArrayIndex
DL_VolumetricScatteringColor   DL_MinAlpha
```

**Three cascades** — the same count REAC 2025 reports for the DD2 / MH Wilds
generation, so it is stable across the engine. **`SDSMEnable` is Sample
Distribution Shadow Maps**, automatic cascade fitting from the depth buffer, which
Capcom have never published anywhere. `DL_TranslucentArrayIndex` is a second
shadow slice for transmitted light. `DL_VolumetricScatteringColor` confirms the
per-light volumetric scattering that RELit exposes as a tool parameter.

**And there is no PCSS.** Zero hits for `pcss`, `blocker`, `penumbra` or
`poisson` across all three shaders. What exists is **eight sample points, rotated
per pixel, fetched through a hardware `SamplerComparisonState`** — a rotated PCF
kernel — alongside a `DL_Variance` term. §3.3 argued from the total absence of
PCSS in the literature that RE ENGINE does not do contact hardening. **That is now
measured rather than argued**, and this project's PCSS with a real blocker search
is genuinely the more sophisticated filter.

**The tetrahedral probe network is alive.** `LightInfo` carries:

```
lightProbeOffset   sparseLightProbeAreaNum
tetNumMinus1       sparseTetNumMinus1
smoothStepRateMinus  smoothStepRateRcp
LightProbe_WorldOffset   AOTint
```

`tetNumMinus1` is a **tetrahedron count**. Earlier research could not establish
that RE7's tetrahedral network survived, because no engine dump contained the
word — and here it is, in the shipped lighting shader, with a *sparse* variant
alongside matching the `LightProbesType { Indoor, Outdoor, Sparse }` enum.
**RE7's 2016 irradiance-volume design is still what lights this game.** That is
the strongest single piece of evidence in this document for §11.2.

> **These captures are NOT the 2019 build.** The shaders compile under **SM6.0**,
> and GDC 2019 states SM6.0 was tried but *"not enough time to ensure
> stability"* — so it did not ship in 2019. The frames also carry bindless
> tables, `CheckerBoardInfo` (which likely explains the non-native render
> resolutions — checkerboard, not a scale slider), `vrsVelocityThreshold`, and
> `RT_*` light counts. This is the build after the **June 2022 next-gen patch**.
> Everything above is true of RE2R *as it ships today*; where it differs from the
> 2019 original is not established, and the bindless and SM6.0 parts almost
> certainly post-date it.

#### Volumes, probes, IES and colour — from the texture inventory

A render-target walk cannot see any of this: froxel fog, light culling and
colour grading all live in 3D textures written by compute. Sweeping the
inventory by *depth* rather than by binding finds them.

**Colour grading: 50 LUTs at 32×32×32 `R8G8B8A8_SRGB`, resident simultaneously.**
Not one grade — fifty, which is a **per-zone grading set** blended as the player
moves between areas. Note the size: **32³, not the 64³** RE:2023 describes for
the baked ACES RRT+ODT. Those are different jobs — a 64³ to bake an expensive
transform once, 32³ for an artist's look per room — and a renderer can want both.
This is the concrete form of §8's argument, and it is a much bigger deal than "we
should add a LUT": the interesting part is that there are *fifty* of them.

**Local reflection probes, exactly as RE7 described them:**

| resource | reading |
|---|---|
| `512×512`, **128 slices**, 5 mips, `R11G11B10_FLOAT` | 128 **octahedral** probes, roughness-prefiltered |
| `256×256`, 6 faces, 5 mips, **`BC6_UFLOAT`** ×many | the baked cubemaps — RE7 states "256×256 BC6H" |
| `256×256`, 6 faces, `R11G11B10_FLOAT` render targets | live capture into cube faces |

RE7's deck says local cubemaps are stored 256² BC6H and expanded at runtime to a
512² octahedral map with mips. **All three stages are visible in the frame.**

**A second shadow atlas for local lights**: `512×512` with **128 depth slices**,
alongside the two 2048²×32 directional arrays. So the shadow budget is 32 slices
at 2048² for the sun and cache, plus 128 at 512² for everything else.

**IES profiles**: a `512×1` **64-slice** `R16_FLOAT` 1D array — the
`IESLightTableSRV` the lighting shader binds. Sixty-four photometric profiles.

**Volumetric fog** is present but not yet named. The candidates are two pairs of
small volumes — `32×32×16` and `32×32×32`, one `R16G16B16A16_FLOAT` and one
`R11G11B10_FLOAT` in each pair — which is the froxel signature (scattering and
extinction in one, integrated in-scatter in the other). It is corroborated by
`DL_VolumetricScatteringColor` in `LightInfo` and by the visible god-ray in the
RPD hall capture. **[inferred]** — these are written by compute, so confirming
them means walking the dispatches, which this pass did not do.

Also present and unexplained: a `64×32×384 R11G11B10_FLOAT` volume, and a
`32×80×32 R32_UINT` volume which is almost certainly the
`LightCullingVolumeSRV` the lighting shader binds — i.e. **the cluster grid, at
32×80×32 carrying 32 bits per cell.** RE7 published 60×34×16 at 1080p, so the
shape has changed while the scheme has not. **[inferred]**

**A note on names.** The engine sets no debug names on *resources* — RenderDoc
reports them all as "3D Texture 3141" and similar. Everything named above is
named because it appears in a **shader's reflection data**, not because the
resource carries a label. That is why the lighting shader was worth more than the
whole texture inventory.

#### The compute map — 848 dispatches, named

Every compute entry point in the frame, by dispatch count. This is the closest
thing to a table of contents for RE ENGINE that exists outside Capcom.

| system | entry points |
|---|---|
| **culling** | `CS_FastClear` ×372, **`CS_MiniClusterFrustumTest` ×112**, **`CS_MiniClusterCompaction` ×112**, `CS_InstancingCompaction` ×56, `CS_DrawIndirectArgumentFill` ×44, `HiZCS2/3/4`, `CS_CullVolumeOccluder`, `CS_CulltestFirst`, `CS_UpdateInstanceCount` |
| **lighting** | `CS_LightCulling2`, `CS_LightCullingPatch`, `CS_LightFrustumTransform`, `CS_LightSphereTransform`, **`IndirectIlluminationCS`**, `PreCalculateLighting`, `CalculateGpuBillboardLighting` ×22 |
| **screen space** | `SSR_IndirectPrepareCS`, `SSR_RayTrace_CS`, `SSR_IndirectClear_CS`, `SSR_ResolveReflectionCS`, `SmallDiffuseFilterCS` ×7, **`FastScreenSpaceSubsurfaceScatteringParam`** |
| **post** | **`HistogramCS`**, **`WhitePointCS`**, `MotionBlur_TileMaxHCS`, `MotionBlur_TileMaxVCS`, `MotionBlur_NeighborMaxCS`, `FillVelocityCS` |
| **geometry** | `CS_SkinningMeshTransform`, `CSSkinning`, `CSBlendShape`, `CSCalculateNormal` ×23, `CSWriteNormal`, `CS_ProbagateDupVertex` *(sic)*, `CS_VertAreaSkin`, `CS_PrimAreaSkin` |

Four things worth pulling out.

**"MiniCluster" is Capcom's name for sub-mesh culling granularity.** 112
frustum-test dispatches and 112 compaction dispatches per frame. CEDEC 2018 and
GDC 2019 describe this as "automatic division into 256-triangle batches with a
per-batch AABB" without ever naming it — and it is the direct ancestor of the
meshlet pipeline REAC 2025 describes. **The culling system is by far the largest
consumer of compute in the frame**, which is consistent with GDC 2019's admission
that it barely paid for itself until the barriers were fixed.

**`HistogramCS` + `WhitePointCS` means auto-exposure is histogram-driven**, with
a computed white point. Our `ToneMapPass` has a fixed exposure uniform and a
hard-coded `kLinearWhite = 11.2`; both of those are the *constants* that these
two passes compute per frame. That is a more valuable thing to copy than the tone
curve itself.

**Subsurface scattering is confirmed screen-space in RE2R**, via
`FastScreenSpaceSubsurfaceScatteringParam` — so GDC 2026's *"RE ENGINE implements
subsurface scattering using screen-space blurring"* describes a technique already
running here, not a recent choice.

**And the resource named `LightCullingVolumeSRV` is written by `CS_LightCulling2`,
at 32×80×32 `R32_UINT`.** Clustered light culling, in compute, confirmed by name
rather than by shape. `IndirectIlluminationCS` then *reads* it — so indirect
illumination is also a compute pass and is light-culling-aware.

#### The output transform is literally BT.709 — measured

The frame's last pass is two draws straight to the swapchain, entry point
**`ScreenOutputPS`**, sampling one `tLinearImage` through an
`OutputColorAdjustment` constant buffer. Per channel it computes:

```
if (x < 0.018)   y = 4.5 * x
else             y = 1.099 * pow(x, 0.45) - 0.099
```

**That is the ITU-R BT.709 opto-electronic transfer function, exactly** — the
0.018 breakpoint, the 4.5 linear slope, the 0.45 exponent and the 1.099/0.099
scale-and-offset, straight out of the recommendation.

It is worth being precise about why that is not a pedantic distinction:

| | breakpoint | slope | exponent | scale / offset |
|---|---|---|---|---|
| **BT.709** (what RE2R does) | 0.018 | 4.5 | **0.45** | 1.099 / −0.099 |
| sRGB | 0.0031308 | 12.92 | 1/2.4 ≈ 0.4167 | 1.055 / −0.055 |
| `pow(1/2.2)` (what we do) | — | — | 0.4545 | — |

The three agree in the midtones and diverge in the **shadows**, where BT.709's
much later breakpoint and gentler toe lift the darkest values relative to sRGB.
For a game played almost entirely in near-darkness that is not a rounding
difference, it is the part of the curve the whole image lives in.

The rest of `OutputColorAdjustment` says the encode is a deliberate, configurable
output stage rather than a constant baked into a shader:

```
float fGamma;  float fLowerLimit;  float fUpperLimit;  float fConvertToLimit;
int   uConfigMode;   float fConfigImageIntensity;   float fConfigImageAlphaScale;
```

`fLowerLimit` / `fUpperLimit` / `fConvertToLimit` are **limited-range (16–235)
television output**; `uConfigMode` selects the transform. Read alongside the
**50 resident 32³ grading LUTs** (above) and `HistogramCS` + `WhitePointCS`
computing exposure and white point per frame, the whole colour chain is:

> scene linear → histogram exposure + computed white point → tone curve →
> per-zone 32³ grading LUT → **BT.709 OETF, optionally limited-range** → display

**Every stage of that except the tone curve is a constant in our renderer.**
`ToneMapPass` has a fixed `kDefaultExposure = 4.5`, `filmic.glsl` a fixed
`kLinearWhite = 11.2` and a `pow(1/2.2)` encode, and there is no LUT at all.
§11.3 ranks the LUT; the exposure and white point are arguably the better first
move, because they are two magic numbers that a measurement can simply replace.

#### Text and UI are textured quads — no distance fields anywhere

The UI shader's entry point is **`PS2D`**, binding a single `primTex` and a
`GUIConstant` buffer. **There is no MSDF, no SDF, and no distance-field decode
anywhere in the frame.** Glyphs come from an atlas as ordinary textured
quads — which is worth knowing given how much machinery this project has built
around MSDF text (`cromwell/sdf/`, `common/sdf.glsl`, `msdf_text.fs.glsl`).

RE ENGINE can afford that because a console-and-PC game ships a known set of UI
sizes and can bake atlases per size; the MSDF argument is strongest where text
scales continuously, which is exactly this project's case. **Not a technique to
copy — a confirmation that the choice was a real fork with a real trade-off, and
that shipped AAA sits on the other side of it.**

What *is* worth noting is the constant buffer:

```
float4x4 guiViewMatrix, guiProjMatrix, guiWorldMat;
float  guiIntensity, guiSaturation, guiSoftParticleDist, guiFilterParam;
float4 guiScreenSizeRatio;   float2 guiCaptureSizeRatio, guiDistortionOffset;
float  guiFilterMipLevel, guiStencilScale;
int    guiDepthTestTargetStencil, guiShaderCommonFlag;
```

Three world matrices, a **soft-particle distance**, a **stencil depth-test
target**, and a distortion offset. This is not a 2D overlay system — it is built
to place UI **in the world**, depth-tested and soft-blended against geometry.
And it renders **early in the frame**, into its own target well before the
G-Buffer, rather than being composited last.

#### Volumetric light shafts — screen-space, bilaterally blurred, not froxels

The RPD main hall capture has a pronounced god-ray through the skylight, so the
question was never *whether* RE2R does volumetric lighting. It was *how*.

**Not with a froxel grid.** No compute pass in the frame writes a fog volume.
The two small volume pairs that look like froxels are written by
`PreCalculateLighting` and `CalculateGpuBillboardLighting` — they are a
**32×32×16 lighting volume for GPU billboards**, a cheap way to light particles
with real scene lighting, which is a nice technique and not fog. The other two
volumes are untouched this frame.

**The shafts are a screen-space chain, named in the shaders:**

```
MaskVS / MaskPS                    →  mask generation
FullScreenTriangleVS / CombineCopyPS
FullScreenTriangleVS / LightShaftBilateralBlurXPS
FullScreenTriangleVS / LightShaftBilateralBlurYPS   ×4
PreTonemapFullScreenTriangleVS / PreTonemap2_PS
```

A **separable bilateral blur**, X then Y, on a buffer that is explicitly called a
light shaft. Bilateral means depth-aware — the blur is prevented from bleeding
across geometry edges, which is the whole difficulty with cleaning up a
screen-space volumetric.

**And the choice of a *bilateral* blur is itself the clue to what feeds it.**
A radial blur from the light's screen position is smooth by construction and
needs no edge-aware filtering. A **per-pixel raymarch against the shadow map**
is noisy and needs exactly this. The presence of a four-pass bilateral chain
therefore points at a raymarched source rather than the classic cheap radial
smear. **[inferred]** — the generating pass carries a generic entry point
(`MaskPS`, or one of several literally called `main`), and confirming the march
would need disassembly rather than a name.

This is the correction §10 needs. That section inferred a froxel system from the
existence of a per-light volumetric scattering parameter. **The parameter is
real** — `DL_VolumetricScatteringColor` sits in `LightInfo` — **and the froxel
volume is not there.** GDC 2019 naming *"light shafts"* as a Depth Bounds Test
client fits: a screen-space pass with a bounded depth range is exactly what the
above is. The `VolumetricFog` component the Autodesk article documents is
therefore most likely a **Dragon's Dogma 2-era addition**, not something the
remakes had.

#### Other passes the frame names

Identified while chasing the shafts, and worth recording because none of it is
published:

- **`VS_Cone2` / `DeferredProjectionSpotLightPS`** — spot lights are drawn as
  **cone geometry** in a deferred pass, and "Projection" means they carry a
  projected texture (a cookie/gobo). Twenty draws in the RPD hall, which is the
  sconces.
- **`VSGpuBillboard` / `PSGpuParticle`** — GPU-simulated particles, lit by the
  billboard lighting volume above.
- **`VfxVertexShader` / `VfxPixelShader`** — the general VFX path, interleaved
  with the particles across a 74-draw transparent pass.
- **`FullScreenTriangleVS`** — full-screen *triangle*, not a quad, for every post
  pass. A quad's diagonal seam costs a second raster pass over the shared edge;
  the triangle avoids it. Free, and this renderer draws quads.
- **`NewBlending`**, **`PreTonemap2_PS`**, **`CombineCopyPS`** — the composite
  chain ahead of `ScreenOutputPS`.

#### Everything else the captures settle

- **Occlusion culling** rasterises into a **`128×64` MSAA 4×** depth target
  (469 draws at the gas station, 392 in the RPD hall). CEDEC 2018 published
  **256×128** MSAA 4×.

  **It is a fixed size, not a scaled one — and that is measured, not assumed.**
  A seventh capture was taken at a different quality preset, which moved the
  render target from `1129×635` to `1476×830`. The occlusion buffer stayed at
  **`128×64`** across both. Two different render resolutions producing the same
  cull buffer settles what a single native capture could not: **RE2R's occlusion
  buffer is half CEDEC 2018's published figure in each dimension, and the
  difference is a change, not a scaling artefact.**

  For contrast, the things that *do* scale with render resolution behaved as
  expected across the same pair: the AO deinterleave went `283×159` → `369×208`,
  exactly a quarter of the render target each time. And the shadow array stayed
  at `2048²×32` in both, so it is fixed too.
- **The partial Z-prepass is real and measurable**: 348 depth-only draws against
  2 154 in the G-Buffer at the gas station, so roughly **16% of geometry** gets
  a prepass. That is "meshes close to the camera" quantified for the first time.
- **Ambient occlusion is deinterleaved** — quarter-resolution `283×159` with
  **16 array slices**, eight bound at once, then resolved through an
  `R8_UNORM[16]`. Capcom have published nothing about RE ENGINE's AO.
- **Bloom** is a seven-step `R11G11B10_FLOAT` pyramid, 564→282→141→70→35→17→8.
- The engine emits **no debug markers** — RenderDoc shows raw D3D12 calls — so
  the pass *names* remain unknown even though the pass *structure* does not.

#### Resident Evil 4 (2023)

Far less is published, and what exists is covered in depth elsewhere in this
document: **RT reflections only, no RTGI, occlusion on plain SSAO/CACAO** (§4.4);
**indirect lighting still probes + local cubemaps + IBL**, confirmed by RE4R's own
hair talk (§4.3); the **shading-normal correction**, the only thing Capcom credit
to RE4R's general shading (§2.2); **strand hair** with a full PS5 cost breakdown
and **shell fur** for Leon's collar (§7); **screen-space SSS**, cut entirely on
PS4 (§2.3).

The one incidental glimpse of its opaque shadowing is an aside in the hair talk:
its original approach was screen-space pixel-dither sampling *"as with opaque
objects"* — so **RE4R's opaque geometry takes its shadows through a dithered
stochastic sample**. Capcom never elaborate.

RE4R's runtime additionally carries `MeshSignedDistanceField`, `GlobalSDFResolution`,
`GlobalSDFClipmapNum`, `ShrinkShadowmap`, `MultiSignedDistanceField`,
`LightProbeRelighting`, `ProbeVisibilityResource` and `GIPointCloud` **[tool]** —
but **a symbol existing proves the code shipped in the binary, not that RE4R uses
it**, and the SDF talk is framed around open worlds with a moving sun.

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

For static sun shadow over large worlds, the directional light's depth is baked
at **16K² or 32K²** and compressed with a **quadtree over Morton-ordered
blocks** — 128×128 tiles, max 7 levels — where a leaf stores depth plus DDX/DDY
gradients at 16-bit precision and interior nodes store child indices. Shipping
engine assets are **2.15 MB to 70.7 MB** at 16K, and the shadow pass drops
**36 ms → 20 ms** on a GTX 1070 Ti. **[CEDEC18]**

> **This is very probably DMC5, not RE:2, and earlier revisions of this document
> said "RE:2/DMC5" without qualification.** The deck's shadow section opens
> 「**DevilMayCry5で特に問題となった**」 — *"this was a particular problem in
> Devil May Cry 5"* — a directional light covering a wide area, a shadow cache
> that flushes when the viewpoint moves, and vertex cost from drawing shadows
> directly. **RE:2 is never named anywhere in the section**, and the 36 → 20 ms
> slide carries no title, no resolution, and no statement of whether 36 ms is the
> shadow pass or the frame. A 16K–32K directional map is least motivated by
> RE2R's tight interiors. **[inferred]**

**Two details worth correcting and keeping.** LZ4 is *not* the shipped format —
it compresses the **intermediate authoring render** (1024×1024 chunks,
`R32G8Typeless`), because raw depth is *"1 GB at 16K, 16 GB at 64K"*. The
finished asset is the quadtree.

And the runtime trick is better than "read a baked map": **the baked depth is
copied in place of the shadow map's clear**, and the baked meshes are then
excluded from the shadow render entirely. It costs a copy that was going to be a
clear anyway, and buys the removal of every static caster's vertex work —
「ベイクすることでDepthのClearの代わりに動作 … **代わりにメモリを利用**」, memory
spent instead of vertices.

The compression predicate is one line and worth having: a 2×2 group merges into a
gradient leaf when `|A + D − B − C| < EPSILON`, i.e. when the four samples are
coplanar, with `EPSILON = TILE_SIZE/2 * FLT_EPSILON / (MAXZ − MINZ)` and
`TILE_SIZE = 128`. Leaves on the same plane across different branches are merged
afterwards, and residual numerical error is minimised with **Nelder–Mead**.

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
shadow approximation, for any title at any date** — and this has now been checked
exhaustively rather than casually. "PCSS" appears **zero times** across GCC 2016,
CEDEC 2016, CEDEC 2017, CEDEC 2018, every RE:2023 deck, REAC 2025, GDC 2026 and
every available engine dump. So do `ソフトシャドウ` (soft shadow), `VSM`, `ESM`
and `バリアンス` (variance). The single appearance of "PCF" in the entire corpus
is a note about *reducing* its sample count inside Monster Hunter: World's
volumetric light-injection shader — not the main shadow pass.

**The engine's own filter enum names no kernel at all**: `via.render.ShadowFilter`
has three values, `Custom` / `Fast` / `Default`. **[tool]**

Weak counter-evidence, tooling-derived: RE ENGINE lights carry a
**`ShadowVariance`** property, which the RELit modding documentation labels
"Shadow Blur" while noting the label is wrong **[tool]**. A variance-based filter
with a global softness scale is not PCSS.

> **A tempting claim, and why it is not in this document.** A widely-repeated
> report holds that a decompiled Monster Hunter Wilds `LightInfo` cbuffer
> contains `DL_PCSS_KERNEL`, `DL_ContactShadow`, `Cascade_Bias1..3` and
> `tetNumMinus1` — which would mean PCSS shipped without any talk mentioning it,
> *and* that the RE7 tetrahedral probe network is still alive in 2025. It would
> be the single most useful fact in this document if true.
>
> **It could not be corroborated, and the reason is structural rather than a
> failed search.** There is no MH Wilds or DD2 engine dump available at all —
> the newest is RE4 (2023), and the enum *values* everyone quotes are read from
> a **RE2 2019** build. There are no shader decompiles of any kind. So the right
> class of evidence simply does not exist in reach, and every symbol above
> returns zero hits.
>
> Capcom's own `LightInfo` — the one in the GDC 2026 code listing — is a
> per-light record in an SRV array indexed by the world-space light grid, which
> is a *different structure entirely* from a per-frame cascade constant buffer.
> The shared name corroborates nothing. **Treat the report as unverified.**

**What RE ENGINE actually ships for soft shadows is the SDF, not PCSS**, and
Capcom say so plainly **[COC23]**: *"Cascade Shadow Map for near view and SDF
Shadow for far view… Since the SDF shadow is designed to be used for distant
scenery, it's a fuzzy shadow that's using the low-precision SDF. **The soft
shadows are not rough when viewed from a distance, but they are not suitable for
close-ups.** Cascade Shadow Map is used in the foreground instead."*

So the softness split is by *distance*, and the near field — where contact
hardening would matter — is an ordinary cascaded shadow map with an unnamed
filter.

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

**Capcom's own verdict on this system, from the same deck** — unusually blunt,
and every complaint is one we would inherit **[CEDEC16]**:

> 「ネットワーク構築は超遅い – 綺麗なＢＳＰツリーを作るのは困難 … プローブ間の
> 補間が美しくない（**まるで頂点カラー**） – ライトリーク • 現状プローブの位置を
> 移動することで回避」

Network construction is *"super slow"*, clean BSP trees are hard, interpolation
between probes is *"not beautiful — **just like vertex colours**"*, and light
leaks are worked around by **moving the probe by hand**. Note that the last two
are the two failures §11.2 would be buying into, and note equally that **both are
consequences of irregular probe placement in a triangle world** — interpolation
across a tetrahedron is what looks like vertex colour, and hand-moving a probe is
what you do when you cannot say exactly which cell a probe belongs to. On a
regular lattice neither applies.

### 4.1.1 Where it went next — and it is a better model for us than RE7's

Monster Hunter: World's 2017 talk shows the same system a year later, simplified
in exactly the directions that suit us. **[CEDEC17]**

> 「**Irradiance(放射照度）のみ** • **3次SH**（球面調和関数）で圧縮 [Sloan08] •
> 時間変化は係数間の**線形補間** • プローブネットワーク [Cupisz12] …
> Gaussian、HanningまたはLancsoz の**窓関数**」

Four decisions worth having:

- **Irradiance only.** Not radiance, not a specular term — the probe answers one
  question.
- **Third-order SH**, rather than RE7's four directional colours.
- **Time of day is a linear interpolation between coefficient sets.** A day-night
  cycle costs one lerp, because SH coefficients are linear in the lighting.
- **A window function** — Gaussian, Hanning or Lanczos — to kill ringing, which
  is the HDR failure mode of SH and the reason naive SH probes flash dark rims
  around bright sources.

And the structural retreat: probe placement moved **from automatic voxelisation
to hand-authoring in the DCC tool**, stored in a **uniform AABB grid**, with the
tetrahedra hit-tested against each cell. **The BSP tree is gone.** Runtime becomes
a grid lookup then a short loop over the cell's tetrahedra, made cheap because
the AABB is wave-uniform so the loads are scalar.

**Read that as a warning about §11.2's shape, not its substance.** Capcom
abandoned automatic placement and abandoned the BSP — the two most elaborate
parts of the RE7 design — and kept SH-compressed irradiance on a uniform grid.
That is *nearly exactly* what a lattice-aligned probe grid here would be, arrived
at by an engine that started from the sophisticated end and retreated. We would
be starting where they finished.

The open question it raises: **SH3 versus RE7's four directions.** Third-order SH
is 9 coefficients per channel — 27 floats against 4×R11G11B10's 16 bytes. §11.2
picks four directions on size, which is right, but the MHW line means SH is the
direction the same team moved *toward*, with windowing as the price of admission.

### 4.1.2 Does the probe system survive to the modern titles?

**Symbolically, yes — into RE4 at least.** The engine's type system carries
`LightProbes` with a `LightProbesType { Indoor, Outdoor, Sparse }`,
`LightProbesInterpolatable`, `IrradianceFilter`, `LocalCubemap`, `ProbesResource`,
and a `CubemapCapture::BASIS { SH_ORDER1, SH_ORDER2, FC3_BASIS, AMBIENTCUBE_BASIS }`
— i.e. the probe storage basis is still selectable and still SH. RE2RT and RE4
add `LightProbeRelighting`, `ProbeVisibilityResource` and `GIPointCloud`; RE8
onward adds `LightProbeBlocker` and `LightProbeBlockerHole`, which are exactly
what you would build to stop leaks after complaining about them in 2016.
**[tool]**

**But nothing establishes it is still *tetrahedral*.** No symbol in any available
dump contains "tetra", "Delaunay" or "simplex". `LightProbesInterpolatable` hints
and proves nothing. So: the probe system demonstrably survives to 2023; its
interior structure after 2017 is unpublished and unverified.

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
rays. The fault is specific and worth naming: **the first pass's bake was
interpolated by the second pass, and that interpolation could not occlude
properly, so multi-bounce light penetrated geometry.** The probe network is the
consumer of that bake and is untouched by the change. Capcom attach **no date
and no title** to the switch.

**The local cubemap turns up alive twenty-odd slides later in the same deck**,
which is about as direct a refutation of "retired" as the source can give. Under
bindless applications **[COC23]**:

> "Mostly used to reduce Video Memory • **Local Cube map (Reflection Probe)** •
> Local Cube maps don't need to be copied to Texture Array etc. during runtime •
> Arbitrary resolution can be retained"

Not merely surviving — being *improved*, and improved in a way that removes the
copy-into-an-array step and lets each probe keep its own resolution. That is a
system being invested in, not one being wound down.

**Stated as a confirmed absence, because it matters:** across the whole RE:2023
corpus there is **no statement retiring an RE7-era irradiance probe volume**. The
words "irradiance volume", "probe volume" and "SH probe" do not appear. The only
runtime-probe discussion in those decks is the slide about *Lumen* — Epic's
system, not Capcom's. Whatever happened to the tetrahedral network, Capcom have
not said it happened.

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

### 4.6 Street Fighter 6 — ray tracing used as a *baker*

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

**The honest costs — and this paragraph used to overstate them.** It read
"a fragment shader on GL 3.3… Capcom's figures are compute shaders on async,
which we have neither of." **Half of that was already false**: `CMakeLists.txt`
forces `OPENGL_VERSION "4.3"`, and `cromwell/gpu/compute/` wraps compute
programs and SSBOs with the memory barrier made structural, so Capcom's three
compute shaders are a shape we can actually write. See the same correction in
§0 and §9.

**What survives the correction is the scheduling half, and it is the part that
matters.** Capcom's 2.5 ms is stated *with async off* precisely because in
shipping it runs on a second queue, concurrent with the ordinary shadow-map
pass — the cost is real but hidden. **GL has no async compute.** There is one
queue, `glDispatchCompute` orders against the draws around it, and every
millisecond lands on the critical path in full. So the number to beat is not
Capcom's 2.5 ms overlapped, it is 2.5 ms *added* — against a frame that
currently draws one 4096² shadow map and nothing else expensive. That is the
experiment.

Half-resolution with a blur, as they do for AO, is the mitigation and it is
already the shape of our SSAO pass. Soft shadows also want several cone widths'
worth of steps. This is a real experiment with a real chance of being too slow
at 1080p, and it should be measured before it is designed around.
But it is the highest-leverage thing in this document, and the reason is that
**the expensive half of the technique — building and maintaining the field — is
nearly free for a game whose world is already a voxel grid.**

---

## 6. The ray-tracing line, read for its decisions

We cannot run any of this. It is recorded because the *ordering* of what Capcom
ray-traced, and why, is informative.

**The arc.** DMC5 SE (v1: GI, reflections) → Village and the remakes (v2) → v3
(denoiser rework, mirrors, area lights) → Requiem and PRAGMATA (full path
tracing, direct lighting included). **[COC23]**, **[GDC26]**

But "v2" hides how differently each title used it, and the differences are the
interesting part **[COC23]**:

| title | version | ray traced |
|---|---|---|
| DMC5 Special Edition | 1 | GI, reflections — *"Acceleration Structures for characters and backgrounds as much as possible"* |
| Resident Evil Village | 2 | GI, reflections, AO — but *"limited to using it for backgrounds only"* |
| Resident Evil 2 / 3 / 7 | 2 | GI, reflections, AO, *"including characters"* |
| Resident Evil 4 | 2 | **reflections only** |
| Exoprimal | 2 | *"supports it only in cutscenes"* |

Capcom's summary of the pattern: *"**Each title decides what to use Ray Tracing
for, depending on the performance at runtime and what is being represented.**"*
Three versions exist concurrently *"to keep released titles stable"* — a shipped
engine carrying three generations of denoiser at once, rather than migrating.

### 6.1 Ray-traced shadows did not exist before v3, and the evidence is a rejection

This was an open question in earlier revisions of this document, because the
"Light Probes" slide says *"by using shadow rays, you can achieve shadows that
are more stable than that of a shadow map and can be produced faster"* — which
reads like a runtime feature. **It is not. It is about the offline bake**, and
three things in the same deck settle it **[COC23]**:

1. The preceding sentence is *"When baked with Ray Tracing, it is just light
   tracing"*. "Also, by using shadow rays…" continues that subject.
2. The two neighbouring slides are lightmap baking (*"Lightmaps used in
   background in some stages in Street Fighter 6"*) and SDF baking.
3. The section closes: *"Hardware Ray Tracing is a useful feature not only for
   the game at runtime, but also **as a development tool**."*

**No runtime ray-traced shadow appears in v1 or v2 at all.** The per-title table
lists only GI, reflections and AO. The first is the **v3 area-light shadow**.

**And shadow maps were never going anywhere.** The proof is lovely because it is
a *reason a technique was abandoned*: the "guiding light position" idea was
dropped because *"it needs shadow rays to check if small geometry lights are
reachable. **Geometry lights don't have shadow maps.**"* That sentence only
makes sense in an engine where directional, punctual and area lights *do* — so
the rasterised shadow map is alive and is what makes buffered lights cheap. The
lens-flare talk corroborates it from the other side, sampling a shadow map per
frame on PS5 to occlude off-screen light sources.

**Dragon's Dogma 2 makes the division explicit, and it is the clearest single
sentence on this in any Capcom source** **[REAC25]**:

> "Dragon's Dogma 2 uses raytracing for global illumination. **GI lighting reuses
> shadow map for shadows. Unable to represent shadows outside field of vision.**"

A ray-traced-GI title, in 2024, taking its shadows from the shadow map — and
hitting the obvious wall, that a shadow map only covers what the camera can see.
Their fix is worth knowing: **inject the previous frame's ray-hit positions into
the shadow map** to extend its coverage beyond the frustum. A shadow map fed by
ray hits rather than only by a rasterised pass.

**Indirect first, direct last.** For five years ray tracing did indirect
illumination only. The RT deck states the substitution target precisely: enabling
RT *"replaces the traditional approximation functionality — Replaces **Indirect
Illumination of Opaque Meshes**, etc."* Not shadows. Direct lighting moved only
at path tracing **[NV]**, and the stated motivation was removing the
gameplay/cinematic discontinuity. The GI-before-shadows ordering matches
`source2_rendering.md` §10's ranking, arrived at independently.

### 6.2 Cost

**PS5, v2 denoiser — and this is Village-era, restated from GDC 2021.** An
earlier revision of this table labelled it v3, which was wrong. **[COC23]**

| group | stage | cost |
|---|---|---|
| preparation | linear depth / geometry, motion, disocclusion history | **376 µs** |
| tracing | generate ray direction 122, sort 69 | **2 481 µs** |
| | trace diffuse 1 583, specular 609 (overlapped 1 665) | |
| | shade diffuse 596, specular 148 (overlapped 625) | |
| accumulation | spatial diffuse 512, specular 128 (overlapped 612); firefly 49; moment 75; temporal 307 | **1 043 µs** |
| filtering | copy 117, wavelet 32–200, bilateral 172, composite 290 | **650–838 µs** |
| | **total** | **~4.7 ms** |

Narrated as: *"ray tracing in total cost 4.7ms. Tracing and shading cost 2.3ms.
Others like generate ray direction and sorting cost 0.2ms. Denoiser cost 2.2ms."*
Sub-items do not sum to their group totals because several are marked
"(Overlapped)" — async, concurrent.

**v3 deltas, PS5:** spatial denoise steps **+1.3 ms**, wavelet filtering removed
**−0.8 ms**, net **+0.5 ms** on the 2.2 ms v2 denoiser baseline, plus
disocclusion rays at **~25% of trace-and-shade cost**. (The deck's own body says
+1.3 while its speaker note says "about 1ms" for the same item; the +0.5 net is
consistent in both. Recorded unresolved rather than smoothed.)

**Path tracing, Dec 2025** **[GDC26]**: 8.78 ms @1080p / 20.09 ms @4K on an
RTX 4070 Ti; 24.20 / 67.93 ms on an RTX 3060.

### 6.3 The denoiser — corrected, and the part worth reading

**The resolution ladder is three rungs, not an upscale from one buffer to
another.** An earlier revision said "disocclusion rays at 120×67 upscaled to
960×540", which garbled it **[COC23]**:

| buffer | resolution | carries |
|---|---|---|
| colour / average direction | **120×67**, 16–64 rays/px | low-frequency **light** |
| moment / shadow | **960×540**, 1 ray/px | medium-frequency **visibility** |
| normal | **4K** | high-frequency **geometry** |
| → final colour | **4K** | via spherical-harmonic projection |

Consoles run every rung checkerboarded. The reasoning is the transferable part:
*"Light contribution for GI is propagated diffusely and continuously. After
propagation, only low frequency output remain which means it can be compressed
in low dimension. On the other hand, visibility term is high frequency geometry
information. And it is critical to the final image quality."* **Separate the
signal by frequency and spend resolution only where the frequency is** — which
is a principle, not a ray-tracing technique, and it is why 67p at 64 rays beats
540p at 1 ray: *"67p's image is substantially clearer and easier to propagate."*

**A correction to an earlier claim in this document.** "Guided-direction
accumulation over up to 500 frames" was recorded here as a v3 feature. The quote
is real — *"Guided history can accumulate up to 500 frames"* — but it belongs to
one of six *candidate* ideas, and the deck closes it with *"You can't guide
direction if you don't have history. So, as previously mentioned, we try to
solve noise only using one frame data. **That doesn't help for this purpose.**"*
It exists as code and it does not answer the problem v3 was solving. **Do not
cite it as shipped.**

**What Capcom rejected is more useful to us than what they built**, since we can
run none of it:

- **Machine learning** — *"the best fit for denoiser"*, rejected because
  *"current generation consoles can't afford the cost"*, and then, unusually
  frankly: *"I don't have enough time and budget to implement machine learning
  framework in RE ENGINE."*
- **Probes, i.e. Lumen** — *"they improved this method to the extreme.
  Performance and quality are balanced… **In my opinion, it won't be easy to go
  beyond what they've already accomplished.**"* An engineer at Capcom declining
  to compete with Epic's radiance cache, in print.
- **Delaunay triangulation** — 2D triangles give *"disfigured results"* against
  3D geometry; 3D is too slow.

**The other headline v3 change is removing a Reinhard curve** — filtering was
happening in non-linear space, causing ghosting. Removing it fixed the emissive
brightness *"issue that artists tend to complain about"*, and broke every
artist-tuned light balance in every shipped title: *"**this feature can only be
used for new titles.**"* That is the clearest illustration in this document of
why a renderer change is sometimes gated by content rather than by code.

### 6.4 Path tracing — where direct lighting finally moved

**The transition, stated plainly** **[GDC26]**: *"In path tracing, the Lighting
pass replaces all existing rendering passes, **including direct lighting**. In
ray tracing, direct lighting is handled the same way as in rasterization. Ray
tracing is applied only to indirect lighting."* Both modes share one pipeline and
`RayQuery`, and evaluate the same bindless materials — PT is not a separate
renderer, it is the same one with the lighting pass swapped.

**Whether shadow maps survive in PT mode is genuinely unresolved, and this
document will not pretend otherwise.** The strongest evidence they are gone is an
artefact report: strand hair shadows mismatch under PT *"because [RT and
rasterization] render Strand depth onto the Shadow Map"* — i.e. PT uses shadow
rays where the others use a map. But the BVH slide lists async build running
*"alongside Visibility Buffer, G-Buffer, and **Shadow Casting passes**"*, and the
pipeline diagram shows no shadow pass for either mode. Capcom never say. Recorded
as unknown.

### 6.5 Sampling structures worth knowing even without RT

**[GDC26]**, and the first item needs a correction to an earlier revision of this
document.

- **Punctual lights are culled into a world-space 16×128×128 3D texture holding
  light IDs as a bitmask.** This document previously said that gives "O(1) light
  sampling". **It does not, and the deck never claims it does.** The shader is
  `while (mask) { id = firstbitlow(mask); … }` — cost is proportional to the
  number of lights *set in that cell*. The grid's purpose is stated narrowly: to
  shrink the candidate set before RIS. **O(1) is claimed only for Walker's alias
  method**, below. If this project ever grows past a handful of local lights, a
  world-space light grid over the lattice is still the right idea — and the
  lattice is already the grid — but the win is a smaller candidate set, not
  constant time.
- One neat detail in that shader: `WaveActiveBitOr(mask)` ORs the wave's lanes
  and iterates the union, so divergent lanes share one light-fetch loop.
- **Emissive polygons are sampled by Walker's alias method**, which *is* O(1),
  over a two-level structure: triangles within a sub-mesh weighted by area and
  built once at startup, then sub-meshes weighted by area × emissive intensity
  and rebuilt only on frames where the weights change. **4 096 samples are
  pre-generated per frame, each by a 4-candidate RIS**, and **32 candidates are
  used per NEE at shading time.** An earlier revision here merged those two
  numbers into "4 096 samples/frame, 32 RIS candidates" — they are different
  counts at different stages.
- **Shadow ray budget: 2 or 3 on the first bounce** (directional gets its own;
  punctual and emissive share one through RIS; IBL sometimes a third),
  **1 on later bounces.**
- **Screen-space alpha test**, for cutout geometry the BVH still contains: project
  the any-hit position through the instance's BVH matrix and the view-projection,
  and if it lands on screen *behind* the rasteriser's depth, `IgnoreHit()`. Capcom
  describe it as the inverse of a shadow map — in the Japanese, it *"grants
  permission to pass through"* rather than making shadows. Same depth comparison,
  opposite conclusion. Note it is screen-space and therefore silently does
  nothing off-screen; the deck does not discuss that. **[inferred]**

**IBL is excluded from RIS candidates indoors**, and the mechanism is sharper
than "little of it is visible". **Streaming RIS generates candidates without
evaluating visibility**, so a bright IBL swamps the candidate set — and then
indoors the IBL is occluded, so those candidates all die at the shadow-ray stage
and the variance is enormous. Excluded IBL is then *evaluated independently* of
the scene light list, not dropped. **[GDC26]**

A sealed interior receiving strong sky light is a pathology in Capcom's sampler
for the same reason it is an artefact in our analytic ambient — different
symptom, same underlying error: **a bright environment term applied without
asking whether it can be seen.**

### 6.6 The denoiser is the architecture, and it constrains everything upstream

**DLSS Ray Reconstruction is the only denoiser named in the entire deck** — no
NRD, no SVGF, no in-house filter, no fallback. That has a consequence Capcom
never state in one sentence but spend ten slides on:

**Anything that changes a surface's appearance without changing a guide buffer
gets denoised away.** Shader-animated normals on rain puddles; translucent
raindrops; animated projector cookies; animated emissive on holograms;
screen-space subsurface blur. Every one produced artefacts, and every fix is the
same shape — write the change into some buffer the denoiser watches. The SSS fix
is the whole recipe in one line: **the guide is `luminance(after) −
luminance(before)`**, one channel, fed to RR.

That is a real architectural lesson about temporal reconstruction generally, and
it is why this project's 2× supersample — which reconstructs from nothing and
knows nothing — has no equivalent failure mode and no equivalent tuning burden.

Two other candid notes worth keeping. **Shader Execution Reordering was tried and
shelved**: *"We tried Shader Execution Reordering. Currently, it is not enough of
a performance gain."* And the **shadow-terminator fix is cutscenes only**, because
the data it needs is not in the G-Buffer so it costs an extra trace — a cleanly
stated quality-for-cost boundary.

**Cost** **[GDC26]**, measured 12 December 2025 on Resident Evil Requiem, and
note the table is headed *DLSS Performance* — so these are output resolutions
with the tracer running at roughly half-linear, which Capcom imply but never
state. **[inferred]**

| GPU | 1080p | 1440p | 4K |
|---|---|---|---|
| RTX 2070 Super | 29.95 ms | 42.76 ms | 99.72 ms |
| RTX 3060 | 24.20 ms | 38.38 ms | 67.93 ms |
| RTX 4060 Ti | 13.64 ms | 19.00 ms | 34.39 ms |
| RTX 4070 Ti | 8.78 ms | 11.70 ms | 20.09 ms |
| RTX 5060 Ti | 11.98 ms | 16.64 ms | 30.78 ms |

Capcom's own caveat: *"This is based on materials from the Resident Evil Requiem
development team in December. Under the development environment at that time,
performance trends looked like this."* No per-stage breakdown exists anywhere in
the deck — not one millisecond figure outside this table, and no buffer format
for anything.

**And the scale of the effort**: *"From start to ship, two developers worked for
roughly 1.5 years."*

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

> **§2.5 measures the shipped end of this chain, and it differs from the talk in
> two ways worth carrying.** RE2R holds **fifty 32³ grading LUTs resident** and
> blends them per zone — the deck's 64³ is the *baked RRT+ODT*, a different job
> from an artist's per-room look, and a renderer can want both sizes for both
> reasons. And the final encode is the **BT.709 OETF**, not sRGB and not a gamma
> constant, applied in a configurable output stage that also supports
> limited-range television output. Exposure and white point are computed per
> frame by `HistogramCS` and `WhitePointCS` rather than authored.

---

## 9. GPU-driven rendering, bindless, visibility buffer

Recorded for completeness. The original note here read "almost none of it is
reachable from GL 3.3" — **that is no longer true, and was already untrue when
written**: `CMakeLists.txt` forces `OPENGL_VERSION "4.3"` for cubemap arrays, so
compute shaders, SSBOs and `glMultiDrawElementsIndirect` have been core in this
context all along. §9.1 works through what that actually buys and what it costs.
The remaining barriers are raylib's API surface and the reason to want any of it
at this scale, not the GL version.

The first of those is now dealt with. `cromwell/gpu/GL.hpp` declares the entry
points rlgl does not wrap — `glMemoryBarrier` above all, without which a compute
write races the draw that reads it — and `cromwell/gpu/compute/ComputeShader.hpp`
puts a dispatch and its barrier in one scope so they cannot drift apart.
`cromwell/gpu/compute/ComputeSelfTest.hpp` proves the chain end to end
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
  merge dissimilar meshes and materials into a single draw. **[COC23]** —
  **and it stopped being a direction and became a shipped pipeline.** The
  REAC 2025 talk is 71 slides on RE ENGINE's meshlet rendering pipeline,
  visibility-buffer deferred, for the DD2 / MH Wilds generation. **[REAC25]**
  Still not something a 24×24 board needs, but the doc should not go on calling
  it a plan.
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
`cromwell/gpu/GL.hpp` is not needed until tier 3, which §9.1 argues against. The
two pieces of work are independent.

---

## 10. Volumetrics — what Capcom have not published

**There is no Capcom deck on RE ENGINE's volumetric fog.** Ten years of talks
cover shadows, probes, SDFs, hair, ray tracing, colour and culling, and skip the
participating medium entirely. What exists is tool-level description:

- A **`VolumetricFog`** component. Fog volumes are **box-bounded**, can be given
  **wind-driven flow and gradients**, and can be made to **conform to terrain**.
  **[tool]**
- **`CloudScape`** for the sky, and cloud shadows that are **not** cast for real:
  a **`ShadowProjectionTexture`** *pseudo-projects* the cloud pattern to weaken
  sunlight, "instead of casting onto the ground as normal, **which risks taking
  up too much processing power**". **[tool]**
- Lights carry a **Volumetric Scattering Intensity** parameter, per light.
  **[tool]**
- Rain has a full lifecycle — "**it falls, wets the environment, creates puddles
  that ripple in the rainfall, and slowly dries out**", with the aside that
  "**puddles don't form on inclines or where any mesh is bent**". Gated indoors
  and in caves by a **`DepthOcclusion`** shielding system, and the continuity is
  stated outright: DepthOcclusion "**already proved effective in Capcom's
  Resident Evil 7**". **[tool]**

**Provenance, because the tier matters here.** All of the above comes from an
Autodesk-published article on Dragon's Dogma 2's tooling, quoting Capcom's own
lighting artist. It is a **staff interview in English translated from a Japanese
original**, not an engineering talk — so the feature *names* are reliable and
nothing about the implementation behind them is. It also describes **DD2**, not
the remakes; only the DepthOcclusion lineage is explicitly traced back to RE7.

Per-light volumetric scattering intensity strongly implies a froxel injection
pass, since that is the parameter such a pass needs and no other architecture
wants it. **[inferred]** But it is an inference, and no format, resolution or
cost is public.

**One scrap of Capcom's actual engineering word does exist**, and it is the only
one: GDC 2019 states the **Depth Bounds Test is used "in RE ENGINE … for decals
and light shafts"**, to skip pixels fully occluded by a wall. **[GDC19]** That
is not a description of the volumetric system, but it does establish that **light
shafts are a distinct screen-space pass with a bounded depth range** rather than
something falling out of a froxel march — which is a small argument *against* the
froxel inference above, or at least for the two coexisting. Capcom say nothing
more.

> **Measurement has now settled this, and the froxel inference was wrong.**
> §2.5 walks an RE2R frame containing a pronounced god-ray. There is **no fog
> volume** — but there *is* a named screen-space chain:
> **`LightShaftBilateralBlurXPS` → `LightShaftBilateralBlurYPS`**, a separable
> depth-aware blur over a buffer Capcom themselves call a light shaft.
>
> So the per-light parameter this section reasons from is real
> (`DL_VolumetricScatteringColor` sits in `LightInfo`), the volumetrics are real
> and visible, and **the froxel grid the parameter seemed to imply does not
> exist.** The Depth Bounds Test note above turns out to describe the actual
> architecture rather than a detail of it. The `VolumetricFog` component the
> Autodesk article documents is most likely a **Dragon's Dogma 2-era addition**,
> not something the remakes had.

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

### 11.0 Why the remakes look as good as they do

The question this whole document exists to answer, so it is worth stating
plainly. **There is no exotic technique. Every individual piece is ordinary, and
most of it is more than a decade old.** What is exceptional is integration — and,
more than anyone likes to admit, the fit between the technology and the content.

**First, what it is *not*, because each of these is a plausible guess that the
sources rule out:**

| not this | evidence |
|---|---|
| ray tracing | RE2R and RE3R shipped with none; RE4R has reflections only and no RT GI (§4.4) |
| shadow filtering | no PCSS, contact hardening or soft-shadow technique published anywhere, at any date; `ShadowFilter` has three values and names no kernel; RE4R's opaque shadows are a **dithered stochastic sample** (§3.3) |
| shadow resolution | the shadow array's default is **1024²**, 32 slices (§3.1) |
| a novel GI algorithm | the probe network is Cupisz 2012 / Valient 2014, and Capcom's own verdict on its interpolation was *"just like vertex colours"* with leaks fixed by moving probes **by hand** (§4.1) |

**What it actually is — five things, none glamorous:**

**1. One lighting model, stated as a goal rather than an outcome.** *"All shaders
use one identical lighting model: Lambert + Cook-Torrance (GGX)"* — so a static
background and a dynamic character are interchangeable under the same lights.
There is no "character shader". Most renderers that look worse than their assets
deserve have exactly this seam, and it reads as characters being *pasted onto*
environments rather than standing in them.

**2. An architecture that makes many shadow-casting lights affordable.** This is
the big one. The shadow cache (static cached per light, dynamic composited in only
when something is actually inside that light's frustum) plus clustered culling at
**512 lights** means an artist can place a practical light in every fixture of the
RCPD without a budget conversation. **Light count is the single biggest lever on
how a scene reads, and their architecture spends its complexity buying it.**
Compare this renderer, where the ceiling is one directional sun (§0).

**3. The relit cubemap, which fixes the most common "wrong" look in real-time
rendering.** `luma(ProbeDiffuse) * luma(cube(r,mip)) / luma(cube(n,lowestMip))`
(§4.2). Without it, a cubemap baked in daylight and sampled in shadow keeps its
specular bright while diffuse goes dark — and *everything reads as metal*. In a
game that is mostly dim interiors with wet, glossy surfaces, getting this right is
worth more than any amount of filtering.

**4. Material economy that puts the exotic cases inside the standard material.**
Translucency shares one scalar with metalness; subsurface is **three bits** next to
occlusion (§2). Lampshades, leaves and skin are therefore *material settings*, not
architecture — which means artists reach for them constantly instead of requesting
a new shader. Add G-buffer decals, which modify the surface *before* lighting so
grime takes the surface's own shadow and probe, and the wetness/rain parameter
sets (§6.4), and you get walls that are dirty, damp and worn everywhere at no
per-instance cost. RE2R's police station is carried by this.

**5. Colour.** ACES through AP1 with grading authored in **DaVinci Resolve against
a live feed of the running game**, baked to a 64³ LUT at 0.4 ms (§8). Nothing
about geometry or light transport, and it does more for "does this look like a
film" than anything else on this list.

**And then the part that is not technique at all.** NVIDIA's 2026 Q&A, looking
back at exactly this generation: shadow-map limitations were *"less noticeable in
cinematics due to **manually tuned shadows**"* **[NV]**. Some of the shots that
sell the look are hand-authored per camera. A systemic renderer cannot copy that,
and should not be judged against it.

#### The uncomfortable conclusion, and the useful one

**RE2R looks extraordinary partly because its content is the best possible case
for its technology.** Bounded interiors. Static geometry. No time of day. Many
small local lights. A camera that rarely sees a long vista. Every one of those is
a condition under which cached shadow maps and a baked probe network are at their
strongest — and every one of them is a condition their *own* engine later failed
to satisfy. When Capcom moved to an open world with a moving sun, this
architecture stopped working and they had to build SDF shadows and ray-traced
probe bakes to replace it (§3.2, §5).

**That is the transferable lesson, and it is not "copy RE ENGINE".** It is:
their renderer is excellent because it was aimed precisely at bounded interiors
with many static lights and no day/night cycle — and **this project's world is
bounded, its geometry is a lattice, and its sun barely moves.** We are closer to
RE2R's problem than Dragon's Dogma 2 is. The techniques worth taking are the ones
that exploit that (many cheap local lights, cached static shadows, a lattice-shaped
probe grid, relit cubemaps, a grading LUT) — not the ones Capcom built *after*
their content outgrew them.

### 11.1 Build the SDF. It is the largest single win. — §5

One R8 3D texture over the lattice, ~324 KB, maintained by a local distance
transform after each destruction event. Then, in order of payoff:

1. **SDF ambient occlusion** alongside the existing SSAO — sees offscreen
   geometry, which SSAO structurally cannot. Half resolution, blurred, exactly as
   Capcom do it. This is the cheapest step and it is independently useful.
2. **SDF soft shadows** by sphere tracing — a third answer to §9 that has no
   projection, no crawl, no bias, no cascade, and no re-render on destruction.
   Measure it before committing. **The risk is not the GL version — compute and
   SSBOs are core at 4.3 and wrapped in `cromwell/gpu/compute/` — it is that GL
   has no second queue, so unlike Capcom we cannot hide the cost behind the
   shadow pass.** Their 2.5 ms overlapped becomes our 2.5 ms added. §5.2.
3. **Exact leak rejection for the probe grid**, which §11 of the Source 2
   document already plans to do by lattice query — the SDF is the same
   information in a form the shader can sample and interpolate.

Note how this composes with the probe-grid decision rather than competing: the
grid supplies indirect light, the SDF supplies visibility. §1's "cache repair"
argument applies to the SDF, not to the shelved lightmap — the SDF caches
*geometry*, which is small, cheap to update locally, and does not go subtly wrong
the way a stale lighting cache does.

> **RE-RANKED, 2026-08-16 — two of these three payoffs do not survive
> `plans/indirect_lighting.md`, and the heading overstates what is left.**
>
> **Item 1, SDF AO, is redundant.** Not marginal — redundant, and in all three
> target genres rather than merely on this map. Ambient occlusion splits into two
> bands and this list assumed a third that does not exist:
>
> | band | owner |
> |---|---|
> | below cell scale — creases, contacts, the line where a crate meets the floor | **SSAO**, at depth-buffer resolution. Nothing else can reach it; a field at one value per cell cannot represent a crease. |
> | cell scale and above — a room being enclosed, a courtyard under an overhang | **the irradiance field**, which answers the same question *directionally and in colour*, with bounce, instead of as a scalar multiply on an ambient term that was already the wrong colour. |
>
> SDF AO sits between them computing a degraded version of the field's answer.
> And this is an engine-level judgement rather than a content one: the split is a
> property of the techniques, and the field is genre-neutral by construction —
> `plans/indirect_lighting.md` §8's seam means an RTS or FPS supplies a field of
> the same shape from a mesh trace or a bake and gets the same term. **The engine
> should not carry SDF AO.**
>
> Note also that RE4R — the title §4.4 establishes as the best-looking and the
> one this document keeps citing — ships **plain SSAO and no SDF AO**, and
> Capcom describe their SDF as deliberately *"low-precision"* and *"designed to
> be used for distant scenery"* (§3.3). The SDF talk is framed around open worlds
> with a moving sun, i.e. Dragon's Dogma 2's problem.
>
> **Item 3, leak rejection, is superseded.** An SDF at cell resolution cannot
> represent a wall 0.09 tiles thick sitting *on* a cell boundary, which is
> precisely the geometry that leaks. `plans/indirect_lighting.md` §5.1 does it
> with three bits per cell — is the +X/+Y/+Z edge blocking — which is exact,
> 5 KB for the map, and at most three bit tests per tap because a tap is always
> in the sample point's 2×2×2 neighbourhood.
>
> **Item 2, soft shadows, survives untouched — and losing the other two makes it
> stronger rather than weaker.** No projection to refit, no texel crawl, penumbra
> in world units, no cascades, no bias, no re-render on destruction.
>
> **Because look at which genre Capcom's framing names.** Distant scenery, long
> view distances, a moving sun — that is the **large-map** case, and large maps
> are what cromwell is aimed at. The tile board is a prototype that exercises
> engine features; it is not the content any of this is being built for, and
> "our map is 24×24" is not an argument the engine may accept. Cascaded shadow
> maps are *at their worst* exactly where the SDF is at its best, so the feature
> whose case this board cannot demonstrate is the one with the largest headroom
> on the target content.
>
> The honest consequence is about **evidence, not priority**: §5.2's experiment
> has never been run because this repo has no content that would make it
> meaningful, and the 2.5 ms-added figure is measured against a frame drawing one
> 4096² map and nothing else. That is a gap in what we know, and it argues for
> building the map before drawing the conclusion — not for demoting the item.
>
> So: **"build the SDF, it is the largest single win" was true of a three-payoff
> item and is not true of a one-payoff item** — but the payoff that survives is
> the one aimed squarely at the genre the engine exists for, and it should be
> ranked there rather than against this board.

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

### 11.3.1 Octahedral probe storage — the measurement solves a stated blocker

> **STATUS: steps 1 and 2 below are DONE, and step 3 is the only thing left.**
> `DeviceProbeSet` ships a six-level GGX-prefiltered chain
> (`rhi/scene/probe_prefilter.fs.glsl`, `PROBE_MIP_LEVELS 6`), `sampleProbe`
> reads roughness straight off as a LOD, and the roughness fade this section is
> written around **is deleted** — `rhi/include/probes.glsl` records the deletion
> as the prefilter's payoff. Everything below is retained as the reasoning that
> produced it, not as outstanding work.
>
> **And the two-renderer framing has expired with it.** This section treats
> `common/environment.glsl` as a live constraint on a shipping path; that path
> is being deleted at RHI parity, so the file is quoted below as history. The
> mip chain was never the reason to want octahedral storage — per-mip control is
> — which is what step 3 says and is why it is still open and still not urgent.
> `plans/indirect_lighting.md` §1 is where this now reads from.

`common/environment.glsl` records a limitation as though it were a fact of life:

> "there is no prefiltered mip chain to sample (**rlgl cannot build one for a
> cubemap**), so rather than hand a rough surface a mirror-sharp reflection it
> never could produce, the result slides back to the analytic sky as roughness
> rises."

That is why `environmentSpecular` fades probes out across roughness 0.12→0.55,
and why every mid-rough metal in this game reflects a blue gradient instead of a
room.

**RE2R does not store its probes as cubemaps.** The capture shows a
`512×512` **2D array of 128 slices with 5 mips** in `R11G11B10_FLOAT` — octahedral
maps — alongside the `256×256×6` BC6H cubemaps they were baked from (§2.5). RE7's
deck says the same thing and this document quoted it in §4.2 without noticing what
it was worth: *"expanded at runtime to a 512×512 × 8 mip R11G11B10Float
**octahedral** map"*.

**An octahedral map is an ordinary 2D texture**, so it mips like anything else —
which is why RE ENGINE can prefilter and we assumedly could not.

> **But do not start there, and the reason is a correction to this section's
> first draft.** The blocker as stated — "rlgl cannot build a mip chain for a
> cubemap" — is true of *rlgl* and irrelevant here, because
> `ReflectionProbeSet.cpp` **already bypasses rlgl and calls raw GL**; it is the
> one file licensed to. `glGenerateMipmap(GL_TEXTURE_CUBE_MAP_ARRAY)` is core GL
> 4.0, `textureLod(samplerCubeArray, …)` is valid GLSL, and
> `GL_TEXTURE_CUBE_MAP_SEAMLESS` is already enabled in `create()`. So the mip
> chain can be had by allocating levels and generating them — roughly thirty
> lines, no format change, and **no octahedral seam to solve at all**.
>
> The honest ordering is therefore:
>
> 1. **Mip the existing cubemap array and sample by a roughness-derived LOD.**
>    Cheap, removes the roughness fade, and is most of the visible win.
> 2. **A real GGX prefilter**, if step 1's box-filtered mips read as too sharp
>    at mid roughness. Hardware mip generation averages; it does not integrate
>    the specular lobe.
> 3. **Octahedral storage**, only if step 2 wants per-mip control that a cubemap
>    array makes awkward — which is the actual reason to prefer it, not the mip
>    chain.
>
> Recording the wrong first draft on purpose: the interesting failure was reading
> a header comment as a hard constraint without checking whether the file it
> sits in was still bound by it.

Octahedral's known cost, for when step 3 arrives: naive bilinear across the
octahedron boundary produces a visible cross, so edges need handling. RE7 names
its answer — "edge-fix filtering" — and does not describe it. **[inferred]**

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
| ray-traced GI total | ~4.7 ms — trace+shade 2.3, denoise 2.2, ray gen+sort 0.2 | PS5, **Village**, restated from GDC 2021 | **[COC23]** |
| RT trace resolution | 960×540 @ 1 ray/px, halved again on console | — | **[COC23]** |
| RT disocclusion rays | 120×67 @ 64 rays | — | **[COC23]** |
| denoiser v2 → v3 | 2.2 ms, then +1.3 spatial, −0.8 wavelet | PS5 | **[COC23]** |
| strand hair raster | 3.653 ms @1920p CBR / 4.008 ms @2160p CBR | PS5, 23 115 strands | **[COC23]** |
| strand hair lighting | 1.32 ms no shadow map / 2.25 ms with | PS5, 10 spotlights | **[COC23]** |
| guide-hair lighting | 0.12 / 0.18 ms — about 1/10 | PS5, 1 420 strands | **[COC23]** |
| shell fur VRAM | 0 MB instanced / 3.18–7.72 MB MDI | — | **[COC23]** |
| grading LUT format | 64³ R10G10B10A2Unorm; artist LUT FP16 33³ | — | **[COC23]** |
| bindless, CPU side | 10.3 → 8.2 ms; G-Buffer cmdlist 3.3 → 2.1 ms | **Xbox One**, Village | **[COC23]** |
| full path tracing | 8.78 ms @1080p, 20.09 ms @4K | RTX 4070 Ti | **[GDC26]** |
| full path tracing | 24.20 ms @1080p, 67.93 ms @4K | RTX 3060 | **[GDC26]** |

> **Two traps in this table.**
>
> The ray-traced GI figure is **Resident Evil Village on PS5**, restated from the
> GDC 2021 talk. It is not an RE2R, RE3R or RE4R measurement, and RE4R does not
> run ray-traced GI at all (§4.4).
>
> More generally: **Capcom have published no per-pass GPU millisecond figure for
> any of the three remakes.** Every number above is Village, or engine-generic on
> PS4, or CPU-side on Xbox One. Anyone calibrating against "what RE4R spends on
> lighting" is calibrating against a number that does not exist publicly.

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
