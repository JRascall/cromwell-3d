# Material detail — the techniques that make a surface read as high-fidelity

Beyond albedo, roughness and parallax: what else Unreal actually ships for
surface detail, what each is really doing, and which are worth taking. Read
from UE 5.7's own shaders and content.

Third in a set: [`surface_depth.md`](surface_depth.md) covers *apparent relief*
(parallax, POM, pixel depth offset); [`terrain_rendering.md`](terrain_rendering.md)
covers *large-area* detail (layer blending, anti-tiling). This note is the
per-surface shading layer between them.

## Sourcing

| Tag | Meaning |
|---|---|
| **[UE-SRC]** | Read from `C:/Program Files/Epic Games/UE_5.7/Engine/Shaders/`. Code and names are Epic's |
| **[UE-CONTENT]** | Ships with the engine; name and path verified on disk, contents are binary `.uasset` and **not** readable here |
| **[TPS]** | From Epic's own third-party-software notice, which names the licensed technique |
| **[inferred]** | My reasoning |

---

## 1. The reframe: most "detail" is detail you failed to lose

The instinct is to add signal — more layers, more maps, more samples. But the
largest single quality difference between a scene that reads as crisp and one
that reads as noisy is usually **not** added detail. It is **detail that was
present and got destroyed by undersampling**.

A normal map at a grazing angle, or on a distant object, has far more variation
than there are pixels to hold it. Point-sampling that variation per pixel gives
a different answer every frame as the camera moves, which is **specular
shimmer** — the crawling, sparkling, aliased look. TAA then smears it, and the
result is a scene that is simultaneously noisy *and* soft.

So §2 comes first, and it is the one I would recommend above everything else in
this note.

---

## 2. Specular anti-aliasing — the highest-value thing here

### 2.1 What Unreal actually does

```hlsl
float NormalCurvatureToRoughness(float3 WorldNormal, float3 dNdx, float3 dNdy)
{
    float x = dot(dNdx, dNdx);
    float y = dot(dNdy, dNdy);
    float CurvatureApprox = pow(max(x, y), View.NormalCurvatureToRoughnessScaleBias.z);
    return saturate(CurvatureApprox * View.NormalCurvatureToRoughnessScaleBias.x
                                    + View.NormalCurvatureToRoughnessScaleBias.y);
}

float NormalCurvatureToRoughness(float3 WorldNormal)
{
    return NormalCurvatureToRoughness(WorldNormal, ddx(WorldNormal), ddy(WorldNormal));
}
```

`Engine/Shaders/Private/MaterialTemplate.ush:4206-4217` **[UE-SRC]**

Five lines, and the idea behind them is worth stating plainly:

**Measure how fast the normal is changing across one pixel** — `ddx`/`ddy` of
the world normal — **and convert that into extra roughness.** A pixel whose
normal varies wildly is a pixel containing many sub-pixel orientations, and the
correct answer for "many orientations" *is* a wider specular lobe. So instead
of picking one normal and aliasing, you widen the lobe to represent the
distribution you could not sample.

`max(x, y)` takes the worse of the two screen axes, `pow(..., ScaleBias.z)`
shapes the response, and the scale/bias pair makes the whole thing tunable per
project from a view constant rather than per material.

This is the practical descendant of Toksvig / LEAN mapping: **roughness and
normal variance are the same quantity at different scales.** A rough surface is
one whose normals vary below the sampling rate. Once you see that, "convert
unresolved normal variance into roughness" stops being a hack and becomes the
correct filtering.

### 2.2 The subtlety worth stealing

```hlsl
// Curvature-to-roughness uses derivatives of the WorldVertexNormal, which is incompatible
// with centroid interpolation because the samples are not uniformly distributed. Therefore
// we use WorldVertexNormal_Center which is guaranteed to be center interpolated.
```

`MaterialTemplate.ush:4222-4224` **[UE-SRC]**

