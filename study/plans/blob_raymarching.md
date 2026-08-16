# Blob rendering by raymarching — how it would work, and when it wins

**A design note, not research.** The factual account of how Valve did it is in
[`portal2_gels.md`](../games/valve/portal2/portal2_gels.md) §6; this note answers a
different question — *if we wanted Portal 2's gel blobs in `cromwell`, would we
mesh them like Valve did, or march them?* Nothing here is scheduled and nothing
is implemented. It exists so the decision is already made the day something in
this project needs a liquid, a slime, a fuel spill, a smoke volume or a
merging-sphere effect.

Requirements-style numbering is deliberately absent: this is a comparison and a
recipe, not a spec. Compare [`particle_architecture.md`](particle_architecture.md),
which *is* a spec, and whose §9 rendering section this would extend rather than
replace.

---

## 1. The question, and the short answer

An implicit surface — a field `f(p)` with the surface at `f(p) = iso` — has to
become pixels somehow. There are exactly two families:

1. **Polygonise it.** Sample the field on a grid, emit triangles (marching cubes
   / marching tetrahedra), feed them to the ordinary material pipeline. This is
   Valve's Blobulator.
2. **March it.** Emit no geometry at all. Rasterise a cheap proxy volume, and in
   the fragment shader walk the ray until it crosses the isosurface, then shade
   the hit.

**The short answer: yes, raymarching is the right choice for this project, and it
is a genuinely smaller system than the Blobulator — but the reason Valve did not
do it is not that it was 2011, and it is worth understanding before choosing.**
The reason is §7: **a mesh is view-independent and a march is not**, and Portal 2
had more views per frame than any other shipped game of its generation.

## 2. Why Valve meshed, stated fairly

From [`portal2_gels.md`](../games/valve/portal2/portal2_gels.md) §6 and §10, the
Blobulator is a tiled, multithreaded, job-driven marching-cubes implementation
with a recycled mesh-fragment pool and four documented degradation paths. That is
a *lot* of machinery. What it buys:

| | mesh (Blobulator) | march |
|---|---|---|
| build cost | once per frame, on the CPU/SPU | **once per view, on the GPU** |
| drawn in N views | build once, draw N times | **march N times** |
| shadow map | it is geometry; it casts | needs a second march, or nothing |
| collision / physics queries | possible from the mesh | none |
| material pipeline | ordinary shaders, ordinary sorting | bespoke shader, bespoke sorting |
| memory | vertex/index pool, and it can starve | none |
| topology change | free (that is what MC is for) | free |
| cost driver | field evaluations × grid cells (**volume**) | field evaluations × pixels × steps (**screen area**) |

The last row is the decision. **Marching cubes is billed by world volume; ray
marching is billed by screen area.** A blob that fills the screen is nearly free
to mesh and expensive to march. A hundred small blobs scattered across a room are
cheap to march and expensive to mesh.

And the row above it is the one that decided it for Portal 2: they render *many*
views per frame (main, split-screen second player, water reflections, and up to
`r_portal_stencil_depth` portal views per portal —
[`portal2_portal_rendering.md`](../games/valve/portal2/portal2_portal_rendering.md) §12).
A mesh amortises across every one of them. That is exactly why
`r_paintblob_mainview_highres` and `r_paintblob_otherviews_highres` are separate
cvars: the *mesh* is built at two qualities and each is reused.

**We do not have that problem.** One camera, one shadow view, no portals.

## 3. The field, and where "bloating" comes from

This is the part most implementations get wrong, and it is the difference between
"spheres that merge" and "spheres that *bulge* as they merge".

### 3.1 Sum of kernels — the real metaball, and the real bulge

```
f(p) = Σ  k( |p - cᵢ| / Rᵢ )        surface where f(p) = iso
```

with a compact-support falloff — Wyvill's `k(x) = (1 - x²)³` for `x < 1`, zero
beyond, is the standard: cheap, C² continuous, and **zero outside `R`**, which is
what makes spatial binning possible at all.

The bulge is a direct consequence of the sum. Two blobs a little under `2R`
apart: at the midpoint neither kernel alone reaches `iso`, but *together* they do,
so a bridge appears — and along the axis between them the field is higher than
either sphere alone, so **the surface pushes outward past where either sphere's
own isosurface was**. That outward swelling is the "bloat". It is the whole
visual signature of a metaball, and it is why gel reads as *liquid* rather than as
overlapping balls.

The cost: `f` is a **density**, not a distance. It tells you nothing about how far
away the surface is, so you cannot take big steps safely.

### 3.2 Smooth-minimum of SDFs — bounded, but it does not bulge

```
d(p) = smin_k( |p - c₀| - R₀ , |p - c₁| - R₁ , … )
smin(a,b,k) = min(a,b) - h²k/4,   h = max(k - |a-b|, 0)/k     // IQ's polynomial
```

