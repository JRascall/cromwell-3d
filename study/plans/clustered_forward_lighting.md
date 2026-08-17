# Clustered forward lighting — what local lights cost, and what they don't

**A design note, not research** — the fifth in this directory, after
`nav_architecture.md`, `bloom_emissive.md`, `console_porting.md` and
`particle_architecture.md`, and the same contract: what cromwell should build,
in what order, with the decisions named and the rejected paths recorded.

Written 2026-08-16, from a decision taken 2026-08-10 that had never been written
down — while **two other notes in this directory were already citing it**.
`blob_raymarching.md` §5 says "the clustered forward lighting grid indexes
lights per froxel; the identical structure indexes blobs", and `raytracing.md`
§6 ranks "local lights, then SSR, then probe GI" as the order that matters more
than ray tracing does. Both were pointing at a plan that existed only in
conversation. This is that plan.

**It is deliberately scheduled AFTER the RHI migration** — see §5.0 for why that
is a sequencing decision rather than an excuse.

**And the phase order is driven by the game, not by the technique.** The usual
telling of Forward+ builds point lights, then the cluster grid, then area
lights, then shadows. This project wants a **dynamic movable spot light** first
and other light types after it, which turns out to be the better order for
reasons that have nothing to do with preference — §2.1 and §5.

---

## 1. Where lighting stands today

Four facts, all checked against the tree as of this note.

**There is exactly one light, and it is the sun.**
`rhi/include/scene_block.glsl` carries `uSunDirection` and `uSunRadiance`, one
each; `rhi/scene/lit.fs.glsl` reads them, evaluates one Cook-Torrance response,
and adds a two-lobe analytic sky. There is no light array, no light count, and
no loop anywhere in the surface shaders. `SunLight.hpp` opens by saying so —
*"the one directional light, and the sky it hangs in"*.

**That is the ceiling on every interior in the game.** A room is lit by the sun
that is outside it, by the analytic sky, and by whatever a reflection probe
captured. A room with no window is lit by the ambient term alone, which is a
flat multiply and reads as one. This is the gap `raytracing.md` §6 puts first in
its ranking, ahead of SSR, probe GI and any form of ray tracing — and it reached
that order from the opposite direction, by asking what RE ENGINE actually does.

**Emissive already exists and it emits nothing.** `.mat` carries
`emissiveColour` and `emissiveStrength`, `MaterialBlockData` carries them to the
shader, and `lit.fs.glsl` adds the term raw after everything else with the
reasoning written out: emission is light *leaving* a surface. A strip light is
therefore a bright patch of pixels and a bloom halo — and the wall beside it
stays exactly as dark as it was. **The bloom work made emissive materials look
like light sources without making them light sources**, and closing that gap is
what this note is for.

**The prerequisites are paid, and that is the surprise worth recording.** Every
piece clustered forward needs already exists on the RHI path, put there for
other reasons:

