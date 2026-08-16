# Source 2 rendering — reference notes

Working notes on how Valve's Source 2 gets its look, and how far this renderer
has followed it. Kept because the answers keep mattering: most of the visual
gap between a hobby renderer and a shipped one is architectural, not shader
tricks, and the architecture is easy to forget between sessions.

Everything marked **[VDC]** is from Valve's own documentation (linked at the
bottom). **[VRF]** is from ValveResourceFormat's reimplementation of Source 2's
shaders — reverse-engineered from the compiled originals, so accurate in
structure and parameter naming but not Valve's source. Everything marked
**[inferred]** is our reading, not Valve's word.

**Which Source 2 game is the reference.** This project is desktop, aiming at
realistic graphics. **CS2 is the primary reference**; Half-Life: Alyx and
SteamVR Home are secondary, because VR rendering trades image quality for the
framerate and stereo cost a headset demands. Alyx is still worth citing where it
is the only or the clearer source — and §12 flags one case where its answer is
the *better* one — but a VR-driven compromise is never the target. Where Valve
documents both, take the desktop answer: the `Csgo *` shader family over
`VR Standard` / `VR Complex`, `env_cubemap_fog` over froxel volumetrics.

> **Status, 2026-08-09.** The lightmap bake described in the second half of this
> document is **built, measured, and switched off**. `useBakedSun_` defaults to
> `false`; the bake is a debug toggle. Everything ships on the dynamic shadow
> map. Read [The bake, and why it is shelved](#the-bake-and-why-it-is-shelved)
> before acting on anything in the measurement tables.
>
> Indirect light is settled on a **realtime probe grid**, for the destructible
> map — see §11. That is the *indirect* term only; direct sun shadows remain a
> separate unsolved problem, in §9.

---

## 1. The one thing that matters most

**Source 2's lighting is mostly precomputed, and its best-looking shadows are
not shadow maps at all.**

Global illumination is **path traced offline and baked** — into lightmaps for
static surfaces, and into light probe volumes for dynamic objects. VRAD3 does
this at map compile. **[VDC]**

That single fact explains most of the quality difference. Soft indirect bounce,
colour bleed, contact darkening and — critically — *shadow softness* are all
resolved by an offline path tracer that has no texel grid, no depth bias and no
aliasing to fight.

> Mesh entities in Source 2 are **never** lightmapped; they need light probes
> built to be lit correctly. **[VDC]**

**The catch for us [inferred]:** Half-Life: Alyx's world is *static*. Ours is
not — a grenade rewrites the lattice mid-turn. Source 2 has no dynamic
technique that matches its own bake, because it never needed one. So there is
no "what does Alyx do here" answer for destructible world shadows. What follows
is therefore split into two questions: how to close the gap on shadows without
a bake (§9), and what Source 2 has that we lack *regardless* of how shadows are
solved (§10) — which, with the bake off, is now the larger half.

## 2. Shadows: two systems, not one

| | static geometry | dynamic objects |
|---|---|---|
| method | **baked into the lightmap** by VRAD3 | cascaded shadow maps |
| softness | area light — `SunSpreadAngle` / Light Source Radius | filter kernel |
| contact hardening | yes, genuinely | no (approximated at best) |
| aliasing | none — there is no depth buffer | texel staircase, bias, acne |

Valve's docs are explicit that the sun's angular extent for soft shadows
"**only affects baked lighting**". **[VDC]** Contact hardening on static
geometry comes from the same area-light model. Spot lights under a 90° cone get
cascaded shadow maps for the dynamic case. **[VDC]**

The *split itself* is the transferable idea, and it survives the bake being
shelved — see §9, which applies it to two shadow maps instead of a bake and a
shadow map.

## 3. Materials

- Metal/rough PBR, Cook-Torrance. **[VDC]**
- **MRAO**: metalness R, roughness G, ambient occlusion B, one texture. **[VDC]**
  (Note glTF's ORM is a *different* order — occlusion R, metalness B.)
- Valve keeps a separate greyscale reflectance map so metalness does not drive
  the specular *amount*. **[VDC]**

## 4. Reflections

Parallax-corrected cubemaps, higher resolution than Source 1. **[VDC]** This is
what stops PBR surfaces reading as plastic — every surface gets a plausible
environment term rather than a constant ambient.

## 5. Atmosphere

Source 2 ships **three** fog systems and they coexist in one shader — see §13
for the implementation. In order of application:

1. **Volumetric (froxel) fog** — a frustum-shaped 3D texture, sampled per
   pixel. Alyx is the title associated with it. **[VDC]**
2. **Gradient fog** — view depth crossed with world height.
3. **Cubemap fog** (`env_cubemap_fog`) — global MIP fog sampled from a cubemap
   by view direction, so distant fog takes the sky's colour per
   direction. **[VDC]**

An earlier revision of this document said CS2 "dropped volumetrics in favour of
`env_cubemap_fog`". That is wrong as stated: the CS2 shader carries all three
paths behind `g_bFogTypeEnabled`, including the full froxel sampler. **[VRF]**
What is true is that `env_cubemap_fog` is the *level-design* choice for distance
fog on CS2 maps. The volumetric machinery is still there, and §13/§14 both
depend on that being the case.

## 6. Tonemapping

Newer CS2 maps use the **Hable (U2/HLVR)** filmic operator. **[VDC]**

## 7. Renderer architecture

Forward for CS2 and Alyx; deferred for Dota 2 and Deadlock. **[VDC]**

---

## 8. Where this renderer actually stands

Verified against the source, not against the previous revision of this
document.

### Implemented

- Linear HDR pipeline, RGBA16F scene target, Hable tonemap, 2× supersampled
  and resolved by the tonemap blit. Nothing upstream of `tonemap.fs.glsl` knows
  what a display is.
- Cook-Torrance GGX with Smith height-correlated visibility, metal/rough,
  MRAO **and** glTF ORM (swizzled per material), tangent-space normal maps.
- Forward renderer — matching CS2 and Alyx.
- Analytic sky feeding both the backdrop and a two-lobe hemisphere ambient, so
  what you see behind the geometry is what the geometry is lit by.
- One directional sun; colour, warmth and intensity derived from elevation.
- Single orthographic shadow map, 4096², R8 colour plane, 12-tap rotated
  Poisson PCF with compare-then-interpolate bilinear taps and normal-offset
  bias.
- SSAO from a depth+normal prepass, 24-sample hemisphere kernel, multiplied
  into ambient only.
- Named material library, texture sets with 1×1 fallbacks, per-material
  batching, model loading with tangent generation, trilinear + 8× anisotropic.
- An octave-summed HDR glow chain — but see §10.5, it only serves the ribbon.

### Not implemented

| # | Missing | Consequence in frame |
|---|---|---|
| 1 | Indirect light of any kind | A sealed room receives the same sky irradiance as open ground. No colour bleed, no directional ambient. |
| 2 | Light probes | Units and props lit identically indoors and out. |
| 3 | Local lights (point / spot) | None exist. `BlastFlashes` is an overlay, not a light. |
| 4 | Environment cubemaps | All reflections are the analytic sky gradient. |
| 5 | Fog / volumetrics | No aerial perspective, no light shafts. |
| 6 | Scene bloom | The sun disc at 45× radiance does not bleed. |
| 7 | Emissive materials | No map, no factor, nowhere in the material system. |
| 8 | Decals, detail maps, blend layers | Surfaces read procedurally clean; visible tiling. |
| 9 | Output dither | An 8-bit sky gradient over most of the frame will band. |

Deliberately **not** pursued, and why:

- **Cascaded shadow maps** — cascades concentrate texels where a view frustum
  needs them. This board is bounded at 24×24; a single ortho over the whole
  lattice is already ~110 texels per tile.
- **TAA** — 2× supersampling is cleaner here and there are no motion vectors to
  build on. CS2 and Alyx use MSAA anyway.
- **Deferred shading** — Alyx and CS2 are forward too.

---

## The bake, and why it is shelved

`SunBaker` path-traces direct sun visibility per surface texel against the tile
grid, packs it into an atlas, and the lit shader reads it in place of the shadow
map for static lattice faces. It works, it is measured below, and
**`useBakedSun_` is `false`** — it ships off, on a debug toggle.

### The verdict, in play

Judged by eye in the running game: quality too low, edges inaccurate, and it
does not cope with a map that changes.

That verdict **disagrees with the measurements below**, which report the
staircase gone at 64 texels/face and zero stale texels after destruction. The
discrepancy is not explained, and explaining it is a prerequisite for ever
reviving this. Three candidates, all **[inferred]**:

1. **Only cell faces are baked.** Props, crates, ramp treads, ladders and
   blocked mass have no patch and stay on the shadow map. A frame therefore
   mixes baked and shadow-mapped shadows with different softness, different
   bias behaviour and different edge character — which reads as *inconsistent*
   whatever the quality of either half. Source 2 has the same split but hides
   it behind an area-light bake whose softness the shadow map is tuned to
   match; ours are not matched.
2. **The five-face-per-cell model cannot represent sub-cell geometry.** A patch
   is a whole cell face. Parapets, kerbs, sunken roads and ledges sit at
   offsets the addressing scheme approximates — `bakedSunVisibility()` already
   carries a special rounding rule for exactly this. Anything it fails to
   attribute silently falls back to the shadow map, mid-surface.
3. **Re-bake cost is not uniformly interactive.** 36 ms at the real grenade
   radius is fine; the stress radii reach 1.6 s, which is a visible hitch.

### What is worth keeping from it

The direct-sun bake is the *least* forgiving thing to precompute — it is high
frequency, and it must be fresh the instant a wall falls. Those are precisely
the two demands that sank it.

An **ambient / sky-visibility** bake inverts both: it is low frequency, so
16 texels/face is ample where sun needed 64, and a value that is one second
stale after a grenade is invisible. The `RayCaster`, the patch list, the atlas
packing and the index texture are all reusable as they stand. If any part of
this machinery comes back, that is the part — and per §10 it is also the single
largest remaining gap.

### Measurements — retained, but read the verdict above first

`--bake-benchmark` runs `SunBaker` over the demo map headlessly. Single
threaded, direct sun only, default sun (azimuth 125, elevation 48, angular
radius 0.03 rad):

| texels/face | rays/texel | full map | after one grenade |
|---|---|---|---|
| 8 × 8   | 8  | 86 ms  | **2.0 ms** |
| 16 × 16 | 16 | 674 ms | **16.1 ms** |
| 32 × 32 | 16 | 2.7 s  | **62 ms** |

The demo map has **1120 patches** (cell faces carrying a lightmap). One grenade
touches around 50 of them — the blast plus its shadow shaft. These figures are
~17× faster than the first measurement, from clamping each ray to where it
leaves the lattice instead of walking a fixed 200 units.

Threaded, at the shipping settings of 64 texels/tile face and 8 rays: the whole
map bakes in **~840 ms** across every core — 4.6M texels, 37M rays, ~43 Mray/s.
Threading splits the patch list; patches write to disjoint texel ranges, so
there is no locking, and each worker builds its own `RayCaster`.

**Staleness under destruction.** Destruction only ever removes occluders, so
every changed texel must get *brighter*; a texel that darkened would be a stale
value the affected region failed to reach. Compared by the stable `(cell, face)`
key, because patch slots move when geometry changes.

| blast | radius | edits | re-baked texels | time | brighter | darker |
|---|---|---|---|---|---|---|
| (11,12) storey 1 | 2.2 | 30 | 5 888 | 36 ms | 1 | **0** |
| (10,11) storey 1 | 3.0 | 36 | 10 496 | 105 ms | 4 | **0** |
| (9,13) storey 1 | 4.0 | 57 | 15 872 | 108 ms | 151 | **0** |
| (12,10) storey 1 | 5.0 | 57 | 27 136 | 1 068 ms | 405 | **0** |
| (11,12) storey 2 | 4.0 | 85 | 24 576 | 1 136 ms | 6 646 | **0** |
| (13,14) storey 2 | 5.0 | 85 | 34 560 | 1 593 ms | 5 246 | **0** |

Zero stale texels at every site. Cost grows faster than area because blasts high
up have longer shadow shafts and their rays are less likely to be blocked.

**Resolution is the whole ballgame.** The first version shipped at 16
texels/face and looked *markedly worse than the shadow map it replaced*. A 4096
shadow map over this lattice's ~37.6-unit ortho is **~110 texels per tile**;
sixteen was seven times coarser. Anything that lowers the bake's resolution must
be checked against the **shadow map**, not against the previous bake.

**Packed atlas plus an index texture.** Patches pack densely, patch *n* to slot
*n*, and a small index texture maps (cell, face) to a slot. The shader still
derives cell and face from world position and normal, so there is no second UV
channel and nothing for the geometry emitter to assign. Only 1120 of 25920
possible patches exist on this map, so a world-shaped atlas wasted twenty-three
parts in twenty-four — that waste was what capped the resolution. Same memory,
four times the linear resolution. The price is that atlas neighbours are not
world neighbours, so bilinear must clamp to the patch's inner half texel.

**Sunlight is not line of sight.** `RayCaster` takes `RayRules`. XCOM's LOS rule
is that half cover never blocks — you shoot over a sandbag wall, that being the
entire point of low cover. Sunlight does not care: a sandbag wall is opaque.
Same traversal, one predicate, selected per caster. `RayRules::Sight` is the
default and LOS results are unchanged (319 visible cells from (7,10,0)).

---

## 9. Closing the shadow gap without a bake

Three of the complaints against the current image are **defects in the dynamic
path**, not missing Source 2 features. They are worth fixing before anything
architectural, because they are small and they change what the remaining
problem even looks like.

### 9.1 The projection refits every frame, so the texel snap cannot work

`Application::shadowFocus()` builds an **AABB** of the frustum corners clipped
to world bounds and takes `radius` from its diagonal. An AABB is not
rotation-invariant: orbit the camera and that radius changes continuously.
`SunLight::shadowProjectionForSphere()` then derives
`worldTexel = radius * 2 / 4096` from it and quantises `lightCentre` to that
grid.

Snapping to a grid whose **cell size changes every frame** stabilises nothing.
The snap is in the right space — the comment is correct about that — but the
spacing is not constant, so shadow edges crawl and swim under any camera
motion. No filter touches this; it is not an aliasing artifact.

**Fix.** Take the radius from the frustum's bounding **sphere**, which is
rotation-invariant by construction, and **quantise the radius** to discrete
steps so `worldTexel` is piecewise constant. Then the existing snap holds.

Smallest useful experiment, and worth doing first: lock the projection to the
whole board and see how much of the "inaccurate" complaint is crawl. On a
bounded 24×24 lattice a fixed full-map ortho is ~110 texels/tile with nothing to
snap and nothing to crawl. It gives up resolution when zoomed in, which is what
the radius quantisation above buys back.

### 9.2 The penumbra width changes with zoom

`kFilterRadius = 2.0` in `pbr.fs.glsl` is two texels of a *variable* world size.
Zoom in and shadows sharpen; zoom out and they soften, with nothing in the world
having changed. Express the filter radius in **world units** and convert to
texels at the point of use.

### 9.3 Every caster is redrawn every frame to reproduce an identical image

The board is static except when a grenade goes off, and the full shadow pass
runs 60× a second regardless.

### 9.4 The architectural move: Source 2's split, applied to two shadow maps

Valve's static/dynamic split does not require the static half to be a bake.

| | map A — static lattice | map B — dynamic |
|---|---|---|
| contents | the tile lattice | units, moving props |
| projection | **board-locked**, never refits | tightly focused on what moves |
| redrawn | on destruction or iso-level change only | every frame |
| resolution | 4096 over the whole board ≈ 110 texels/tile | 2048 over a few metres — far denser |

Shade with `min(A, B)`.

What this buys, against the two failed approaches:

- **Correctness under destruction** — one re-render, not a re-bake. Never
  stale. This was the bake's fatal objection.
- **Stability under camera motion** — map A has no refit, so §9.1 cannot bite
  it. This was the shadow map's fatal objection.
- **More texels on both halves** than either has today.

### 9.5 Then: PCSS, and screen-space contact shadows

**PCSS** — search for blockers, widen the filter by blocker distance — gets the
contact hardening that Valve only obtains from the area-light bake: a unit's
foot shadow tight, a roof edge across the ground soft.

**Screen-space contact shadows** — a short ray march in depth — close the small
gap under objects that normal-offset bias inevitably opens. The depth+normal
prepass that feeds SSAO is already there to march against.

Neither is a Source 2 technique. Both are the honest dynamic-world answer to a
problem Source 2 solved by not having it.

---

## 10. The roadmap, ranked by what it changes on screen

With the bake off, **100% of ambient is the analytic two-lobe hemisphere**.
That makes the lighting gap larger than the shadow gap, and it is worth being
clear about the ordering: §9 makes shadows stop misbehaving; §10 is what makes
the picture look like Alyx.

1. **Indirect light, via a realtime probe grid.** The single biggest gap: a
   sealed room gets open-ground sky irradiance dimmed only by SSAO. The chosen
   approach is an incrementally-updated irradiance probe grid rather than any
   kind of bake — see §11 for why and for what it costs. It subsumes what used
   to be listed here as two separate items, because the same grid lights static
   surfaces *and* the units and props standing on them, which is Valve's
   probe-lit mesh entity in all but name. **[VDC]**
2. **Local lights.** None exist. Even unshadowed point lights — baked
   contribution for static, dynamic for muzzle flash, fire and explosions —
   would change interiors more per line of code than anything else here.
3. **Environment cubemaps.** Ambient specular is currently the analytic sky
   gradient, so every metal reflects a smooth blue ramp. Source 2 uses
   parallax-corrected cubemaps. **[VDC]** A single prefiltered sky cubemap with
   roughness mips is the cheap first step; per-room parallax-corrected probes
   are the real answer. Related: AO currently multiplies ambient diffuse and
   ambient specular equally — specular wants a roughness-derived occlusion term
   instead.
4. **Fog and volumetrics.** We have none of Source 2's three systems, so there
   is no aerial perspective and no light shafts — and shafts through a hole a
   grenade just made are a signature shot currently on the table. Start with
   cubemap fog, which is cheap and fits the analytic sky exactly. The froxel
   volume is the expensive one, and it is the shared prerequisite for water
   volumetrics (§12.3) and smoke (§14) as well as shafts — so it earns its cost
   three times over. Full detail in §13.
5. **Scene bloom, and emissive.** `GlowPass` is a good octave-summed chain, but
   it runs on the backbuffer *after* tonemap and only re-draws ribbons. Running
   the same chain against the HDR scene target before the tonemap blit is
   small. It needs emissive materials to pay off, and there is no emissive map
   or factor anywhere in the material system — do the two together or neither
   reads.
6. **Material authoring depth.** Detail maps, trim sheets and vertex-blend
   layers are how Source 2 worlds avoid visible tiling; decals are much of why
   its surfaces are not procedurally clean. We have destruction but no scorch,
   grime or wear. Also missing: the separate greyscale reflectance map that
   keeps metalness from driving specular *amount*. **[VDC]**

Two cheap ones worth doing whenever, independent of the above:

- **Dither before the 8-bit write** in `tonemap.fs.glsl`. The sky is a smooth
  wide gradient over most of the frame; at 8 bits it will band. Three lines.
- **Exposure is a fixed uniform** — no adaptation. Fine for a fixed sun outdoors;
  becomes wrong the moment (1) makes interiors genuinely darker than exteriors
  in the same shot.

---

## 11. Realtime GI — the decision, and the shape of it

**Decided:** indirect light comes from an **incrementally-updated irradiance
probe grid**, evaluated at runtime. Not a bake, not a hybrid.

The reason is the destructible map, and it is the same reason twice. A bake is
wrong the instant a wall falls, and the only repair is to re-bake — which is a
blocking cost that grows with blast radius, reaching 1.6 s in the measurements
above. A probe grid has nothing to invalidate: probes are re-traced
continuously whether or not anything changed, so destruction is not an event
the lighting has to *handle*. At most, probes near the blast get pushed to the
front of the update queue so the hole fills with light sooner. Everything
Source 2 gets from VRAD3 at compile time, this gets over the next handful of
frames — and on a turn-based board, behind a detonation animation, that
convergence is not perceivable.

### What it does and does not cover

**It does not give you sun shadows.** This is the distinction to hold onto: GI
is the *indirect* term. The hard-edged shadow a wall throws across the ground
is direct sun, and it still comes from the shadow map in §9. Those are two
independent systems with two independent sets of problems, and adopting a probe
grid does not close §9 by even a little. Deciding realtime GI is not deciding
that lighting is solved.

What it does cover:

- Sky occlusion — an interior stops receiving open-ground irradiance. This is
  the worst-reading of the current artifacts and the grid fixes it directly.
- Bounce and colour bleed — light through a window fills the room instead of
  making one bright patch and stopping.
- **Units and props**, from the same grid, trilinearly sampled. Source 2 needs
  a separate probe build for mesh entities because its world half is
  lightmapped; ours has one system for both. **[VDC]**

### Why the grid fits this codebase unusually well

- **The lattice is already the grid.** One probe per cell needs no placement
  pass, no author-placed volumes, no packer. Contrast the shelved bake, which
  needed a patch list, an atlas packer and an index texture.
- **The `RayCaster` already exists**, with `RayRules` already separating the
  sunlight predicate from the LOS one — the distinction a GI trace needs.
- **Leak rejection is a lattice query, not a heuristic.** DDGI's standard
  failure is light bleeding through thin walls, normally fought statistically
  with per-probe depth and a Chebyshev test. Here the solid cells are *known*.
  A probe on the far side of a wall from the surface being shaded can be
  rejected outright by asking the lattice, which is both cheaper and exact.
  Probes inside solid cells are marked dead the same way.
- **Multi-bounce is free.** Probes gather from *other probes'* previous-frame
  irradiance where a ray hits a surface, so successive frames accumulate
  bounces without tracing paths. One-bounce cost, many-bounce result.

### Rough cost — estimated from our own measurements, not measured

The lattice is 24 × 24 × 9 = **5184 cells**, so 5184 probes at one per cell. At
64 rays per probe that is **332k rays** for a complete refresh of every probe in
the world. §"The bake, and why it is shelved" measured the threaded `RayCaster`
at **~43 Mray/s**, which puts a full refresh at roughly **8 ms of CPU** — and a
full refresh every frame is not needed. A round-robin over 8 frames is ~1 ms per
frame.

Two caveats on that number, both pushing it up. Bake rays test *visibility* and
stop; GI rays must also shade what they hit, so cost per ray is higher, though
DDA traversal should still dominate. And this is arithmetic on someone else's
benchmark, not a measurement of the thing itself — the first task is to make it
one.

### Alternatives considered

| | verdict |
|---|---|
| **Probe grid (DDGI-shaped)** | **Chosen.** Fits the lattice, reuses the ray caster, no staleness, lights dynamics from the same data. |
| Voxel cone tracing | The world *is* a voxel grid, so the structure is free — but cone-traced specular is expensive and leak-prone, and it buys little the probe grid does not. |
| Screen-space GI | Cheap, and the depth+normal prepass already exists to feed it. But it cannot see offscreen geometry, so it **cannot darken an interior** — it does not address the main problem. Possibly worth adding later for fine contact-scale bounce. |
| Hardware ray tracing | Not reachable from raylib / GL 3.3. |
| Any bake | Rejected — see the post-mortem above. The destructible map is the whole objection. |

---

## 12. Water and glass

Read from `csgo_water_fancy` and `csgo_glass` as reimplemented by
ValveResourceFormat. **[VRF]** Both are CS2 shaders, so this is the desktop
answer throughout.

> **Superseded in depth by [`source2_glass.md`](source2_glass.md).** That note
> takes the four cases this section does not cover — frosted, breakable, vessels
> with more than two interfaces, and liquid inside one — and is written against
> s&box's plain-text copy of the Source 2 shader library rather than VRF's
> reimplementation. What follows stays correct for the architectural pane.

### 12.1 Glass — much simpler than expected, and cheap for us

**CS2 glass does not refract.** The whole model is `GetGlassMaterial()` in
`common/texturing.slang`: **[VRF]**

```glsl
float viewDotNormalInv = clamp(1.0 - (dot(mat.ViewDir, mat.Normal)
                                      - g_flEdgeColorThickness), 0.0001, 1.0);
float fresnel = saturate(pow(viewDotNormalInv, g_flEdgeColorFalloff))
              * g_flEdgeColorMaxOpacity;
vec4 fresnelColor = vec4(g_vEdgeColor.xyz, g_bFresnel ? fresnel : 0.0);
return mix(vec4(mat.Albedo, mat.Opacity), fresnelColor, g_flOpacityScale);
```

An alpha-blended surface that grows **more opaque and takes an edge tint at
grazing angles**, with the ordinary PBR specular and cubemap reflection on top.
Glass is not a separate shading model — it is the standard material with a
Fresnel opacity ramp. `csgo_glass` adds one thing: opacity is remapped through
`g_flTranslucencyRemap` (a min/max pair) so a texture's alpha can be squeezed
into an authored range.

| parameter | role |
|---|---|
| `g_flEdgeColorThickness` | offsets N·V — how opaque the pane is head-on |
| `g_flEdgeColorFalloff` | Fresnel exponent — how fast opacity climbs off-axis |
| `g_flEdgeColorMaxOpacity` | ceiling on the Fresnel term |
| `g_vEdgeColor` | the tint picked up at grazing angles |
| `g_flOpacityScale` | blend between plain albedo/opacity and the Fresnel result |
| `g_flTranslucencyRemap` | `csgo_glass` only — remap opacity into a range |

This is the same model VDC documents for `VR Standard`'s Glass parameters —
Fresnel exponent, Fresnel thickness, Fresnel max opacity. **[VDC]** Same maths
in both, so it is a Source-2-wide choice and not a VR compromise; safe to copy.

**Why there is no refraction, and why that is physically right.** Real glass
refracts, so the absence looks like a shortcut. It is not. A flat pane has **two
parallel interfaces, and the second undoes the first** — light bends entering
and bends back leaving, so the emergent ray is parallel to the incident ray and
merely displaced sideways by `d = t·sin(θ₁−θ₂)/cos(θ₂)`. For a 6 mm pane at 45°
with n ≈ 1.52 that is about 2 mm; at 3 m, roughly a pixel.

The magnitude is not even the main argument. **Perceived refraction is *varying*
displacement, not displacement.** A flat pane shifts everything behind it
equally, and a uniform shift has no reference to be judged against — it is
invisible even at several pixels. Visible distortion needs the offset to change
across the surface, which requires curvature, varying thickness, or a moving
surface. Bottles and lenses distort; windows do not, which is why text is
readable through one.

Water is the opposite case on both counts, which is exactly why CS2 spends a
scene-colour grab there and not here: **one** interface rather than two parallel
ones, so nothing cancels the bend, and wave normals that make the displacement
vary per pixel.

What the Fresnel opacity ramp *is*, meanwhile, is the dominant real behaviour.
Glass reflects ~4% head-on and approaches 100% at grazing angles; since
R + T = 1, transmittance falls as reflectance rises. Driving alpha from a
Fresnel term is therefore Fresnel transmittance, and the specular plus cubemap
reflection added on top is the other half of the same energy split. Together
they cover the physics of a thin dielectric interface. Only the ray bending is
omitted, and for a flat pane it is not observable.

Two engineering reasons on top **[inferred]**: screen-space refraction cannot
see anything offscreen or occluded, so it fails worst at grazing angles —
precisely where glass is most opaque and most visible; and CS2 is competitive,
where distorting the view through a window would be actively harmful.

The capability exists where it is warranted — `g_flRefractScale` sits in the
same block, and the layered materials carry `g_flLayer1RefractionAmount`. It is
simply off for flat glass. **Our read:** build glass as Fresnel opacity + tint +
the existing PBR specular, and add refraction only alongside curved or thick
glass — bottles, windscreens, a domed skylight — where it starts to earn its
cost.

**The subtlety worth copying, and it is Alyx's.** In the standard path the
shader writes `outputColor.rgb = diffuse*lighting + specular + …` with
`outputColor.a = Opacity`, so ordinary alpha blending scales **specular by
opacity** — the more transparent the pane, the weaker its highlight, which is
wrong. Alyx's `F_TRANSLUCENT == 2` "membrane" mode is commented in the source as
unique, and instead premultiplies only the diffuse by opacity and forces
`a = 1.0`, leaving specular at full strength. **[VRF]** That is the physically
correct behaviour for glass, and worth noting as the one case so far where the
VR title has the better answer. Take it.

#### Alyx's glass — the three things the CS2 model does not cover

CS2's model is *architectural* glass: flat panes, clean, seen at a distance.
Alyx's glass is better because it is none of those things, and it is worth
separating what that costs.

**1. Markable glass.** `vr_glass_markable` is its own shader in the Alyx family,
routed through the same `glass_vfx_common` edge-Fresnel path. **[VRF]** VRF
reimplements the *shading* but not the marking, so the mechanism is not
recoverable from that source. **[inferred]** It has to be a dynamic per-surface
mask in UV space — a render target the marker or hand writes into, which then
modulates a grime layer's opacity, albedo and roughness. Structurally the same
idea as `cs2_baked_bomb_damage` and `csgo_projected_decals`: a persistent mask
in authored space, accumulated at runtime. Note that *wiping* grime away is the
same mask with the opposite sign — drawing and cleaning are one feature.

**2. Grime as a layer, not a texture.** The reason Alyx's glass reads as real is
that it is almost never clean, and the dirt is not painted into the albedo. It
wants its own layer with albedo, opacity, **roughness** and normal — the
`complex` family already carries detail normals (`g_tNormalDetail`,
`g_flDetailNormalStrength`) for this shape of problem. **[VRF]** The roughness
channel is the important one: dirt makes glass locally rougher, which breaks the
mirror reflection into a haze. That reads as grime far more than any albedo
change does, and it is the cheap half of the effect.

**3. Solid glass objects — bottles.** This is exactly the case §12.1's flat-pane
argument excludes. A bottle is thick and curved, its two interfaces are not
parallel, so nothing cancels the bend, and the displacement varies across the
surface. Both conditions for visible refraction are met, and the CS2 model does
not cover it.

**What Alyx does for bottles is not what you would guess.** Valve's Matt Wilde
built the liquid inside Alyx's bottles as a *pixel shader illusion on the
bottle's own surface* — "the entire effect is a visual change to the surface of
the bottle and there's nothing actually inside them at all", shipped in a May
2020 update. **[press]** The liquid level, the meniscus, the sloshing: all faked
in the surface shader, with no interior geometry and no simulation. For
hand-held glass, the interior is a **shading** problem, not a geometry or
physics one. That is the single most useful thing in this subsection.

**Machinery that already exists for thick glass:**
`F_TRANSMISSIVE_BACKFACE_NDOTL`, with `g_tTransmissiveColor`,
`g_tTransmissiveMask` and `g_vTransmissionColor`, computes lighting from the
**back** face's N·L and adds it as `lighting.TransmissiveDirect`. **[VRF]** That
is Source 2's "light passing through a surface" term, shared with foliage and
skin, and it is the right term for coloured glass that glows where the sun is
behind it.

**A lead worth following: Deadlock's glass is a different model entirely.**
`texturing.slang` gates the whole edge-Fresnel block on
`!defined(GLASS_IS_TRANSMISSIVE)`, with the comment that *"Deadlock reuses the
F_GLASS name for a transmission model that has none of these parameters, so it
opts out"*. **[VRF]** I could not find where that symbol is defined in the files
read, so the model itself is unrecovered. Worth chasing, because **Deadlock is a
desktop Source 2 title** — if a physically-grounded transmission glass model is
wanted, that is the place to look rather than CS2 or Alyx.

**Our read — two tiers, and they are not the same job [inferred]:**

| tier | covers | needs |
|---|---|---|
| **1 — architectural** | windows, panes, partitions | CS2's edge-Fresnel + a grime layer with its own roughness. No refraction. Cheap. |
| **2 — object glass** | bottles, jars, windscreens, domes | sorted two-sided draw, genuine refraction, thickness/transmission tint. Genuinely more expensive. |

Marking sits on top of either as a runtime mask, and is largely orthogonal to
both.

Tier 1 is close and worth doing. Tier 2 only earns its cost once there are hero
or hand-held glass objects in the game — and on a tactical camera looking down
at a board, that may never happen. Decide that before building it.

### 12.2 Water — `csgo_water_fancy`

A large shader. The transferable structure, in the order it matters:

**Reflection is a three-tier ladder, not a technique.** `F_REFLECTION_TYPE`:
`0 = Sky Color Only`, `1 = Environment Cube Map`,
`2 = SSR over Environment Cube Map`. **[VRF]** The important word is *over* —
SSR never stands alone. Every ray that misses, leaves the screen or runs out of
steps falls back to the cubemap, and the cubemap itself degrades to flat sky
colour. Three shippable quality levels out of one code path.

SSR details worth stealing: only **20 forward steps** (`g_nSSRMaxForwardSteps`),
`g_flSSRMaxThickness = 4.0` for the depth-buffer thickness test, blue noise
jittering *both* the step size and the thickness (`g_flSSRSampleJitter`), and
the step count scaled down by camera pitch — fewer steps when looking along the
horizon, where rays travel furthest for least gain. Roughness drives both a
cubemap LOD and a lerp between the surface normal and the reflected ray.

**Refraction is a screen-space grab.** `g_tSceneColor` and `g_tSceneDepth`,
distorted by the wave normal, clamped by `g_flRefractionLimit`, offset by
`g_flRefractSampleOffset` — and split per channel by
`g_flRefractChromaticSeparation` for dispersion. `F_BLUR_REFRACTION` optionally
blurs the refracted image.

**Depth absorption is what makes it read as water,** not the reflections.
`g_vWaterFogColor` / `g_flWaterFogStrength` and `g_vWaterDecayColor` /
`g_flWaterDecayStrength` / `g_flWaterMaxDepth` apply Beer-Lambert-style decay
through the water column, measured from the depth delta between the surface and
the scene behind it. Plus `g_flWaterFogShadowStrength` — shadowed water fogs
differently — and `g_flUnderwaterDarkening`.

**Waves are multi-octave, not a scrolling normal map.** `g_nWaveIterations`
(default 3) over `g_tWavesNormalHeight`, with separate
`g_flLowFreqWeight` / `g_flMedFreqWeight` / `g_flHighFreqWeight`, plus speed,
phase offset, sharpness and normal jitter.

**Shorelines are softened by depth** — `g_flEdgeHardness`,
`g_flEdgeShapeEffect` — so water does not cut a hard line against the bank.

**Two dirt layers.** Foam (`g_tFoam`, with scale, wobble, colour, min/max) and
debris/scum (`g_tDebris` + `g_tDebrisNormal`, with tint, reflectance,
"oilyness", edge sharpness, wobble). These are most of what stops it reading as
a clean CG plane.

**Caustics** (`F_CAUSTICS`) with an optional triplanar projection, depth
falloff, and `g_flCausticShadowCutOff` — caustics are killed inside shadow.

**Specular is deliberately pushed into bloom.** `g_flSpecularPower = 300`, with
`g_flSpecularBloomBoostStrength = 100` over `g_flSpecularBloomBoostThreshold` —
sun glints are explicitly boosted past the bloom threshold. This only works if
scene bloom exists.

### 12.3 The four water effects we actually want

Stated goals for this project's water: **refraction, subsurface scattering,
volumetrics, caustics.** CS2 gives three of them outright and approximates the
fourth. Here is where each one comes from, and where CS2 stops being the
reference.

#### Refraction — CS2 has it, copy it directly

Covered above: a screen-space grab of `g_tSceneColor` distorted by the wave
normal. The three details that make it look authored rather than wobbly are
`g_flRefractionLimit` (clamp the distortion so the grab never reaches off-object
and smears a silhouette), `g_flRefractChromaticSeparation` (offset the three
channels for dispersion), and `F_BLUR_REFRACTION` (blur the refracted image with
depth, for anything deeper than a puddle).

The one correctness trap: the scene-colour grab contains geometry *in front of*
the water too. CS2 guards this with the depth compare it already does for
absorption — if the sampled pixel is nearer than the water surface, the
refracted sample is invalid and must fall back to the undistorted one, or the
water will smear objects that are standing in front of it.

#### "Subsurface scattering" — what water actually needs is absorption

Worth separating two things that both get called SSS:

- **Skin-style SSS** — diffusion of light *within a thin surface*, the
  `vr_skin` shading model with a transmission colour. Not what water wants.
- **Volumetric absorption and in-scatter through a depth of medium** — light
  entering the surface, losing wavelengths with distance, and coming back out.
  This is what makes water read as water, and what "SSS" almost always means
  when someone says it about water.

CS2 does the second one, analytically, from the depth delta between the water
surface and the scene behind it: `g_vWaterDecayColor` / `g_flWaterDecayStrength`
/ `g_flWaterMaxDepth` for Beer-Lambert extinction, `g_vWaterFogColor` /
`g_flWaterFogStrength` for the in-scattered colour, and
`g_flWaterFogShadowStrength` so water in shadow scatters differently from water
in sun. **[VRF]**

That is a *single-scattering* approximation evaluated at the surface — it is
cheap, it is what CS2 ships, and it gets ninety per cent of the look. It cannot
do shafts of light inside the water body, because it never marches the volume.
For that, see the next item.

**Sea of Thieves solves the same problem with a different proxy, and the two
compose.** Rare blend a deep-water colour against a subsurface colour by view
angle, sun direction and a *wave-peak thickness* mask, where CS2's proxy for
optical thickness is the *water column depth*. Neither game does real subsurface
scattering; both pick a cheap geometric stand-in for thickness and blend authored
colours by it. Full detail, and what to take, in
[`sea_of_thieves_water.md`](../rendering/sea_of_thieves_water.md) §3.

#### Volumetrics — CS2's water shader does NOT do this

This is the one place where the water shader stops being the answer. Genuine
light shafts *inside* a water volume, and underwater god rays, need a marched
participating medium — which in Source 2 is the froxel system in §13, not
`csgo_water_fancy`.

The right architecture **[inferred]**: keep CS2's analytic absorption as the
cheap surface-level term, and treat the water body as a medium region injected
into the froxel volume for the in-scatter. They compose — absorption is what the
water takes *out* of the refracted background, the froxel volume is what the
water puts *in* along the view ray. Trying to get shafts out of the surface
shader alone is the mistake to avoid.

Note this is the same system that gives light shafts through a hole a grenade
just made, so it is not water-specific work. §13.

#### Caustics — CS2 has it, and the two subtleties are worth copying

`F_CAUSTICS`, with `g_flCausticsStrength` (default **40** — they are not
subtle), `g_flCausticUVScaleMultiple`, `g_flCausticDistortion`,
`g_flCausticSharpness`, `g_vCausticsTint`, and two parameters that carry most of
the plausibility:

- `g_bUseTriplanarCaustics` — project the caustic pattern triplanarly rather
  than planar-down, so it lands correctly on walls and sloped banks instead of
  smearing.
- `g_flCausticShadowCutOff` — **caustics are killed inside shadow.** A caustic
  is focused sunlight; it cannot exist where the sun does not reach. Skipping
  this is the single most common reason game caustics read as a projected
  texture.

Plus `g_flCausticDepthFallOffDistance`, so the pattern fades out with depth
rather than tiling to the sea floor at full strength.

Caustics here are a *projected pattern*, not a photon-mapped simulation. That is
the shippable choice and it is what CS2 does.

#### Summary

| want | CS2 source | our status |
|---|---|---|
| Refraction | `csgo_water_fancy`, screen-space grab + dispersion | needs a scene-colour copy |
| "SSS" = absorption / in-scatter | `csgo_water_fancy`, analytic from depth delta | needs a depth delta; otherwise self-contained |
| Volumetrics (shafts in the medium) | **not the water shader** — froxel fog, §13 | needs §13 |
| Caustics | `csgo_water_fancy`, projected + triplanar + shadow cutoff | needs the shadow term, which we have |

### 12.4 What we would need first

| prerequisite | needed by | status |
|---|---|---|
| Environment cubemaps | all glass reflection; water tiers 1 and 2 | §10.3, missing |
| A sorted transparent pass | any glass at all | missing — everything draws in one opaque batch |
| Scene colour copy | water refraction | missing |
| Scene bloom | water's specular glints | §10.5, missing |
| Depth + normal buffer | SSR | **already there** — the SSAO prepass |

**Read [inferred].** Tier-1 glass is cheap and close: a Fresnel opacity ramp over
the existing PBR shader, a grime layer, and a sorted blend pass. No refraction is
needed to match CS2, and the material system already declares `AlphaMode::Blend`
without anything honouring it. Tier-2 object glass adds sorted two-sided draw,
a scene-colour grab and transmission — the same scene-colour dependency water
has, so if both are wanted they should be built once. Water itself is a large
system depending on three things we do not have, and should wait behind cubemaps
and bloom.

Neither belongs ahead of §9 or §11 — a window that reflects a sky gradient
because there are no cubemaps, standing in a room lit as though the roof were
missing, is not going to read as CS2 whatever the glass shader does.

---

## 13. Volumetric fog — the froxel system

Read from `common/fog.slang`. **[VRF]** This is the substrate three separate
wants share: light shafts in water (§12.3), light shafts through destroyed
geometry (§10.4), and smoke (§14). Worth understanding once.

> Source 2's froxel system is only recoverable in outline, from reimplemented
> shaders. **RDR2's equivalent is documented in full by the people who shipped
> it** — three-volume split, material packing, blend order, temporal filtering
> and per-pass PS4 costs — in [`rdr2_atmospherics.md`](../rendering/rdr2_atmospherics.md).
> Read that before building any of this; §9 there argues our board fits entirely
> inside the near sampler, which deletes the expensive half of both designs.

`ApplyFog()` runs up to three systems per pixel, in this order:

### Volumetric fog

A **frustum-shaped 3D texture**, `sampler3D g_tFogVolume` — a froxel grid, not a
world-space volume. Every shaded pixel does one fetch:

```glsl
vec3  jittered   = positionWS + blueNoise * dither.xxx + dither.yyy;
vec4  projected  = vec4(jittered, 1.0) * g_mVolFogFromWorld[0];
vec3  frustum    = vec3(projected.xy / projected.ww, projected.w);
vec4  coords     = vec4(frustum.xy, sqrt(ClampToPositive(frustum.z)), projected.w);
vec4  volFog     = texture(g_tFogVolume, coords);
PixelColor = (PixelColor * volFog.a) + volFog.rgb;
```

Four things in there are the whole design, and all four are worth copying:

- **The volume stores premultiplied in-scatter in RGB and transmittance in A.**
  The composite is therefore one multiply-add per pixel, and identical whatever
  is in the volume — fog, water, smoke, all of it. The expensive marching
  happens once, into the volume, not per pixel per effect.
- **Cost is independent of scene complexity.** Shading a pixel costs one 3D
  texture fetch whether the volume holds a whole map's fog or nothing.
- **`sqrt()` on the depth coordinate** distributes slices non-linearly, so
  resolution concentrates near the camera where the eye can resolve it.
- **Blue-noise jitter of the sample position**, before projection. Froxel grids
  are coarse; without the jitter their slice boundaries show as banding. Same
  trick the water shader uses on its SSR steps.

VRF's own note is worth recording: *"I have all the shader code for volumetric
fog, but it would be HARD to implement."* **[VRF]** Treat this as the expensive
item it is.

### Cubemap fog (`env_cubemap_fog`)

`textureLod(g_tFogCubeTexture, fogCoords, cubemapFogLod)`, where the direction is
the view ray transformed into the fog cube's space, and **the LOD rises with the
fog blend factor** — distant fog samples blurrier mips, which is a cheap and very
effective stand-in for scattering widening the point spread. Culled by squared
distance and by world height, so near and low pixels skip it entirely.

This is by far the cheapest of the three and it is what CS2 maps actually use for
distance fog. If only one fog system ever ships here, it should be this one — and
we already have an analytic sky to generate the cube from.

### Gradient fog

View-space depth crossed with world height, one blend factor. Trivial, and the
right first thing to have.

*(A fourth, spherical vignette, is Alyx-only; VRF's comment calls it "a bad idea,
plain and simple". Ignore.)*

---

## 14. Smoke — CS2's voxel grenades

The property that makes this interesting is not the rendering. It is that
**every player sees the identical smoke**, which is what killed CS:GO's one-way
smokes. **[press]** Getting that is an architectural choice, not a graphics one.

### What Valve and press state

- Smoke grenades are **volumetric dynamic objects**, not sprite blobs.
- They **expand to fill space naturally**, flowing around crates, through doors,
  and never through walls.
- They **react to the map's lighting**.
- Bullets, HE grenades and Molotovs **displace them**, opening temporary
  sightlines that then close again.
- **All players see the same smoke regardless of viewing angle.**
- Sub-tick servers make the grenade itself land identically for everyone.

### How it is done — from recreations, not from Valve

Everything in this subsection is reverse-engineered or reproduced by third
parties. Marked **[repro]**; do not read it as Valve's implementation.

The decomposition that recurs across every credible recreation: **[repro]**

1. **Mesh voxelizer** — bake the map's collision into a voxel occupancy grid, so
   the sim knows what is solid.
2. **Limited flood fill** — from the grenade's cell, flood outward through free
   voxels, stopping at solids and when the smoke's volume budget is spent. This
   is what produces "fills the room, doesn't cross the wall" for free.
3. **Volume ray march** at render time, against that voxel set.
4. **Tiled Worley noise** for density detail.
5. **SDF-based deformation** for bullet holes — the projectile's path writes hit
   points with a small radius, punching local holes that then heal.

An academic reproduction (SMU Guildhall) reports SIMD-optimised flood fill,
compute-shader ray marching, physically-based lighting with soft shadowing,
10,000+ interacting particles at a stable 60 FPS from integrated graphics
upward. **[repro]**

**Point 4 is the one to internalise.** The voxel grid is coarse and only decides
*where smoke is*. Everything that reads as billowing cloud comes from 3D Worley
noise sampled during the march. Simulation for extent, noise for texture. Trying
to get the look from a finer simulation is the expensive wrong turn.

**Voxel resolution: not established.** I could not find a credible figure for
CS2's actual grid size, and the recreations all pick their own. Do not repeat a
number for this without a source.

### Why it can be identical on every machine — [inferred]

The smoke's state is *a set of occupied voxels produced by a flood fill over
static geometry from a known origin cell*. That is a discrete, integer
computation. Run it anywhere with the same map and the same origin and it is
bit-identical — no floating-point drift to diverge, no per-client timing
dependence.

That is precisely why CS2 can afford to make it authoritative, and it is why the
approach is a *fluid-sim substitute rather than a fluid sim*. A Navier-Stokes
solve would not survive this requirement: floating-point accumulation across
different hardware would drift, and the drift is exactly what one-way smokes are
made of. **The determinism is not a nice property of the technique — it is the
reason the technique was chosen.**

### Why this fits our project unusually well — [inferred]

Every recreation's hardest step is step 1, the mesh voxelizer. **We do not need
it.** The world already *is* a voxel lattice, and `BlockedMass`, `Terrain` and
`Standability` already answer "is this cell solid".

- The flood fill is a breadth-first walk over `Lattice` cells with an existing
  blocked-mass predicate. Tens of lines.
- It is **turn-based**, so the fill runs once per turn or per event, not per
  frame. The cost argument that dominates CS2's design barely applies.
- **It should block line of sight, and that is nearly free.** `RayCaster`
  already takes `RayRules` precisely so sunlight and sight can disagree about
  what blocks. Smoke is a third predicate on the same traversal — the same
  design that let a sandbag wall be opaque to sun and transparent to fire.
  That turns smoke from a visual effect into an XCOM mechanic with no new
  traversal code.
- Rendering is the froxel volume in §13, with smoke injected as a medium — the
  same volume water and light shafts want. One system, three payoffs.
- Destruction interaction falls out: the fill re-runs against the new lattice,
  so a blown-open wall lets smoke through on the next evaluation.

The honest caveat: **§13 is the prerequisite and it is the expensive part.** The
simulation side of smoke is nearly free for us; the volumetric rendering is not,
and it is the same work three times over on this list. That argues for doing §13
properly and once.

---

## Sources

- [Source 2 — Valve Developer Community](https://developer.valvesoftware.com/wiki/Source_2)
- [Source 2 Lighting](https://developer.valvesoftware.com/wiki/Source_2/Docs/Level_Design/Lighting)
- [Lightmap (Source 2)](https://developer.valvesoftware.com/wiki/Lightmap_(Source_2))
- [light_environment (Source 2)](https://developer.valvesoftware.com/wiki/Light_environment_(Source_2))
- [Cubemaps (Source 2)](https://developer.valvesoftware.com/wiki/Cubemaps_(Source_2))
- [Physically Based Rendering](https://developer.valvesoftware.com/wiki/Physically_Based_Rendering)
- [$mraotexture](https://developer.valvesoftware.com/wiki/$mraotexture)
- [VR Standard (Source 2 Shader)](https://developer.valvesoftware.com/wiki/VR_Standard_(Source_2_Shader))
- [env_cubemap_fog](https://developer.valvesoftware.com/wiki/Env_cubemap_fog)
- [CS2 Level Design Lighting](https://developer.valvesoftware.com/wiki/Counter-Strike_2_Workshop_Tools/Level_Design/Lighting)
- [CS2 Material Creation](https://developer.valvesoftware.com/wiki/Counter-Strike_2_Workshop_Tools/Materials/Material_Creation)
- [CS2 Adding Water](https://developer.valvesoftware.com/wiki/Counter-Strike_2_Workshop_Tools/Level_Design/Adding_Water)
- [SteamVR Environments — Adding Lighting](https://developer.valvesoftware.com/wiki/SteamVR/Environments/Adding_Lighting)

For §12 and §13, read from ValveResourceFormat's reimplemented shaders rather
than the wiki — VDC now sits behind an anti-bot wall and is not reliably
fetchable:

- [ValveResourceFormat](https://github.com/ValveResourceFormat/ValveResourceFormat) —
  `Renderer/Shaders/water_csgo.frag.slang`, `complex.frag.slang`,
  `common/texturing.slang`, `common/fog.slang`

For Alyx's bottle liquids in §12.1, marked **[press]**:

- [Valve Developer Breaks Down Half-Life: Alyx's Incredible Liquid Shaders](https://www.uploadvr.com/alyx-liquid-shaders/)
  — Matt Wilde on the bottle-surface pixel shader, May 2020 update.

For §14, marked **[repro]** — third-party recreations, not Valve's
implementation:

- [Garrett Gunnell — CS2 Smoke Grenades (Unity)](https://github.com/GarrettGunnell/CS2-Smoke-Grenades)
  and the [accompanying video](https://www.youtube.com/watch?v=ryB8hT5TMSg) —
  voxel propagation by limited flood fill, mesh voxelizer, volume ray marcher,
  tiled Worley noise, SDF deformation.
- [Voxel-Based Physically Simulated Particle System for Realistic Smoke Effects
  (SMU Guildhall thesis)](https://scholar.smu.edu/guildhall_programming_etds/10/)
  — SIMD flood fill, compute-shader ray marching, performance figures.
- [Counter-Strike 2: Responsive Smokes](https://www.youtube.com/watch?v=_y9MpNcAitQ)
  — Valve's own announcement, for the behavioural claims.
- [80.lv — CS2-style responsive smoke in Unity URP](https://80.lv/articles/counter-strike-2-style-responsive-smoke-set-up-in-unity-urp)