Because `smin(a,b) ≤ min(a,b)`, the blended surface only ever moves *outward
toward the other object* — you get a fillet at the join, and no swelling beyond
either sphere. It looks like welded spheres, not like liquid.

The gain is decisive though: the result is *approximately* a signed distance, so
you can **sphere-trace** — step by the field value itself, which is a safe
distance to travel. That turns a 200-step fixed march into a 20-step one.

The catch nobody mentions: smin over many primitives is **not 1-Lipschitz** near
the blend region — the field under-reports distance there — so raw sphere tracing
overshoots and punches holes in the surface. Multiply the step by a safety factor
(0.7–0.85) and clamp `k` to something like `0.5 × R`.

### 3.3 The recommendation: bulge, but step safely

Take the density field of §3.1 for the look, and get bounded stepping from the
*geometry* rather than from the field:

- The support radius `R` is finite, so **the isosurface can never lie outside the
  union of the support spheres.** That union is a plain SDF — a normal `min` of
  spheres, exactly 1-Lipschitz.
- **Phase 1, outside:** sphere-trace against that union. Big steps, exact, no
  density evaluations at all.
- **Phase 2, inside the hull:** fixed steps, sized to a fraction of the smallest
  `R` (`R/4` is a reasonable start), evaluating the density sum.
- **Phase 3, on a sign change:** 3–4 bisection or one secant refinement between
  the last two samples. This is what removes the stair-stepping on the silhouette
  and it costs almost nothing because it is a handful of evaluations on hit
  pixels only.

That is Sherstyuk's structure and it is still the right one. It gives the metaball
bulge *and* skips the empty space, which is where all the rays are.

## 4. The march

```glsl
// sketch, not code to paste — see §9 on where this would actually live
vec3  ro, rd;                      // ray from the camera through this fragment
float t     = tEnterProxy;         // entry into the proxy volume
float tMax  = min(tExitProxy, sceneDepthAlongRay);   // never march past the world

for (int i = 0; i < MAX_STEPS && t < tMax; ++i) {
    vec3  p    = ro + rd * t;
    uint  cell = clusterIndex(p);              // §5
    float dHull = hullDistance(p, cell);       // min over support spheres
    if (dHull > 0.0) { t += max(dHull * 0.9, tMax * PIXEL_EPS); continue; }

    float f = density(p, cell);                // Σ kernels in this cluster only
    if (f > ISO) { t = refine(ro, rd, tPrev, t); hit = true; break; }
    t += stepInside;
}
```

Details that matter more than the loop:

- **Distance-scaled epsilon.** The minimum step and the hit threshold should grow
  with `t`, so precision is constant *in screen space* rather than in world
  units. A blob 200 m away does not deserve millimetre steps.
- **Depth in.** Sample the scene depth buffer, convert to a distance along the
  ray, and use it as `tMax`. That is how blobs get correctly occluded by world
  geometry, for free, with no sorting.
- **Depth out.** Writing `gl_FragDepth` from the hit `t` makes the blobs behave
  like a real surface for everything downstream — fog, DOF, later passes. It also
  **disables early-Z for the pass**, so declare `layout(depth_greater) out float
  gl_FragDepth` (`GL_ARB_conservative_depth`, core in 4.2, and we target 4.3) to
  get some of it back, and run the pass after opaque.
- **Normals** by central differences on the field. Use the four-tap tetrahedron
  trick, not six axis-aligned taps — the gradient is the single most expensive
  thing in the shader after the march itself, and 4 < 6 is a 33% saving on it.
- **The proxy.** Instanced boxes around clusters of blobs, back faces, depth test
  `GREATER` disabled and no depth write on the proxy itself, so the shader runs
  once per covered pixel whether or not the camera is inside the box. Merge or
  accept overdraw; an early `tEnter > tMax` discard makes overlap cheap.

## 5. The acceleration structure, which is the actual system

Everything above is a weekend. **The part that decides whether this ships is
never evaluating every blob per step.**

How many is "every"? Valve's own shipped maps are the answer, and it is smaller
than the cap suggests: `mp_coop_paint_bridge` runs its gel waterfall at
**`maxblobcount 40`**, `mp_coop_paint_come_along` at 50, against an FGD maximum of
250 ([`portal2_gels.md`](../games/valve/portal2/portal2_gels.md) §6). **Forty
spheres.** Budget for the worst case — several streams, a bomb's twenty, blobs
mid-air through a portal — but do not design for hundreds when the shipped
reference is dozens. At forty, a brute-force loop is very nearly fine and the
cluster grid is an optimisation; at four hundred it is the system.

The answer is already half-built in this project's plans: **the clustered forward
lighting grid indexes lights per froxel; the identical structure indexes blobs.**
Same build pass, same per-cluster index list, same lookup by view-space position.
If clustered lighting lands first, this is a second index buffer over the same
grid and the marcher's inner loop is the light loop with a different array.