Screen-space derivatives assume the two samples in a quad are a known distance
apart. **Centroid interpolation moves sample positions inside partially-covered
pixels** — which is exactly what it is for — and that silently corrupts every
`ddx`/`ddy` taken from a centroid-interpolated value. Epic keep a separate
centre-interpolated normal for this purpose.

[inferred] The generalisation is worth carrying: **any use of `ddx`/`ddy` on an
interpolated value is only valid if you know that value's interpolation mode.**
That is a class of bug that produces subtle edge artefacts and is essentially
undebuggable by inspection.

### 2.3 Why this is first on the list

It costs a handful of ALU and no extra texture samples. It requires no new
content. It makes every existing material look better, most dramatically on
exactly the surfaces this project has a lot of — tiled, normal-mapped, viewed
at a fixed oblique angle where grazing incidence is permanent rather than
occasional.

---

## 3. Detail normals and how normals are combined

### 3.1 Detail texturing

`Engine/Content/Functions/Engine_MaterialFunctions01/Texturing/DetailTexturing.uasset`
**[UE-CONTENT]**.

The technique: a second, high-frequency normal (and often roughness) tiled at a
much smaller scale, blended over the base, usually faded by distance. It buys
close-range micro-structure that the base texture's resolution cannot hold,
without increasing the base texture's memory at all.

**Cost is one extra sample; the benefit is entirely at close range**, so it
should be distance-faded — which also avoids it becoming an aliasing source at
distance (§2).

### 3.2 Combining two normal maps is not addition

The engine ships `FlattenNormal`, `WorldAlignedNormal`, `WorldAlignedNormal2`,
`DeriveTangentBasis` and a `ReorientedNormalsDemo` **[UE-CONTENT]**.

That last name matters. Naively blending two tangent-space normals — adding the
XY and renormalising — is wrong in a way that flattens detail: it treats the
detail normal as if it were relative to flat, not relative to the base normal.
**Reoriented Normal Mapping** rotates the detail normal into the base normal's
frame, so a bump on a slope stays perpendicular to the slope.

[inferred, from the presence of a demo asset by that name — the graph is binary
and unread, but RNM is the standard technique the name refers to.] The visible
symptom of getting it wrong is detail that fades out on curved or steep parts
of a surface, which is precisely where you most wanted it.

---

## 4. Occlusion — and the half of it people skip

Ambient occlusion is usually wired to diffuse only. That is half the job.

**A crevice occludes reflections as well as ambient light.** A surface with
correct AO on diffuse and no occlusion on specular gets a characteristic look:
the cracks are dark, but they still shine, so the whole surface reads as
covered in a film. Specular occlusion — driven by AO, cavity, and the roughness
— is what makes deep detail look genuinely deep.

Unreal also ships **screen-space contact shadows**
(`Engine/Shaders/Private/ScreenSpaceShadows.usf`, referenced from the base pass
**[UE-SRC]**), which are the small-scale grounding cue that shadow maps are too
coarse to provide — the dark line where an object actually meets a surface.

[inferred] Ranked by value per cost, correct occlusion usage is second only to
§2, because Megascans-style content already ships the AO and cavity maps
([`surface_depth.md`](surface_depth.md) §2) and most projects only use half of
them.

---

## 5. Substrate — the layered material system

`Engine/Shaders/Private/Substrate/` is a substantial subsystem **[UE-SRC]**:

```
Substrate.ush                 SubstrateEvaluation.ush
SubstrateDeferredLighting.ush SubstrateForwardLighting.ush
SubstrateLegacyConversion.ush SubstrateMaterialClassification.usf
SubstrateDBuffer.usf          SubstrateExport.ush
Glint/
```

`SUBSTRATE_ENABLED` appears throughout `BasePassPixelShader.usf` **[UE-SRC]**,
and `SubstrateLegacyConversion.ush` exists to map old materials onto it — so
this is a replacement for the fixed shading-model system, not an addition
beside it.

