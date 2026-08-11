# Terrain rendering — heightfields, layer painting, anti-tiling, foliage

Unreal's Landscape read from the engine's own C++, the layer-blending system
that lets artists paint dozens of materials seamlessly, the algorithms that stop
a repeated texture *looking* repeated, and how grass gets onto it.

The paging half of this question is [`world_streaming.md`](world_streaming.md).
The **voxel** alternative — where terrain is editable volume rather than a
heightfield — is [`voxel_terrain.md`](voxel_terrain.md), which already covers
dual contouring, clipmaps, geomorphing and triplanar-with-distance-tiers; §6 and
§7 here read against it rather than repeating it.

## Sourcing

| Tag | Meaning |
|---|---|
| **[UE-SRC]** | Read from `C:/Program Files/Epic Games/UE_5.7/Engine/Source/`. Names, fields and constants are Epic's |
| **[PAPER]** | A published, peer-reviewed technique |
| **[inferred]** | My reasoning |

---

## 1. The heightfield bargain

A heightfield is a 2D array of heights. It buys an enormous amount:

- **Storage is a texture**, so it streams, mips and compresses with the existing
  texture pipeline for free.
- **Topology is implicit** — a regular grid needs no index buffer, no mesh, no
  authoring of connectivity, and LOD is trivially "use every Nth sample".
- **Queries are O(1)** — height at (x, y) is an array lookup, so collision,
  navigation and placement are cheap.

And it forbids exactly one thing: **overhangs**. No caves, no arches, no
undercuts. Every heightfield engine then bolts meshes on top for those, which is
why "terrain" and "the rest of the world" are separate systems in Unreal and
the same system in Space Engineers ([`voxel_terrain.md`](voxel_terrain.md)).

**Source 2 did not take this bargain at all.** It has no heightfield landscape;
its worlds are meshes. s&box, built on Source 2, ships its own `.terrain` asset
type — Facepunch had to add one. So for this note Unreal is the subject and
Source 2 is the counter-example.

---

## 2. How Unreal stores it

### 2.1 One RGBA8 texture holds height *and* normal

```cpp
#define LANDSCAPE_ZSCALE  (1.0f/128.0f)
inline constexpr float MidValue = 32768.f;

// height
uint16 Height = (uint16)(Texel->R << 8) | (uint16)Texel->G;
return (static_cast<float>(Height) - MidValue) * LANDSCAPE_ZSCALE;
```

`Landscape/Public/LandscapeDataAccess.h:13-48, 207-208` **[UE-SRC]**

- **16-bit height packed into R and G** of an 8-bit RGBA texture,
- **the normal in B and A** — the accessor is literally
  `GetHeightData(LocalX, LocalY, HeightAndNormals)`,
- zero at `32768`, one unit of height per `1/128` cm, giving a total range of
  `65536/128 = 512` metres.

Two consequences worth naming. **Normals are stored, not derived** — so the
shader does not reconstruct them from neighbouring samples every frame, and
normals can be authored to disagree with the geometry (which is how a smooth
normal survives a coarse LOD). And **height precision is fixed at 1/128 cm over
a 512 m range**; a taller world needs vertical scaling, which costs precision
proportionally.

[inferred] Packing four meaningful channels into one RGBA8 fetch is a
deliberate cache decision — one texture read gives you everything the vertex
shader needs. It is the same instinct as this codebase's 2-byte-per-cell
summary replacing 68-byte `Tile` fetches.

### 2.2 Components and subsections

```cpp
int32 ComponentSizeQuads;
/** Number of quads for a subsection of the component. SubsectionSizeQuads+1 must be a power of two. */
int32 SubsectionSizeQuads;
int32 NumSubsections;
```

`Landscape/Classes/LandscapeComponent.h:427-435` **[UE-SRC]**

A landscape is a grid of **components** (the unit of culling, LOD, material
assignment and streaming), each divided into **subsections**. The
power-of-two-plus-one constraint is the giveaway: `SubsectionSizeQuads+1`
vertices per side means the *vertex* count is a power of two, which is what
makes mip generation and LOD halving exact rather than approximate.

`GenerateWeightmapMips` / `UpdateWeightmapMips` take
`(NumSubsections, SubsectionSizeQuads, ...)` explicitly **[UE-SRC]** — mip
generation has to respect subsection boundaries, because filtering across a
subsection seam would bleed one section's data into its neighbour's.

---

## 3. LOD, and the constraint people miss

Per-section LOD biases live in a GPU buffer (`SectionLODBiases`,
`GetSectionLODBias`, `FetchHeightmapLODBiases`) **[UE-SRC]**, so neighbouring
sections can know each other's LOD and stitch without cracks.