| Needed | Already there |
|---|---|
| Compute dispatch | `IRenderDevice::beginCompute`, `createComputeShader`, `ICommandEncoder::dispatch` |
| A capability query, not an assumption | `capabilities().compute` — and `beginCompute` returns **null** rather than a silent no-op when it is false |
| Structured buffers | `BufferUsageStorage`, `ICommandEncoder::bindStorageBuffer(slot, buffer)` |
| Read/write images | `TextureUsageStorage`, `bindStorageTexture(slot, texture, mip)` |
| Early-Z, so the light loop is not multiplied by overdraw | The depth prepass; the lit pass tests `Equal` (the probe capture's note says it is the one place that does not) |
| A stage added once existing for every view | `ScenePipeline` is per **view** — the lesson `bloom_emissive.md` P2 had to spend a phase on and this does not |
| A per-view switch for it | `ViewLayers::features`, which `bloom` joined |
| A place for world-scoped GPU data | `RenderScene` owns `DeviceProbeSet` and `DeviceDecalSet`, with the ownership argument already written |

So this is not a capability project. It is a data structure, a compute shader,
a loop in an include, and four phases of consequences.

---

## 2. The decision, in four lines

**Clustered forward (Forward+).** Divide the view frustum into a 3D grid of
froxels — tiles across the screen, slices in depth. A compute pass tests every
light against every froxel and writes a per-froxel index list. Each fragment
computes which froxel it is in from its own depth, and loops **only that
froxel's lights**.

The renderer stays forward. Nothing about the surface shaders' structure
changes; a loop appears between the sun's response and the ambient term.

### 2.1 The light types, and why the spot goes first

| Type | Shading | Culling volume | Shadow | Ships in |
|---|---|---|---|---|
| **Spot** | inverse-square, windowed, × a cone angle falloff | a cone, or its bounding sphere | **one 2D map — a perspective version of the sun's** | P1 / P2 |
| **Point** | inverse-square, windowed | a sphere | a **cube**: six faces, six times the render cost and six times the atlas | P3 |
| **Rect / area** | LTC — a matrix transform and a polygon integral | a sphere sized by intensity | **the hard one.** No single apex to project from; ships unshadowed or approximated by a spot at its centre | P3 |
| Extra directionals | as the sun | the whole frustum | cascades | **not wanted** — one sun, and `ShadowMap.hpp` explains why it needs no cascades either |

**The spot is the right first light, and not because it was asked for.** Four
independent reasons land on it:

1. **It is the only type whose shadow is a 2D map**, so the expensive half of
   the feature starts on machinery that already exists — `DepthTarget`, the
   depth-only caster material shared with the ribbon prepass, and `shadowTap`'s
   compare-then-interpolate filter. A point light's first shadow is six of
   these plus a face selection; starting there means paying the cube tax before
   the flat case is even known to work.
2. **Every other type degrades from it.** A cube shadow face *is* a 90° spot. A
   rect light with no LTC *is* a spot with a wide apex. Implement the spot
   properly and the others are the same shading code with a term removed and
   the shadow lookup swapped — implement the point first and the cone falloff,
   the perspective projection and the apex maths all arrive later anyway.
3. **It is the tightest culling volume of the three**, which makes it the type
   least likely to need clustering at all — reinforcing §10's first question.
4. **It is the one that reads as dynamic.** A searchlight, a torch, a car
   headlight, a UI beam picking out a target: motion is legible because the
   cone's edge is legible. A point light in motion reads as an ambiguous
   travelling glow, which is a much weaker demonstration that the feature works.

## 3. What forward+ costs — asked and answered here so it is not re-asked

The question that produced this note was *"do I lose anything going forward+"*,
and it deserves a permanent answer rather than a second conversation.

**The image is identical.** Forward+ and deferred evaluate the same BRDF with
the same inputs and produce the same pixels. The architecture decides what is
**affordable**, never what it looks like. Anyone describing one as "better
looking" is describing a game that spent the affordability differently.

The four real costs, honestly:

| Cost | How much it bites here |
|---|---|
| **The light loop lives in every lit shader, not in one full-screen pass** | Small. It goes in one file under `rhi/include/` and is included by `lit.fs.glsl`, `transparent.fs.glsl` and `transmission.fs.glsl` — three consumers, and they already share `brdf.glsl`, `shadow.glsl`, `probes.glsl` and `dbuffer.glsl` for exactly this reason. The standing cost is that a change to the lighting model recompiles all of them, and shader permutations grow with material features rather than being decoupled from them. |
| **Material complexity and light count multiply inside one shader** | The genuine loss. A heavy material inside a long cluster loop is one large shader: register pressure, lower occupancy, and the loop's cost scaled by however expensive the material is. Deferred decouples these; forward does not. It is bought back with a quality dial (max lights per froxel), not with cleverness. |
| **Shading cost scales with overdraw** | Covered for opaque geometry by the depth prepass and `Equal` testing. **Not** covered for the transparent pass, alpha-tested foliage, or particles — each layer pays the full loop. This is where a forward+ frame actually goes bad, and it is a content budget rather than a code problem. |
| **Screen-space passes still cannot read albedo or metalness** | Unchanged, not worsened. `GBuffer.hpp` already records that SSR treats every surface as a dielectric because there is no channel spare. Deferred would have fixed this as a side effect; forward+ leaves it as its own decision, which is an attachment when a consumer arrives. |

What is **not** lost: light count (clustered handles thousands), shadowed local
lights (phase 4 is identical either way), MSAA, cheap transparency, per-material
BRDF freedom, or anything in the phase plan below.

---

## 4. Why not deferred

Deferred shortens **none** of phases 1, 3 or 4 below. It replaces phase 2's
froxel cull with a tiled cull, and charges for it:

- **A fat G-buffer.** `GBuffer.hpp` documents having exactly one channel left
  and spending it on roughness. Albedo and metalness need a second colour
  attachment, paid by every pixel every frame.
- **MSAA, or the lack of it.** Deferred wants a temporal answer instead, and
  this renderer's 2× supersample currently reconstructs from nothing and has no
  history buffer to tune — `raytracing.md` §6.6 records that as a virtue.
- **A separate forward path anyway**, for glass, transmission and ribbons.
  Building both is more work than building one.
- **It fights the DBuffer.** The decal design assumes forward lighting: decals
  change what the material *is*, and then the material is lit once with this
  fragment's shadow, probe and occlusion already in hand. That argument is
  written out in `lit.fs.glsl` and it is a forward argument.

And the profile is wrong. Deferred pays off with high light density, uniform
materials and TAA already accepted. A tactics game with an orbit camera, cutaway
interiors, glass and a supersample is the opposite case on all four.

---

## 5. The build order

### 5.0 P0 — this waits for RHI parity, and that is a decision

`MIGRATION.md` §4.13 deletes `FrameRenderer`. Building local lights before that
means writing them twice — and the raylib path structurally **cannot** take the
better half: §1 of that document explains that raylib binds shader inputs by its
own naming convention, so a shader converted to explicit bindings and a `std140`
block cannot be driven by it. A light SSBO is exactly that kind of input.

So: the raylib path never gets local lights, and the device path gets them once,
after parity. The sequencing costs nothing, because every prerequisite in §1's
table is already in place and none of it decays.

### 5.1 P1 — one movable spot, unshadowed, naive loop

The phase that makes lights **exist**. A `DeviceLightSet` on `RenderScene`
(§6), uploaded as one storage buffer; a count in `PassBlock`; a loop in
`rhi/include/lights.glsl` included by the three surface shaders. **No culling at
all** — every fragment loops every light, which at this phase's light count is
the correct implementation rather than a placeholder.

- **Spot first** (§2.1): position, direction, colour, intensity, radius, inner
  and outer cone angles. Inverse-square falloff **windowed to zero at the
  radius** — an unwindowed inverse-square never reaches zero, so every light
  would sit in every froxel forever and the eventual cull would have nothing to
  cull. The cone falloff is `smoothstep` between the cosines of the two angles;
  authoring the angles and comparing cosines is what keeps the per-fragment
  work to one dot product.
- **Movable from the first commit, not retrofitted** — §5.5. The whole buffer
  re-uploads each frame; nothing caches a transform.
- A `local lights` switch on `ViewLayers::features`, because cromwell owns the
  loop.
- A `lights` profiler zone in the same commit — CLAUDE.md's rule, and here it
  is load-bearing: this phase's cost lands entirely inside the lit pass's
  existing zone, so without its own row the frame simply appears to have got
  slower for no attributable reason.
- **Acceptance: a spot light the dev panel can fly around the board**, throwing
  a visible cone across the floor and up a wall, with the panel saying what a
  dozen of them cost. And it will visibly light **through** walls, which is not
  a bug in this phase — it is the demonstration that P2 is necessary.

### 5.2 P2 — a shadow for that spot: one map, no atlas

**Reordered to second, and this is the substantive change from the technique's
usual telling.** The generic plan leaves shadows until last because they are the
expensive part. That is right for a renderer adding fifty static lamps to an
exterior; it is wrong here, because the thing being built is a **movable** spot
and the first thing anyone does with a movable spot is point it at a wall. An
unshadowed light in a cutaway interior — a camera that can see three rooms at
once — lights through walls, floors and ceilings, and it is reported as "the
lighting is broken" rather than as a missing feature.

**It is deliberately ONE map and not an atlas.** A perspective depth target for
the single hero spot, rendered with the existing depth-only caster material,
sampled with the existing `shadowTap`. The atlas — many maps, slot allocation,
a per-frame re-render budget, static-versus-dynamic policy — is a P3 problem
that should be designed once the light *count* and *kinds* are known, not
guessed at N=1. Splitting it this way is what turns "one to two weeks of shadow
work" into a few days now and the rest later, against real requirements.

**The traps are in §7.2 and they are not obvious**, because the existing PCSS
filter is written for an orthographic directional light and says so in its own
header. Read that before starting.

- Acceptance: the spot casts, the shadow moves with it, and the edge does not
  crawl or shimmer as it moves (§7.2).

### 5.3 P3 — point and rect lights, and the atlas that N lights force

Now the type table (§2.1) is filled in, and the shadow work generalises rather
than being invented:

- **Point** — the same shading with the cone term dropped, and a **cube** shadow:
  six faces, six times the render cost and six times the atlas footprint. This
  is where the atlas has to exist, and where a re-render budget per frame stops
  being optional.
- **Rect / area, via LTC** — Linearly Transformed Cosines: the response becomes
  a matrix lookup plus a polygon integral, at a cost close to a point light's.
  This is what makes a window, a strip light or a screen read as a *surface*
  emitting rather than a point pretending to be one, and it is the type that
  finally connects to the emissive materials the bloom work already ships. It
  needs two lookup textures as assets (§10). **Ships unshadowed or approximated
  by a spot at its centre** — a true area shadow has no single apex to project
  from, and `raytracing.md` records that RE ENGINE did not have one until its
  third version.

The ordering inside this phase is the game's to choose: rect lights buy more
look for less shadow work, point lights buy more coverage for more.

### 5.4 P4 — the cluster build, in compute, if the count ever asks

Two dispatches: build this view's froxel AABBs (only when the projection or the
grid dimensions change), then cull lights into per-froxel index lists every
frame. The surface shaders swap `for (all lights)` for
`for (this froxel's lights)` — a change to two lines of the include and nothing
else, which is the entire reason P1 puts the loop in one file.

