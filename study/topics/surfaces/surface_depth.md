# Apparent depth without geometry — parallax, POM, pixel depth offset

Why a Megascans surface in Unreal looks like it has real relief when it is a
flat polygon, which nodes do it, what each costs, and the one test that tells
you whether you are looking at a trick or at geometry.

Companion to [`terrain_rendering.md`](../world/terrain_rendering.md) — the same problem
one level up, where the question is layers and tiling rather than a single
surface.

## Sourcing

| Tag | Meaning |
|---|---|
| **[UE-SRC]** | Read from `C:/Program Files/Epic Games/UE_5.7/Engine/`. Names, constants and maths are Epic's |
| **[UE-CONTENT]** | An asset that ships with the engine — existence and name verified on disk, contents are binary `.uasset` and **not** readable |
| **[QUIXEL]** | Quixel's own documentation |
| **[inferred]** | My reasoning |

---

## 1. The short answer

**It is not tessellation** — or it was not, until recently. What makes a
Megascans surface read as deep is four separate things stacked, and only the
last one is geometry:

| # | Contribution | Where the depth "lives" |
|---|---|---|
| 1 | **Measured maps** — normal, AO, cavity, displacement scanned from a real object | The data, not the technique |
| 2 | **Parallax** — `BumpOffset`, or Parallax Occlusion Mapping | UV coordinates |
| 3 | **Pixel Depth Offset** — the material writes the depth buffer | The depth buffer |
| 4 | **Nanite tessellation + displacement** (UE 5.4+) | Actual triangles |

Most of the "wow" is **#1**, and that is the part people misattribute to the
shader. The rest is a ladder of increasingly expensive illusions, plus a real
one at the top.

---

## 2. Why photogrammetry data looks different

Before any shader trick: a scanned surface ships maps that a hand-authored one
usually does not, and they are *measured* rather than inferred.

Quixel's assets are **"scanned from real-world objects using purpose-built
hardware"**, and the surface set includes albedo, normal, **displacement**,
**occlusion** and roughness **[QUIXEL]**, with cavity and curvature in the fuller
sets.

Three of those do the heavy lifting, and it is worth being precise about why:

- **The normal map is measured, not painted.** A hand-authored normal map is
  usually derived from a height map, which means it only contains what the
  height map contained. A scanned normal contains real micro-structure with
  correct, irregular, non-idealised detail — and crucially the *statistics* of
  real surfaces, which the eye is tuned to.
- **The AO/cavity maps are baked from real geometry.** This is the big one.
  Ambient occlusion darkens crevices *correctly*, and correct contact darkening
  is one of the strongest depth cues the visual system has. A flat polygon with
  a real AO map already looks deep before any parallax is applied.
- **The albedo is de-lit.** Scanning captures lighting; Quixel's pipeline
  removes it. If it did not, the baked lighting would fight the runtime
  lighting and the surface would look flat and dirty regardless of technique.

[inferred] The practical consequence, and it is the one worth internalising:
**if the maps are wrong, no amount of parallax will save the surface; if the
maps are right, quite a lot of depth appears with no parallax at all.** Effort
spent on the technique before the data is misallocated.

---

## 3. The ladder of tricks

### 3.1 Normal mapping — lighting only

A normal map changes only the *shading normal*. It costs one sample and no
geometry, and it fails in one specific, diagnostic way: **at grazing angles the
surface is visibly flat**, because nothing has moved. Bricks stay flush with
their mortar; you can see the polygon.

### 3.2 `BumpOffset` — parallax mapping in one instruction

UE's cheap parallax node. Its inputs, with Epic's own comments **[UE-SRC]**:

```cpp
FExpressionInput Coordinate;
FExpressionInput Height;
FExpressionInput HeightRatioInput;
float HeightRatio    = 0.05f; // Perceived height as a fraction of width.
float ReferencePlane = 0.5f;  // Height at which no offset is applied.
```