Concretely, per frame:

1. Bin blobs into the cluster grid by their support sphere (a blob touches every
   cluster its `R` overlaps — the same conservative test as a point light's
   radius).
2. Per cluster, store `(offset, count)` into a flat index array.
3. In the march, `clusterIndex(p)` → iterate that cluster's blobs only.

Bounded work per step, independent of total blob count. This is CLAUDE.md's
first rule — *do less work, algorithmically* — and it is worth an order of
magnitude more than anything in §4.

The one honest complication: a blob's support sphere can be large relative to a
cluster, so clusters near a dense stream will hold many blobs. Cap the per-cluster
list and accept dropped contributions at the tail (Valve do exactly this, and say
so out loud: *"Too many particles are added to a tile. Some particles will be
discarded"*).

## 6. What marching buys that the mesh cannot

This is the part that makes it more than a shortcut, and it maps directly onto
the `$interior*` parameters in Portal 2's `paintblob` shader
([`portal2_gels.md`](../games/valve/portal2/portal2_gels.md) §10):

- **Real thickness.** Once you have hit the surface, keep marching to find the
  *exit*. `thickness = tExit - tHit` is the actual distance through the gel, so
  the interior tint is Beer–Lambert absorption — `exp(-σ · thickness · colour)` —
  rather than Valve's clever normal-based fake (`$interiorFogNormalBoost`, which
  exists precisely because a mesh has no thickness at the shading point).
- **Refraction with real depth.** Offset the scene-colour sample by the surface
  normal *scaled by the thickness you just measured*, instead of by a constant
  `$interiorRefractStrength`.
- **Continuous LOD.** Step count and march resolution are two floats. There is no
  "low quality mesh" to build and no second code path — Valve needed
  `r_paintblob_highres_cube`, `_mainview_highres`, `_otherviews_highres` and a
  separate low-quality console renderer to get what a step budget gives for free.
- **Radius animation is free.** Growing, shrinking, pulsing or splashing blobs
  cost nothing extra; with marching cubes every change re-polygonises. The gel
  *arriving* — the thing that makes it read as liquid — is the case that hurts a
  mesher most and a marcher not at all. And this is not hypothetical: Portal 2's
  emitter authors it directly, with `start_radius_min/max`, `end_radius_min/max`
  and `radius_grow_time_min/max` per sprayer, so **every blob in the game is
  already changing size over its life** and the Blobulator re-polygonises for it
  every frame.
- **No pool, no starvation, no degradation paths.** All five of the Blobulator's
  error strings are about the fragment pool. There is no pool.

## 7. What it costs, honestly

- **Fill rate, and nothing but fill rate.** A screen-filling gel spill is the
  worst case and it is the common case in a puzzle chamber. §8 is the mitigation
  and it is not optional.
- **Per view.** Shadow map, reflection, a second camera — each is another full
  march. A mesh is built once. If this project ever grows a reflection pass
  ([`../realtime_reflections.md`](../realtime_reflections.md)), blobs in it are a
  second march or an absence, and "an absence" is a real answer for a small
  effect.
- **No shadows without deciding.** Blobs cast nothing unless the shadow pass
  marches them too (cheap-ish: no shading, front-to-back, early exit on first
  hit) or unless they get a crude proxy — spheres in the shadow pass, which for a
  soft liquid is nearly indistinguishable.
- **No collision geometry.** Physics keeps using the blob *spheres*, which is what
  Valve do anyway (`paintblob_collision_box_size`, `BlobTraceEnum`). The mesh was
  never the collider. This looks like a cost and is not one.
- **Sorting and translucency.** Gel is translucent, so it composites after opaque,
  and two separated blob volumes overlapping in screen space need either one
  march that handles both (the cluster grid does) or accepted error.
- **TAA/DOF.** Anything reading depth will now read blob depth. That is desirable,
  but it means the depth write in §4 is a decision with reach.

## 8. The resolution plan

Blobs are smooth, low-frequency and rounded; they are the ideal candidate for the
oldest trick in the fill-rate book, and the one
[`../games/valve/source2_particles.md`](../games/valve/source2_particles.md) §13.5
already lists:

1. March at **half resolution** into a small target (colour + hit distance).
2. **Depth-aware upsample** to full res — bilateral weights on the depth
   difference so the silhouette against world geometry stays sharp.
3. Composite.

That is a 4× cut on the dominant cost for a barely visible quality loss. If the
silhouette shimmer shows, the standard fix is to run only the *edge* pixels at
full resolution, detected from the upsampled hit distance.

**Temporal reuse** is the second lever: reproject last frame's hit distance and
start this frame's ray just short of it. Typical saving is most of the march on
pixels that were already gel last frame. Disocclusions must fall back to the full
march, and a wrong start distance is a hole, so it needs a conservative back-off.
Not phase one.