- Log-distributed depth slices, not linear (§7.1).
- Both buffers are `BufferUsageStorage`, reached through `bindStorageBuffer`.
  No new interface.
- Acceptance: the same scene at ten times the lights, at a frame cost that grows
  with lights *visible per froxel* rather than lights *in the world* — measured
  both ways, or the phase has demonstrated nothing.

**This is last, and it may never be built.** See §10. Moving it here is not
deferral for its own sake: with the spot going first the light count stays small
for a long time, and a naive loop over a dozen lights is not a stepping stone to
a cluster grid — it is the finished feature. The grid gets built when a
measurement asks for it, or when `blob_raymarching.md` §5 wants the same
structure for something else.

### 5.5 What "dynamic and movable" specifically costs

Worth separating out, because it is a requirement rather than a phase and it
changes decisions in all four.

**Upload the whole light buffer every frame.** Not dirty-tracking. At a dozen
lights it is a few hundred bytes, and a dirty-tracking scheme is a second
description of the truth that can disagree with the first. This is the decision
the decal path already made — one upload per frame, bind per draw — and for the
same reason.

**A moving light's shadow map cannot be cached, and that is what actually
limits the count.** A static lamp renders its map once and keeps it forever; a
moving spot re-renders every frame, which means a full scene draw per shadowed
moving light per frame. **The limit on shadowed dynamic lights is a scene-draw
budget, not a shading budget** — which is the opposite of where the intuition
points, and it is why P3's atlas needs a policy (static slots that persist,
dynamic slots that re-render, and a per-frame cap on the latter) rather than
just an allocator.