`Engine/Source/Runtime/Engine/Public/Materials/MaterialExpressionBumpOffset.h` **[UE-SRC]**

And the maths, recovered from the compiler **[UE-SRC]**
(`MaterialExpressions.cpp:5942-5962`), which builds:

```
UV' = UV + CameraVector_TangentSpace.xy * HeightRatio * (Height - ReferencePlane)
```

Three things are worth reading out of that one line:

- **It shifts UVs along the tangent-space view direction**, proportional to
  height. Look at a surface from an angle and the tall bits appear to shift
  against the low bits — motion parallax, which is a genuine depth cue.
- **`ReferencePlane = 0.5` means the offset is signed about mid-height.**
  Heights above 0.5 shift one way, below the other, so the surface pivots about
  its middle rather than sliding as a whole. Get this wrong and the texture
  visibly swims.
- **There is no divide by `CameraVector.z`.** The textbook formulation divides
  by the view vector's tangent-space Z, which blows up at grazing angles. Epic
  use the *offset-limiting* variant, which is stabler and cheaper at the cost of
  under-shifting when viewed edge-on. [inferred, from the absence of the divide
   — but this is a well-known variant and the omission is not an accident.]

Cost: **one extra texture sample and a handful of ALU.** Limitation: it is a
single step, so it is only correct for gentle height fields. Steep features
smear, and it cannot occlude — a tall brick never hides the mortar behind it.

### 3.3 Parallax Occlusion Mapping — the node you are thinking of

Unreal ships it as a **material function**, not a node **[UE-CONTENT]**:

```
Engine/Content/Functions/Engine_MaterialFunctions01/Texturing/
    ParallaxOcclusionMapping.uasset
    ParallaxOcclusionMapping_BoundedUVs.uasset
    Parallax_For_Bomb.uasset
```

(These are binary `.uasset`, so the graph inside is not readable here — the
names and locations are verified, the implementation is not.)

POM replaces the single offset with a **ray march through the height field in
tangent space**: step along the view ray, sample the height at each step, and
stop where the ray first goes below the surface. That gives:

- **Correct occlusion** — near features genuinely hide far ones.
- **Correct silhouettes *within* the texture** — a hole looks like a hole.
- Optionally **self-shadowing**, by marching a second ray toward the light.

Cost is the honest part: **N samples per pixel**, typically 8-32, with more
needed at grazing angles precisely where it matters most. It is the single most
expensive thing in a typical Megascans material, and it is why POM is usually
gated to close range and faded out with distance.

`_BoundedUVs` is the variant that clamps the marched UVs so the march cannot
wander outside the intended tile — which is what stops POM bleeding across a
texture atlas or a decal's bounds. [inferred, from the name; the graph is not
readable.]

`Parallax_For_Bomb` is parallax adapted for texture bombing — see
[`terrain_rendering.md`](../world/terrain_rendering.md) §5.4, and note that the two
techniques interact: stochastic sampling and parallax both need consistent UVs
and fight each other naively.

### 3.4 Pixel Depth Offset — where the illusion stops being an illusion

This is the piece most people miss, and it is what separates "the texture looks
bumpy" from "the object sits in the world correctly".

POM is a *UV* trick — the depth buffer still contains a flat polygon. So a
POM'd surface intersecting another object shows a straight, flat intersection
line that betrays the whole thing, and shadows fall as if it were flat.

`PixelDepthOffset` fixes that by making the material **write depth**
**[UE-SRC]**:

```cpp
#if OUTPUT_PIXEL_DEPTH_OFFSET
    ApplyPixelDepthOffsetForBasePass(MaterialParameters, PixelMaterialInputs, BasePassInterpolants, Out.Depth);
#endif
```

`Engine/Shaders/Private/BasePassPixelShader.usf:962-968` **[UE-SRC]**

Two implementation details are worth stealing.

**It can only push away, never pull toward.** From `MaterialTemplate.ush:4750+`
**[UE-SRC]**:

```cpp
DeviceDepth = max(SceneDepth + PixelDepthOffset, SceneDepth);
```

Clamped against the original. That is why POM in Unreal makes *holes* and not
*bumps sticking out* — you can recess a surface into its polygon, never extrude
it beyond. The polygon remains the outer hull.

**It uses `SV_DepthLessEqual`, not `SV_Depth`.** Epic's comment explains the
consequence in detail — with a depth output that can only move away from the
camera, the hardware can keep **early-Z rejection**, which it must disable for
an unconstrained depth write. The same comment documents a real PS4 bug where
`RE_Z` caused the depth test to run twice and produced z-fighting when the
computed depth landed *slightly* above the original — hence the clamp is
needed *"even if PixelDepthOffset is 0.0f"* **[UE-SRC]**.

And it is not only the base pass: `PixelDepthOffset` appears in
`DepthOnlyPixelShader.usf`, `VelocityShader.usf`, `AnisotropyPassShader.usf`
and the mobile base pass **[UE-SRC]**. So the offset participates in **shadow
depth, motion vectors and prepass** — which is what makes shadows and temporal
AA agree with the illusion instead of revealing it.

[inferred] The general lesson: **an illusion that only exists in one pass will
be exposed by every other pass.** Getting POM to look right is 20% the march
and 80% making depth, shadows and velocity agree with it.

### 3.5 Nanite tessellation and displacement — the real thing

Since UE 5.4, the answer to "is it actually tessellation?" can be **yes**.
`Engine/Shaders/Private/Nanite/NaniteTessellation.ush` (358 lines) **[UE-SRC]**
contains:

```hlsl
ByteAddressBuffer TessellationTable_Offsets;
ByteAddressBuffer TessellationTable_VertsAndIndexes;

float CalcDisplacementLowTessDistance(...)
{
    const float LowTessSize   = PrimitiveData.MaterialDisplacementFadeOutSize;
    const float MaxDisplacement = GetAbsMaxMaterialDisplacement(PrimitiveData);
    return (MaxDisplacement * InstanceData.NonUniformScale.w * NaniteView.LODScale) / LowTessSize;
}
```

Two design decisions are visible in that alone:

- **Tessellation patterns are table-driven**, not computed — a precomputed
  table of vertex and index layouts indexed by a pattern code, so the GPU looks
  up a subdivision rather than deriving one.
- **Displacement fades out with distance**, via
  `MaterialDisplacementFadeOutSize` and a maximum-displacement bound. The
  maximum matters twice: for the fade, and because a displaced surface's bounds
  must be conservatively enlarged or it will be culled incorrectly. [inferred
  for the second, but `GetAbsMaxMaterialDisplacement` existing at all implies a
  bounds consumer.]

**This genuinely changes the silhouette**, which no amount of POM can do.

---

## 4. The test that tells you which one you are looking at

**Look at the silhouette.**

| Technique | Interior relief | Silhouette | Depth buffer |
|---|---|---|---|
| Normal map | shading only | flat | flat |
| BumpOffset | shifts, no occlusion | flat | flat |
| POM | correct occlusion | **flat** | flat |
| POM + Pixel Depth Offset | correct occlusion | **flat** | recessed |
| Nanite displacement | real | **real** | real |

Every trick short of real displacement leaves the **outline of the polygon
unchanged**. Put a POM'd brick wall against the sky and the edge is a perfectly
straight line no matter how deep the bricks look. That is the giveaway, and it
is also why POM is usually applied to *floors and walls seen from the front* —
surfaces whose silhouette is not against anything.

[inferred] Which is the practical guidance: **choose the technique by whether
the silhouette is visible**, not by how much depth you want. A ground plane can
use POM forever; a boulder against the horizon needs geometry.

---

## 5. What `cromwell` should take

This renderer already has a normal-mapped, lit pipeline, and the note's
findings scale down cleanly:

1. **Spend on the maps before the technique** (§2). A correct, measured AO or
   cavity map buys more apparent depth per unit of effort than any shader here,
   and it costs nothing at runtime beyond a sample this renderer already takes.
   If tile materials ever get authored properly, AO is the channel to get right
   first.

2. **`BumpOffset` is nearly free and should be the default ambition** (§3.2).
   One sample, a few ALU, and one line of maths that is now written down above.
   For a tile-based game viewed from a fixed-ish elevated angle, single-step
   parallax is well matched — the view angle is stable, so the artefacts that
   punish it at grazing angles largely do not arise.

3. **If POM is ever added, budget Pixel Depth Offset with it, not after**
   (§3.4). POM without depth offset looks wrong the moment anything intersects
   the surface, and this project has units standing on tiles — an intersection
   at every unit's feet. The clamp-to-push-away and `SV_DepthLessEqual`
   behaviour is the part to copy exactly, because it preserves early-Z.

4. **Remember the illusion must hold in every pass** (§3.4). This renderer has a
   shadow map and an SSAO pass; a depth trick that only exists in the lit pass
   will be contradicted by both. That is the same discipline as CLAUDE.md's
   derived-cache rule — one source of truth, and every consumer reads it.

5. **Judge by silhouette** (§4). Given the fixed camera elevation, most tile
   surfaces are seen face-on and never silhouetted, which is precisely the case
   where the cheap tricks are indistinguishable from the expensive ones. That is
   a genuine architectural advantage of this project's camera and worth
   exploiting deliberately.

**And the honest summary of the original question:** Megascans looks deep
mostly because Quixel measured a real rock, and partly because Unreal marches a
ray through the height field and then tells the depth buffer about it. The
tessellation you thought you were seeing was, until recently, not there at all —
and now that it can be, it is the one technique that changes the outline.

---

## 6. Open threads

Stated rather than guessed:

- **The POM material function's actual implementation** — step count, adaptive
  stepping by view angle, the refinement step, and how self-shadowing is done.
  The `.uasset` is binary. Reading it needs the editor, or the compiled shader.
- **Horizon mapping** — a related technique for self-shadowing height fields
  that came up while researching this and is not covered here.
- **Whether Nanite displacement is practical at this project's scale** — the
  fade-out machinery implies real cost, and I have not measured it.

---

## Sources

**[UE-SRC]** — Unreal Engine 5.7, `C:/Program Files/Epic Games/UE_5.7/Engine/`:

| Area | Path |
|---|---|
| BumpOffset node and its defaults | `Source/Runtime/Engine/Public/Materials/MaterialExpressionBumpOffset.h` |
| BumpOffset maths | `Source/Runtime/Engine/Private/Materials/MaterialExpressions.cpp:5942` |
| Pixel depth offset computation and the clamp | `Shaders/Private/MaterialTemplate.ush:4750` |
| PDO in the base pass | `Shaders/Private/BasePassPixelShader.usf:767, 962` |
| PDO in other passes | `Shaders/Private/{DepthOnly,Velocity,Anisotropy,MobileBasePass}*.usf` |
| Nanite tessellation and displacement | `Shaders/Private/Nanite/NaniteTessellation.ush` |

**[UE-CONTENT]** — verified present, contents not readable:
`Engine/Content/Functions/Engine_MaterialFunctions01/Texturing/ParallaxOcclusionMapping.uasset`,
`ParallaxOcclusionMapping_BoundedUVs.uasset`, `Parallax_For_Bomb.uasset`.

**[QUIXEL]** — [Quixel Megascans](https://quixel.com/megascans/) and
[Quixel Mixer texture setup docs](https://docs.quixel.com/mixer/1/en/topic/advanced-texture-setup-for-exports.html)
for the shipped channel set.

**Related notes:** [`terrain_rendering.md`](../world/terrain_rendering.md) §4.2 for
height-blending, which is the same height data used for a different purpose, and
§5 for how parallax interacts with anti-tiling;
[`source2_rendering.md`](../../games/valve/source2_rendering.md) for this renderer's material and
AO position.