And the bias itself is not a distance function — it is **a streaming query**:

```cpp
float FLandscapeComponentSceneProxy::ComputeLODBias() const
{
    if (HeightmapTexture)
        if (const FTexture2DResource* TextureResource = ...)
            ComputedLODBias = static_cast<float>(TextureResource->GetCurrentFirstMip());
    return ComputedLODBias;
}
```

`Landscape/Private/LandscapeRender.cpp:4503-4516` **[UE-SRC]**

**Mesh detail is clamped by the resident heightmap mip.** Fully discussed in
[`world_streaming.md`](world_streaming.md) §5, because it is the cleanest
example of streaming and LOD being one problem.

**Nanite landscape** (`LandscapeNaniteComponent.cpp` **[UE-SRC]**) builds a
Nanite mesh from the landscape asynchronously — note
`LandscapeNaniteStallDetectionTimeout` and `NaniteExportCacheMaxQuadCount`,
which are the fingerprints of a build step slow enough to need a watchdog. It
replaces the bespoke LOD scheme with the general one; the heightfield remains
the authoring and collision representation. [inferred, from the component
structure — the Nanite component is additive, not a replacement for
`LandscapeComponent`.]

---

## 4. Layer painting — how dozens of materials blend seamlessly

This is the part of the question with the most practical content.

### 4.1 Weightmaps

Each layer painted onto the terrain has a **weightmap** — a texture channel per
layer holding 0-255 coverage per texel, mipped and streamed like the heightmap.
Painting writes weights; the material reads them.

The material-side vocabulary is small and complete **[UE-SRC]**:

| Expression | Purpose |
|---|---|
| `MaterialExpressionLandscapeLayerBlend` | blend N layers by weight |
| `MaterialExpressionLandscapeLayerWeight` | fetch one layer's weight |
| `MaterialExpressionLandscapeLayerSample` | sample a layer |
| `MaterialExpressionLandscapeLayerSwitch` | branch on whether a layer is present |
| `MaterialExpressionLandscapeLayerCoords` | terrain-space UVs |
| `MaterialExpressionLandscapeGrassOutput` | drive grass placement — §7 |
| `MaterialExpressionLandscapeVisibilityMask` | hole punching |
| `MaterialExpressionLandscapePhysicalMaterialOutput` | surface type for physics/audio |

`LandscapeLayerSwitch` is the one that matters for cost: it lets the material
**compile out** layers a given component does not use. Without it, every
component pays for every layer in the material. [inferred, but it is the only
sensible reading of a switch node in a system with per-component material
permutations.]

### 4.2 Three blend modes, and only one of them looks right

```cpp
enum ELandscapeLayerBlendType : int
{
    LB_WeightBlend,
    LB_AlphaBlend,
    LB_HeightBlend,
};
```

`Landscape/Classes/Materials/MaterialExpressionLandscapeLayerBlend.h:19-23` **[UE-SRC]**

- **`LB_WeightBlend`** — normalised weighted average. Layers sum to 1. The
  default, and it produces the soft muddy gradient everyone recognises as
  "painted terrain".
- **`LB_AlphaBlend`** — straight alpha over, no normalisation. For decals and
  overlays where you want one layer to simply win.
- **`LB_HeightBlend`** — **the good one.** Each layer supplies a height/
  displacement value, and the blend picks the layer that is *physically higher*
  at that texel, softened over a transition width.

Height blending is why modern terrain does not look painted: at a grass/gravel
boundary, weight blending cross-fades the two colours into a smear, whereas
height blending lets **gravel stones poke through the grass individually**,
because each stone is locally taller. The transition follows the texture's own
structure instead of ignoring it.

[inferred] The general principle is worth extracting because it applies far
beyond terrain: **when blending two textured surfaces, blend by a signal that
correlates with the surfaces' structure, not by a scalar that ignores it.** The
same trick makes decals, snow accumulation and wetness look like they belong.

### 4.3 The cost, and Runtime Virtual Texture

A terrain material with 8 layers × (albedo + normal + roughness + height) is 32
texture fetches per pixel before any anti-tiling work, on a surface that fills
the screen. This is why terrain materials are the classic shader-cost disaster.

`LandscapeComponent` implements `GetRuntimeVirtualTextures()` and
`GetVirtualTextureRenderPassType()` **[UE-SRC]**. **Runtime Virtual Texture is
the structural fix**: render the composited terrain material once into a
cached, mipped, camera-adjacent texture, then shade from *that* — one fetch
instead of thirty-two, amortised over many frames because the composite only
needs redoing when the camera moves far or the terrain changes.