The premise is that real surfaces are **layered** — lacquer over metal flake
over primer; wet mud over dry stone; dust over glass — and the old model of
"pick one shading model and one set of parameters" cannot express that. Substrate
composes **slabs** with physically-meaningful coverage and transmission between
them.

I have not studied its evaluation model in depth, so this section is a pointer
rather than an account. What is verifiable: it exists, it is pervasive, it has
a legacy conversion path, and it is the entry point for §6.

---

## 6. Glints — actual micro-facet sparkle

The most exotic thing in the list, and it is a licensed third-party technique.
From Epic's own TPS notice **[TPS]**:

```xml
<Name>Real-Time Geometric Glint</Name>
<Location>UE5/Main/Engine/Source/Runtime/Renderer/ThirdParty/Glint</Location>
<Function>I have converted their LUT that need to be sampled at runtime to a UE5
texture loaded on demand when needed. The glint technique is used in shaders by
our material Slab node to simulate glints.</Function>
<Eula>https://github.com/ASTex-ICube/aa_real_time_glint/blob/main/LICENSE</Eula>
```

`Engine/Shaders/Private/Substrate/Glint/real-time_geometric_glint.tps`

So: **a LUT-driven glint model from the ASTex/ICube research group, loaded on
demand and driven through Substrate's Slab node.**

What it solves: standard microfacet BRDFs assume *infinitely many* microfacets
per pixel, so they produce a smooth highlight. Real snow, car flake, glitter and
some metals have *countably few* facets per pixel, and the correct appearance is
discrete sparkle that scintillates as the camera moves. Statistically averaging
that away is exactly wrong, and no amount of normal-map detail reproduces it,
because the effect is about the *distribution* being sparse rather than about
resolution.

[inferred] Worth knowing about, not worth building. It is narrow (snow, glitter,
flake paint), it needs a LUT and a specialised evaluation, and it is licensed
from a research group. File under "know the name when you need it".

---

## 7. The shading models that are themselves detail

Briefly, because these are well documented and the point is only that they
exist and are cheap-ish once the deferred pipeline supports them:

- **Anisotropy** — UE has a dedicated pass (`AnisotropyPassShader.usf`
  **[UE-SRC]**). Brushed metal, hair, satin. The highlight stretches along a
  tangent direction instead of being circular, and it is the difference between
  "grey metal" and "machined aluminium".
- **Clear coat** — a second specular lobe over the base. Car paint, lacquer,
  varnished wood.
- **Subsurface / two-sided foliage** — light transmission. The single biggest
  factor in vegetation looking alive rather than like painted cardboard.

Each is a shading model rather than a texture trick, so the cost is in the
G-buffer and the lighting pass, not the material.

---

## 8. The function library is a catalogue worth browsing

`Engine/Content/Functions/Engine_MaterialFunctions01/` is organised by concern
**[UE-CONTENT]**:

```
AlphaBlend  Chromakeying  Coordinates  Cubemaps  Debug  Decal  Density
Gradient    ImageAdjustment  Landscape  Lighting  Math   Opacity
Reflections Shading       SpeedTree    Texturing Units  Vectors  Volumetrics
```

and `Texturing/` alone contains `DetailTexturing`, `BakedDisplacement`,
`MF_VectorDisplacement`, `SlopeMask`, `ComputeMipLevel`, `PointSampledUVs`,
`LocalAlignedTexture`, `BrickAndTileUVs`, `SpiralBlur-Texture`,
`ParallaxOcclusionMapping` and the rest.

[inferred] The list itself is the useful artefact: it is Epic's own answer to
"what does a material author actually need", refined over a decade. Browsing
the *names* is a cheap way to find techniques you did not know to look for —
`SlopeMask` and `ComputeMipLevel` in particular are the kind of small utility
that everyone reinvents.

---

## 9. Ranked for `cromwell`

Given a tile-based game with a fixed oblique camera, an existing normal-mapped
lit pipeline, SSAO and a shadow map:

| Rank | Technique | Why | Cost |
|---|---|---|---|
| **1** | **Specular AA via normal-curvature-to-roughness** (§2) | Fixes shimmer on every existing material. Fixed oblique camera means permanent grazing angles — this project is an unusually strong case | ~10 ALU, no samples, no content |
| **2** | **Specular occlusion, not just diffuse AO** (§4) | The maps are usually already there; using half of them is why detail looks filmed-over | ~free if AO exists |
| **3** | **Detail normals, distance-faded** (§3.1) | Close-range micro-structure without bigger textures | 1 sample, near only |
| **4** | **Reoriented normal blending** (§3.2) | Only matters once §3 exists, but getting it wrong wastes §3 | ~free |
| **5** | **Contact shadows** (§4) | Grounding cue a shadow map cannot give; units meeting tiles is this game's most-viewed contact | a screen-space trace |
| **6** | **`BumpOffset`** ([`surface_depth.md`](surface_depth.md) §3.2) | Cheap parallax, well suited to a stable view angle | 1 sample + ALU |
| — | Anisotropy / clear coat / subsurface (§7) | Only if the content wants them; each is a G-buffer cost | shading model work |
| — | Substrate (§5) | Architectural, and this renderer is forward-clustered ([`source2_rendering.md`](source2_rendering.md)) — a poor fit | large |
| — | Glints (§6) | Narrow, licensed, LUT-driven | large |

**The shape of the recommendation:** the top four are all cheap, all
content-free, and all about *preserving or correctly using signal you already
have* rather than adding more. That is not a coincidence — it is the same
ordering as CLAUDE.md's rule that algorithmic wins precede constant-factor ones,
applied to image quality. **Fix what you are losing before adding more to
lose.**

---

## 10. What I did not verify

- Substrate's evaluation model (§5) — pointer only.
- Whether UE has a dedicated specular-occlusion function, or whether it is left
  to material authors. My grep of `ShadingModels.ush` / `DeferredLightingCommon.ush`
  found no `GetSpecularOcclusion`, so **§4's specular-occlusion argument is
  stated as a principle, not as a description of a UE feature.**
- The `DetailTexturing` and `ReorientedNormalsDemo` graphs — binary `.uasset`.
- Contact shadow quality/cost trade-offs; `ScreenSpaceShadows.usf` was located,
  not read.

---

## Sources

**[UE-SRC]** — Unreal Engine 5.7, `C:/Program Files/Epic Games/UE_5.7/Engine/Shaders/Private/`:

| Area | Path |
|---|---|
| Normal-curvature-to-roughness, centroid caveat | `MaterialTemplate.ush:4206-4240` |
| Substrate | `Substrate/` (13 files) |
| Contact shadows | `ScreenSpaceShadows.usf`, referenced from `BasePassPixelShader.usf` |
| Anisotropy pass | `AnisotropyPassShader.usf` |

**[TPS]** — `Shaders/Private/Substrate/Glint/real-time_geometric_glint.tps`,
pointing to [ASTex-ICube/aa_real_time_glint](https://github.com/ASTex-ICube/aa_real_time_glint).

**[UE-CONTENT]** — `Engine/Content/Functions/Engine_MaterialFunctions01/`,
notably `Texturing/` (`DetailTexturing`, `FlattenNormal`, `WorldAlignedNormal`,
`SlopeMask`, `BakedDisplacement`, `MF_VectorDisplacement`, `ComputeMipLevel`)
and `Engine_MaterialFunctions02/ExampleContent/ReorientedNormalsDemo`.

**Related notes:** [`surface_depth.md`](surface_depth.md) — parallax, POM and
pixel depth offset; [`terrain_rendering.md`](terrain_rendering.md) — layer
blending and anti-tiling, where §2's aliasing argument also applies;
[`source2_rendering.md`](source2_rendering.md) — this renderer's shading and AO
position.