## 9. Where it would sit in `cromwell`

- **Behind the RHI.** No GL in game code, no exceptions — the pass is a
  `IScenePass` implementation in the engine, and it declares the targets and
  buffers it needs through the RHI like every other pass.
- **The engine owns the drawing; the game owns the blobs.** The game supplies a
  flat array of `{ centre, radius, colourIndex }` — a caller-supplied buffer,
  filled per frame, no allocation, per CLAUDE.md's hot-loop rules — and never says
  how it is drawn. Same boundary as every other renderer feature here.
- **One SSBO for blobs, one for the cluster index lists.** Both rewritten per
  frame, both sized once at start-up to a hard cap (Valve's is 250 per sprayer
  and they ship 40; ours should be a single global cap).
- **Lighting: one light for the whole stream is enough.** Portal 2's sprayer has a
  `light_position_name` keyvalue naming *one entity* whose position lights every
  blob it emits. Before wiring blobs into the clustered light loop, note that the
  shipped reference does not — a liquid reads correctly from a single dominant
  direction plus an environment map, and that is a large saving in the marcher's
  innermost loop.
- **One profiler zone, named `blobs`.** Sub-zones only after a measurement says it
  is a big slice of the frame — the granularity rule in CLAUDE.md exists exactly
  for a system like this, where it is tempting to instrument the march, the
  binning and the upsample separately before knowing whether the whole thing costs
  0.3 ms.
- **A `CW_GPU_ZONE("blobs")` in the same commit**, because this is entirely GPU
  work and an unzoned pass is invisible rather than zero.

## 10. If it were ever built, in this order

1. Blob array + cluster binning + the proxy draw. Flat-shaded, no blending, ugly.
   This proves the acceleration structure, which is the only part that can fail
   architecturally.
2. Hull sphere-trace + density march + bisection refine (§3.3). Now it bulges.
3. Normals, depth write, occlusion against the scene.
4. Shading — two specular lobes and an env map, matching
   [`portal2_gels.md`](../games/valve/portal2/portal2_gels.md) §10's numbers as a
   starting point because they are a shipped answer.
5. Thickness → absorption → refraction (§6). This is where it stops looking like
   plastic.
6. Half-res + depth-aware upsample (§8), *measured before and after*.
7. Shadow-pass proxy spheres, if it turns out to matter.

Steps 1–3 are the system. 4–5 are the look. 6 is the only optimisation, and per
CLAUDE.md it does not get written until a measurement asks for it.

## 11. When not to do this

- **Under about twenty blobs that rarely touch**, draw instanced spheres with a
  normal-blend hack and stop. The whole apparatus is unnecessary.
- **If something needs a real mesh out of it** — collision from the *surface*
  rather than the spheres, a decal projected onto it, a physics cloth over it —
  then mesh it, because a marcher has nothing to hand you.
- **If the effect must appear in four or more views.** That is Portal 2's
  situation and it inverts the table in §2.
- **If it fills the screen routinely.** Marching is billed by screen area; a
  wall-to-wall spill is the case where meshing wins outright.

## 12. Open questions

- Does the cluster grid want to be the *lighting* grid (shared build, shared
  froxel layout, one more index buffer) or its own with a coarser resolution
  tuned to blob radii? Sharing is less code and probably the wrong resolution.
- Half-res upsample interacts with the shadow map and SSAO passes that already
  read depth — what order, and does the blob depth belong in the prepass?
- Is the metaball bulge (§3.1) actually wanted for our likely uses — spilled fuel,
  slime, blood pooling — or is the cheaper smin fillet (§3.2) indistinguishable
  once it is wet and refractive? Worth a spike before committing to the density
  path, since smin alone is a much smaller shader.

---

## Sources

- [`portal2_gels.md`](../games/valve/portal2/portal2_gels.md) §6 and §10 — the
  Blobulator's architecture, cvars, shipped shader parameters and error strings.
- [`portal2_portal_rendering.md`](../games/valve/portal2/portal2_portal_rendering.md)
  §12 — why Portal 2's view count is the reason a mesh amortises.
- [`particle_architecture.md`](particle_architecture.md) §9 — the rendering
  section this would extend.
- [`../games/valve/source2_particles.md`](../games/valve/source2_particles.md) §13 —
  overdraw as the real particle cost, and the half-res depth-aware upsample.
- Technique lineage, for the record: Blinn's blobby model (1982) for the kernel
  sum; Wyvill's soft objects (1986) for the compact-support polynomial falloff;
  Hart's sphere tracing (1996) for distance-bounded stepping; Sherstyuk (1999) for
  marching a density field with a geometric bound; Quilez's polynomial `smin` for
  the bounded alternative in §3.2.