**A moving light gets no transmission tint.** `sunShadow` returns a *colour*,
not a fraction, because light through a stained window is dimmer and differently
coloured — and it gets that from a second target, the shadow transmission plane,
rendered at half the shadow resolution. Giving every local light one doubles its
shadow cost for an effect that only reads where there is glass between the light
and the surface. **Local lights return a scalar; the sun keeps its colour.**
Write that down rather than discovering it as an inconsistency.

---

## 6. Where things live — decided by precedent, not by argument

The three lifetimes are already established by the probes and the decals, and
lights answer them identically:

| Thing | Owner | Why |
|---|---|---|
| The lights themselves — `DeviceLightSet` | **`RenderScene`** | A lamp is in a *world*, not in a viewpoint. Two players in one building must not each upload the same lights. This is verbatim the argument `RenderScene.hpp` makes for moving `DeviceProbeSet` off `ScenePipeline`, and the correction it records is one this feature should not have to repeat. |
| The froxel grid and the index lists | **`ScenePipeline`** | A cluster grid is a subdivision of *this view's frustum* and means nothing in another. Per view, like every other target it owns. |
| The tuning — max lights per froxel, grid dimensions | The renderer, edited in the dev panel | Same arrangement as `BloomTuning` and `AmbientOcclusion::Tuning`. |
| Which lights exist, where they are, and what they mean | **The game** | A muzzle flash, a fire, a window's spill: all gameplay. The engine takes a list and never learns why. |