[inferred] RVT is the same architectural move as this project's derived caches
(CLAUDE.md's escape-hatch pattern): expensive authoritative data, summarised
into something cheap to read, with invalidation at the boundary that owns it.
It also composes with §5 — an expensive anti-tiling scheme becomes affordable
if it runs into an RVT rather than per-pixel per-frame.

---

## 5. Anti-tiling — stopping a repeated texture from looking repeated

The problem: a 2 m² rock texture tiled across 1 km repeats 500 times. The human
visual system is extremely good at spotting periodicity, and it shows up as a
visible lattice at grazing angles and mid distance — the same lattice-artefact
family as `spatial_queries.md` §5.2's grid distance bias, arriving in the
shading domain.

Five answers, cheapest first. They compose, and shipped terrain usually uses
three of them at once.

### 5.1 Macro variation

Multiply the albedo by a large, low-frequency noise (tens of metres). Costs one
extra sample, breaks up uniformity of *tone* but not of *pattern* — you still
see the repeat, it is just less uniform. Nearly free and always worth doing;
never sufficient alone.

### 5.2 Multiple tiling scales of the same texture

Sample the texture at two or three different scales and blend. The beat
frequency between them is much longer than either period. Cheap (2-3 samples),
and effective at mid distance where the artefact is worst.

### 5.3 Distance-based detail tiers

Different textures — or different tiling rates — at close, medium and far
range, cross-faded. [`voxel_terrain.md`](voxel_terrain.md) documents Space
Engineers doing exactly this with **triplanar at three distance tiers**, so it
is not covered again here. This fixes the *far* case, where repetition is most
visible because you can see the most repeats at once.

### 5.4 Texture bombing / stochastic sampling

Scatter randomly rotated and offset copies of the texture, blend the overlaps.
Kills periodicity properly, and naively costs 3-4 samples plus blending — and
introduces its own problem, which §5.5 is the answer to.

### 5.5 Histogram-preserving blending — the real algorithm

Heitz & Neyret, *High-Performance By-Example Noise using a Histogram-Preserving
Blending Operator*, HPG 2018 **[PAPER]**. The best-founded answer, and worth
understanding rather than just citing.

**The method:** partition output texture space with a **triangle grid**;
associate each vertex with a **random patch of the input texture**; evaluate any
point by blending the **3 patches** of its containing triangle. Infinite
non-repeating output from a small example, at ~3 samples.

**The insight — and this is the transferable part.** Naive blending of texture
patches *"usually produces visual artifacts such as ghosting, softened
discontinuities and reduced contrast, or introduces new colors not present in
the input"* **[PAPER]**. That is not a bug in the blend weights; it is
arithmetic. Averaging two samples of a high-contrast stochastic texture pulls
the result toward the mean, so the blended regions are visibly flatter and
greyer than the unblended ones — you have traded a periodic artefact for a
*blotchy* one.

The fix is a blending operator that **preserves the histogram** of the input:
transform into a space where linear interpolation is variance-preserving, blend
there, transform back. The result has the same statistical distribution of
values as the source texture, so blended and unblended regions are
indistinguishable.

Reported as *"more than 20 times faster"* than comparable procedural-noise
techniques **[PAPER]**, and it applies to *"random-phase inputs as well as ...
many non-random-phase inputs that are stochastic and non-periodic, typically
natural textures such as moss, granite, sand, bark"* **[PAPER]** — which is
precisely the terrain texture set.

**The limitation, stated honestly:** it is for *stochastic* textures. It cannot
synthesise a brick wall, a road with markings, or anything with structure the
eye tracks. Terrain, moss, gravel, bark — yes. Anything man-made — no. So it is
a terrain-material technique specifically, not a general texturing one.

### 5.6 What to actually do

[inferred, from the above]

| Distance | Technique |
|---|---|
| Near | full-detail texture, macro variation (§5.1), height-blended layers (§4.2) |
| Mid | multi-scale (§5.2) or stochastic (§5.4/5.5) — this is where repetition is most visible |
| Far | distance tiers (§5.3), and increasingly an RVT or baked composite (§4.3) |

And the cost note that makes it tractable: **run the expensive scheme into a
Runtime Virtual Texture, not per-pixel per-frame** (§4.3). Stochastic sampling
at 3 samples × 4 maps × 8 layers is unaffordable in a base pass and entirely
affordable amortised into a cache.

---

## 6. Triplanar, and when a heightfield does not need it

A heightfield already has natural UVs: world X and Y. **Triplanar is not needed
for the terrain surface at all** — it is needed for *cliffs*, where the
heightfield's top-down UVs stretch to nothing on near-vertical faces.

So the standard heightfield material blends between top-down UVs on flat ground
and triplanar (or a dedicated cliff layer) on steep ground, driven by the
surface normal's Z. That slope-driven blend is exactly the height-blend problem
in §4.2 and benefits from the same treatment.

[`voxel_terrain.md`](voxel_terrain.md) covers triplanar in depth for the voxel
case, where it *is* mandatory because there are no natural UVs on an arbitrary
isosurface. **That is the real distinction: heightfields use triplanar as a
patch for steep slopes; voxel terrain uses it as the primary parameterisation.**

---

## 7. Foliage and grass

### 7.1 Grass is generated from the terrain material

The mechanism is elegant and worth copying. `MaterialExpressionLandscapeGrassOutput`
is a **custom material output** taking a list of `FGrassInput` **[UE-SRC]**:

```cpp
USTRUCT()
struct FGrassInput
{
    FName Name;
    TObjectPtr<ULandscapeGrassType> GrassType;
    FExpressionInput Input;   // density, evaluated in the material
};
```

So **grass density is an output of the terrain material itself**, computed by
the same graph that decides the terrain's appearance, from the same weightmaps.
Paint more grass layer, get more grass — automatically and by construction,
because the density expression reads the same weight the albedo does.

`LandscapeGrassWeightExporter.cpp` **[UE-SRC]** renders that density output into
a **grass map**, which the runtime samples to place instances. Grass placement
is therefore a GPU render of the terrain material, cached per component, not a
CPU scatter pass.

[inferred] The general principle: **derive secondary content from the same
signal that drives the primary appearance, rather than authoring it twice.**
Two independently-authored sources of "where is grass" will drift; one will
not. This is the same discipline as CLAUDE.md's "test derived data against its
source".

### 7.2 The instance parameters

`FGrassVariety` **[UE-SRC]**, trimmed to the interesting fields:

```cpp
TObjectPtr<UStaticMesh> GrassMesh;
bool  bUseGrid;                       // grid placement vs pure random
float PlacementJitter;
int32 MinLOD;
bool  bWeightAttenuatesMaxScale;      // low weight -> smaller instances
float MaxScaleWeightAttenuation = 0.5f;
bool  RandomRotation;
bool  AlignToSurface;
bool  bAlignToTriangleNormals;
bool  bUseLandscapeLightmap;
bool  bCastDynamicShadow;
bool  bCastContactShadow;
bool  bAffectDistanceFieldLighting;
uint32 InstanceWorldPositionOffsetDisableDistance;
uint32 bEnableDensityScaling : 1;
```

Four of these are the difference between grass that works and grass that does
not:

- **`bUseGrid` + `PlacementJitter`** — jittered grid, not pure random.
  Poisson-like coverage without a Poisson-disc pass, and no clumping. This is
  the same reasoning as `spatial_queries.md` §5.2: the *sampling pattern* has
  visible consequences, and a jittered lattice is usually the right compromise.
- **`bWeightAttenuatesMaxScale`** — instances shrink where the layer weight is
  low, so grass **fades out by size** at a layer boundary rather than
  disappearing in a hard line. The boundary problem again, solved the same way
  as §4.2.
- **`InstanceWorldPositionOffsetDisableDistance`** — stop evaluating wind
  (world-position offset) past a distance. Wind is a vertex shader cost on
  millions of instances, and beyond some range nobody can see it move.
- **`bEnableDensityScaling`** — the scalability hook, so density is a settings
  slider rather than a content change.

Rendering is via instanced static meshes (`GrassInstancedStaticMeshComponent`,
`FoliageISMActor` **[UE-SRC]**), i.e. one draw call per mesh type per cluster
with per-instance transforms.

### 7.3 The rest of the foliage stack

`ProceduralFoliageBlockingVolume`, `InteractiveFoliageActor`,
`FoliageInstanceBase` **[UE-SRC]** — plus, in modern UE, **PCG** for rule-based
scattering of larger props. The split is: **grass is derived from the terrain
material per-frame-ish; trees and props are baked instance lists placed at
author time.** Density is the discriminator — you cannot store a billion grass
instances, and you must not regenerate a forest every frame.

---

## 8. Ambient occlusion — briefly, and honestly

I did not research this to the depth of the rest, so this section is short on
purpose. What is verifiable from the source read here: landscape participates in
distance-field lighting (`bAffectDistanceFieldLighting` on foliage
**[UE-SRC]**), supports lightmaps (`bUseLandscapeLightmap` **[UE-SRC]**), and
Nanite landscape inherits the general Nanite/Lumen path (§3).

The terrain-specific AO question — baked curvature/cavity from the heightfield
versus runtime SSAO versus distance-field AO — I have not verified in this
engine's code, and this project's own AO work is covered in
[`source2_rendering.md`](source2_rendering.md). Flagging it as an open thread
rather than guessing.

---

## 9. What `cromwell` should take

This project's world is a **tile grid with storeys**, not a heightfield, so
most of §2 and §3 is not directly applicable. Five things are, and they are
general:

1. **Blend by a signal that correlates with structure, not by a scalar that
   ignores it** (§4.2). Height blending is the single most visually valuable
   idea in this note and it costs one extra channel per material. It applies to
   any two-surface transition this renderer ever has — tile-to-tile material
   boundaries, damage overlays, wetness, snow.

2. **Derive secondary content from the primary signal** (§7.1). Unreal computes
   grass density in the terrain material itself. Anything this engine scatters
   should be a function of the same data that decides appearance, not a second
   authored source that can disagree.

3. **Fade by size at a boundary, not by presence** (§7.2). Instances shrinking
   to nothing reads as natural; instances vanishing reads as a bug. Same
   principle as §4.2, applied to geometry instead of pixels.

4. **Cache the composite** (§4.3). Runtime Virtual Texture is the derived-cache
   pattern this codebase already uses, applied to shading: expensive
   authoritative material, summarised into a cheap read, invalidated at the
   boundary that owns it. It is also what makes §5's expensive anti-tiling
   affordable — so if anti-tiling is ever wanted, the cache comes first.

5. **Jittered grid, not pure random, for scattering** (§7.2). Cheaper than
   Poisson-disc, no clumping, and consistent with this project's existing
   lattice-bias discipline.

**And one calibration.** The reason terrain looks good in a modern engine is not
one algorithm. It is height-blended layers, plus three tiers of anti-tiling,
plus material-derived grass, plus a virtual texture making the total affordable
— four systems that each individually look like polish and collectively are the
feature. Budget accordingly if it is ever wanted here.

---

## Sources

**[UE-SRC]** — Unreal Engine 5.7, `C:/Program Files/Epic Games/UE_5.7/Engine/Source/Runtime/`:

| Area | Path |
|---|---|
| Heightmap encoding | `Landscape/Public/LandscapeDataAccess.h` |
| Components, subsections, weightmap mips | `Landscape/Classes/LandscapeComponent.h` |
| LOD, section biases, streaming coupling | `Landscape/Private/LandscapeRender.cpp` |
| Layer blend modes | `Landscape/Classes/Materials/MaterialExpressionLandscapeLayerBlend.h` |
| Landscape material expressions | `Landscape/Classes/Materials/` |
| Nanite landscape | `Landscape/Private/LandscapeNaniteComponent.cpp` |
| Grass output, grass map export | `Landscape/Classes/Materials/MaterialExpressionLandscapeGrassOutput.h`, `Landscape/Private/LandscapeGrassWeightExporter.cpp` |
| Grass varieties, instancing | `Landscape/Classes/LandscapeGrassType.h`, `Foliage/Private/InstancedGrass.cpp`, `Foliage/Private/FoliageISMActor.cpp` |

**[PAPER]**:

- Eric Heitz and Fabrice Neyret, [*High-Performance By-Example Noise using a Histogram-Preserving Blending Operator*](https://eheitzresearch.wordpress.com/722-2/), ACM SIGGRAPH/Eurographics HPG 2018 ([Inria](https://inria.hal.science/hal-01824773/), [ACM](https://dl.acm.org/doi/10.1145/3233304))
- [Unity's implementation writeup](https://unity.com/blog/engine-platform/procedural-stochastic-texturing-in-unity) and [Unity Grenoble demos](https://unity-grenoble.github.io/website/demo/2020/10/16/demo-histogram-preserving-blend-synthesis.html) — useful practical reading

**Observed on disk:** s&box `.terrain` assets under
`E:/SteamLibrary/steamapps/common/sbox/download/assets/` — compiled only, so
the existence of a Facepunch terrain system is evidence, its implementation is
not readable.

**Related notes:** [`world_streaming.md`](world_streaming.md) — the paging half;
[`voxel_terrain.md`](voxel_terrain.md) — triplanar in depth, clipmaps,
geomorphing, and the volume alternative; [`elite_dangerous.md`](elite_dangerous.md)
— planet-scale generated terrain and the point where terrain stops being
geometry; [`source2_rendering.md`](source2_rendering.md) — this renderer's
lighting and AO position.