**The engine/game seam, stated once:** the GPU buffer, the culling and the
shading are cromwell's; light ownership and behaviour are the game's. That is
`MIGRATION.md` §0's test — a new project would write the buffer and the cull
again unchanged, and would write its own reasons for a light to exist.

**One portability note that will otherwise be copied by accident:**
`SunLight.hpp` includes `raylib.h`. Whatever `LocalLight` type this introduces
must not — `console_porting.md` §3.2 is about exactly this shape of leak, and a
new lighting type added after the RHI landed has no excuse for it.

---

## 7. The decisions that get made wrong if they are not made here

### 7.1 The lights and the grid

**Log-distributed depth slices.** A linear split of near-to-far puts almost
every froxel in the far distance where nothing is, and lumps the entire
foreground into slice zero. The orbit camera's depth range is large and its
subject is close. Use the standard exponential distribution
(`slice = log(z/near) / log(far/near) * slices`); the near and far this needs
are already computed for the shadow focus.

**Every view builds its own grid, including the 128² probe capture.** That is a
consequence of `ScenePipeline` being per view, and it is correct — but it means
the capture pays a cluster build for a face 128 pixels wide, which is cheap and
should simply be allowed rather than special-cased.

**Local lights should be ON in a probe capture, unlike probes themselves.**
`scene_block.glsl` sets `uProbeParams.x = 0` during a capture because a texture
cannot be its own attachment and sampler, and because a probe reflecting probes
compounds its own error. **Neither reason applies to lights.** A light is not a
render target, there is no recursion, and a strip light's contribution to what a
window reflects is precisely what a probe is for. Switching lights off in the
capture would make reflections systematically darker than the surfaces beside
them, which reads as a probe brightness bug.

**The transparent pass loops lights too.** It already shares the sun, the shadow
map, the sky and the BRDF with the opaque pass for the stated reason that a
lit wall and the pane of glass in front of it must not disagree. A window lit
by the sun alone while the wall behind it has a lamp on it is the same class of
bug, and a more visible one.

**The loop goes in an include from the first line of P1.** Not written out in
`lit.fs.glsl` and copied to the other two later. `scene_block.glsl`'s header
records that the same list was edited in two files twice, and both times a link
error caught what review did not.

**A max-lights-per-froxel cap is a quality dial with a visible failure mode.**
When it overflows, lights vanish — and which ones vanish depends on the camera,
so it presents as flickering. Log the overflow once by count (the pattern
`RenderScene` already uses for undrawable renderables), and put the cap in the
panel so the failure is diagnosable rather than mysterious.

### 7.2 The spot shadow — three places the sun's filter does NOT transfer

`rhi/include/shadow.glsl` is a good filter and roughly half of it is
directional-specific. It says so itself, which is why this is a short list
rather than a research project — but each item silently produces a plausible
wrong picture rather than an error.

**The PCSS penumbra formula inverts.** The header states it outright: *"THE
TEXTBOOK FORMULA DOES NOT APPLY: (receiver − blocker) / blocker comes from
similar triangles under a PERSPECTIVE light. The sun is directional and its map
orthographic, so depth is linear and there is no apex."* A spot **is** a
perspective light with an apex, so for it the textbook formula is the correct
one and the sun's `2 × distance × tan(angular radius)` is the wrong one. The
two are not interchangeable and the note in that header reads, at a glance, as
though it settles the question for both. It settles it for one.

**Depth is no longer linear.** A perspective projection crushes the far half of
the depth range into a sliver, so the blocker search's `(projected.z −
averageBlocker) × depthRange` — an honest world distance under an orthographic
map — becomes meaningless under a spot. It needs unprojecting, or the whole
comparison needs doing in view space.

**The world size of a texel varies across the map.** `uShadowScales.y` is one
number because an orthographic projection has one answer; a spot's texel
footprint grows with distance from the apex. Both the **normal-offset bias**
(`normal × worldTexel × (1.5 + 3.0 × slope)`) and the penumbra floor depend on
it, so a single constant gives acne near the light and peter-panning far from
it — which reads as "the bias needs tuning" and cannot be fixed by tuning.

**And the texel-snap trick is simply unavailable.** The sun's map is snapped to
texel increments so the shadow does not crawl as the camera moves; that works
because translating an orthographic projection by a whole texel is a no-op on
the picture. A spot that **rotates** has no equivalent — there is no increment
to snap to. A moving spot's shadow edge will shimmer, and the answers are
resolution, filter width, and accepting some of it. Deciding that in advance is
cheaper than diagnosing it as a bias bug later.

**What DOES transfer, unchanged:** `shadowTap`'s compare-then-interpolate (the
header explains why a `sampler2DShadow` gave blocky plateaus and speckle), the
rotated Poisson disc and its second ring, and the depth-only caster material.
That is most of the file's hard-won content and none of its geometry.

---

## 8. Costs, so the choice is informed

**Memory** is trivial and worth stating anyway. A 16×9×24 froxel grid is 3,456
clusters; at a 32-bit offset+count each that is 27 KB, plus an index list
bounded by `clusters × maxLightsPerCluster` (about 110 KB at 32). The light
buffer itself is a few dozen bytes per light. Nothing here is a memory decision.

**Time** splits in two, and only one half is what people expect:

- The **cull dispatch** is small — thousands of froxels against tens of lights,
  once per view per frame. It will not be the cost.
- The **loop in the surface shaders** is the cost, and it is paid per fragment
  per light per layer. Which is why the overdraw row in §3 is the one to watch,
  and why the dial in §7.1 exists.

**And the third cost, which only arrives with §5.5:** a shadowed **moving**
light is a full scene draw per frame. That is not a shading cost at all, and it
is the one that sets the ceiling on how many dynamic shadowed lights a frame can
hold.

Per CLAUDE.md, the split gets **one** zone until a measurement asks for more:
`lights` for the cull. The shading half already lives inside `lit scene` and
should stay there until it is a big enough slice to earn a sub-zone.

---

## 9. Rejected, and why

- **Deferred shading.** §4. The short version: it shortens three of the four
  phases not at all, and charges a G-buffer attachment, MSAA, a second forward
  path and an argument with the DBuffer for the one it does shorten.
- **Tiled (2D) instead of clustered (3D).** Tiles cull against a depth range per
  tile, which collapses wherever one tile spans a near object and a far one —
  the silhouette of every unit against the street, in a game whose camera is
  always looking down a diagonal. Clustered costs one more dimension in a buffer
  measured in kilobytes.
- **Per-object light lists — the old forward way, four lights per mesh.** It
  fails structurally on this content rather than merely being crude: the statics
  are **one mesh per storey**, and the visibility overlay is too (`MIGRATION.md`
  §4.4). A per-object list would assign four lights to an entire floor of a
  building. Per-fragment culling is not an optimisation here, it is the only
  thing that answers the question at all.
- **Linear depth slices.** §7.1.
- **The technique's usual phase order** — point lights, cluster grid, area
  lights, shadows. It is written for a renderer adding many static lamps to an
  exterior. This project's first light is a **movable** one and its interiors
  are cut away, so leaking through walls arrives on day one rather than at
  scale; and the point light's cube shadow is six times the work of the spot's
  flat one, which makes it the wrong place to learn whether the shadow works at
  all. §2.1, §5.
- **A shadow ATLAS first.** Distinct from the above, and still rejected: a slot
  allocator, a static/dynamic policy and a re-render budget designed at N=1 are
  three guesses about requirements that P3 is what actually produces. P2 builds
  **one map**, which is days rather than a fortnight and is the part that
  generalises.
- **Doing any of this before RHI parity.** §5.0.
- **Building the cluster grid on the assumption it is needed.** §10.

---

## 10. Open questions

- **How many lights does this game actually want?** This is the question that
  decides whether P4 is ever built. A tactics board wants: window spill, a fire
  or two, muzzle flashes, an explosion's flash, maybe a room's lamp. That is a
  *dozen*, not a thousand — and at a dozen the P1 naive loop is not a stepping
  stone, it is the finished feature, and P4 exists mainly to serve the blob
  marcher (`blob_raymarching.md` §5) and whatever comes after. **Do not build
  the cluster grid until a measurement or a content plan asks for it.** Building
  it because the technique is called "clustered forward" is the exact mistake
  CLAUDE.md's measurement rule exists to prevent.
- **Does the spot want a cookie?** A projected texture in the cone — a gobo, a
  window's mullions, an IES profile measured off a real fixture — is one texture
  sample inside a loop that has *already* computed the light-space position for
  the shadow lookup, so it is nearly free once P2 exists, and it is most of what
  makes a spot read as a **fixture** rather than as a cone. It is also the point
  at which a light starts carrying an asset, which is the authoring question
  below arriving earlier than expected.
- **Whose spot is it?** A dev-panel light flown around the board proves the
  feature; a unit's torch, a searchlight on a tower, or a beam marking a target
  is a *gameplay* light with an owner, a lifetime and a reason to move. The
  engine takes a list either way (§6), but which exists first decides whether
  P1's acceptance test is a panel slider or a game system — and P2's shadow
  budget (§5.5) depends on how many of them move at once.
- **Do transient effects become lights?** A muzzle flash lasting one frame is a
  light that pops. A grenade's flash over six frames is a light that needs a
  falloff curve and an owner that outlives the frame. This is a particle-system
  question as much as a lighting one — `particle_architecture.md` reserves no
  attribute for it — and answering it late means answering it twice.
- **Is a light an authored asset?** A `.light` file, placement in a dev tool,
  or purely code-spawned? `material_extensibility.md`'s position is that a
  game's look should be authorable as content; a lighting rig is a large part of
  a look, and a rig that can only be spawned from C++ is a rig only a programmer
  can iterate on.
- **The LTC lookup tables are shipped assets.** Two 64×64 float textures, from
  the reference implementation. Which means a decision about generated-versus-
  shipped data that `assets/` has not had to make before.
- **How this interacts with the sun bake.** `SunBaker` bakes the sun's
  visibility; local lights are dynamic and unbaked, so a baked surface and a
  dynamically lit one sit side by side. Probably fine — they are different terms
  — but it has not been thought about, and the shelved bake post-mortem in
  `source2_rendering.md` is the place to think about it from.
