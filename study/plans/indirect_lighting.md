# Indirect lighting — the irradiance probe grid, and what it does not fix

**A design note, not research** — the tenth in this directory, and the same
contract as the rest: what cromwell should build, in what order, with the
decisions named and the rejected paths recorded.

Written 2026-08-16, reading from three notes that all point at this and none of
which is it. `re_engine_rendering.md` §4 is the reference material and §11.2 the
recommendation; `source2_rendering.md` §11 took the decision — *"indirect light
comes from an incrementally-updated irradiance probe grid"* — and never said how;
`raytracing.md` §6 ranks it third and explains what it is third *behind*.

**Three things this note settles that none of those did.** They disagreed
without noticing:

- `source2_rendering.md`'s shelved-bake post-mortem proposes an **ambient /
  sky-visibility lightmap** as the thing to revive, and its own §11 decides on a
  **probe grid**. Those are two different answers to one question, written
  fifteen hundred lines apart in the same file. §3 below picks one.
- §11 says leak rejection "is a lattice query, not a heuristic" and never says
  what the query is or who asks it. §5 makes it concrete, and it turns out to be
  three bits per cell.
- Nobody checked that the tracer this all rests on is in `game/`, not
  `cromwell/`. §8.

**It is scheduled after clustered forward lighting, and that is a dependency
rather than a preference** — see §7.0. Shipping sky occlusion first makes every
interior correctly dark with nothing in the world able to light it.

**This is an RHI-path note and the raylib path is not a consideration anywhere in
it.** `MIGRATION.md` has always said the parallel path is *"chosen at startup,
deleted at parity"*; the deletion is now the plan rather than an eventuality, so
nothing below is qualified with what `common/environment.glsl` or `PbrShader`
can or cannot do. Where a raylib file is quoted it is quoted as history.

---

## 1. Where indirect light stands today

Five facts, all checked against the tree as of this note.

**Diffuse indirect is one function of the normal, and it is the same function
everywhere in the world.** `rhi/scene/lit.fs.glsl:180` is
`diffuseAlbedo * skyIrradiance(normal)`, scaled at :216 by
`uExposureAndAmbient.y * ao`. `rhi/include/sky.glsl` is a horizon/zenith/ground
lerp on `direction.y` — a two-lobe hemisphere. **A sealed interior room receives
exactly the irradiance an open field does.** That is the gap this note exists to
close, and it is the largest remaining one in the frame.

**The ambient intensity is a global fudge factor and is documented as one.**
`probes.glsl:272` names it: *"lobe colours that need a fudge factor to land at a
plausible irradiance — that factor is `uExposureAndAmbient.y`, currently 0.42."*
One scalar tuned for the average of the whole map, applied to every surface in
it.

**Specular indirect is genuinely finished, and better than the notes say.** The
reflection probe half is done: `DeviceProbeSet` owns a cubemap array, one probe
per room, two volumes per probe, per-pixel selection with explicit priority and
a doorway crossfade, parallax correction against the room's own box, and — the
part the research notes predate — **a six-level GGX-prefiltered chain**
(`rhi/scene/probe_prefilter.fs.glsl`, `PROBE_MIP_LEVELS 6`). The roughness fade
to analytic sky is deleted; `probes.glsl:305` records the deletion as the
prefilter's payoff.

> **That closes `re_engine_rendering.md` §11.3.1 steps 1 and 2 outright.**
> §11.3.1 was written against `common/environment.glsl`'s claim that no mip
> chain could exist, and reasoned about it as a live constraint on a second
> renderer. It is not one: that file is going away with the rest of the raylib
> path, so the only remaining question in §11.3.1 is step 3 — octahedral
> storage — which was never about the mip chain and is not urgent.

**The tracer already exists, is measured, and is not the engine's.**
`game/los/RayCaster.hpp` is a 3D DDA over the lattice with `RayRules` already
separating `Sight` from `Sunlight` — the exact predicate split a GI trace needs,
built for a different reason. `game/light/SunBaker.hpp` already drives it
multithreaded over the whole map, already has an incremental
`bakeRegion(sun, centre, radius)` that collects the shadow shaft of changed
cells, and the shelved bake measured it at **~43 Mray/s**. Both live under
`game/`. §8.

**There is no irradiance probe of any kind.** No storage, no field, no sampler,
no upload. This is a new system, not an improvement to one.

---

## 2. The decision, in four lines

**An irradiance field on the lattice: one probe per cell, four directions per
probe, traced on CPU against the existing `RayCaster`, uploaded as four small
3D textures, sampled by an eight-tap gather with exact leak rejection.**

24 × 24 × 9 = **5184 probes** on the default map. Four directions at
R11G11B10Float is 16 bytes a probe, so the entire world's indirect light is
**83 KB** — small enough to hold two copies and blend, small enough to upload
every frame without thinking about it.

**No bake, no tetrahedralisation, no BSP, no placement pass.** Capcom's RE7
system spends its whole build budget — voxelise, seed sparsely, tetrahedralise,
build a BSP, then build a 32³ uniform grid *to accelerate queries into the BSP*,
80 seconds offline — on the single problem of probes sitting at irregular
positions in a world made of triangles. Ours sit one per lattice cell, so the
lookup is `(z * height + y) * width + x`. **We adopt the part that works and
skip the part that cost them the effort**, which `re_engine_rendering.md` §11.2
calls the most valuable instance of an argument it makes three times.

And Capcom themselves retreated to nearly this. By Monster Hunter: World the
automatic voxel placement was gone, the BSP was gone, and what remained was
irradiance on a **uniform grid** (§4.1.1). **We start where they finished.**

### 2.1 What it covers, and what it does not

Worth stating plainly, because "realtime GI" is heard as "lighting is solved"
and this is one term of many.

| | |
|---|---|
| **Sky occlusion** | ✅ The main event. An interior stops receiving open-ground irradiance. This is the worst-reading artefact in the frame today. |
| **Bounce and colour bleed** | ✅ Light through a window fills the room instead of making one bright patch and stopping. |
| **Units, props, particles, glass** | ✅ From the same field, trilinearly. Source 2 needs a *separate* probe build for mesh entities because its world half is lightmapped; one field serves both here. |
| **Multi-bounce** | ✅ Free — probes gather other probes' previous values, so one-bounce cost gives a many-bounce result. §7.4. |
| **Sun shadows** | ❌ Direct sun, still the shadow map. Not touched, not improved, not made redundant. |
| **Contact-scale darkening** | ❌ A cell is ~1.5 m × 1.5 m × 1 m. The gradient into a doorway at sub-metre scale is SSAO's job and stays SSAO's job. §2.2. |
| **Bounce from local lights** | ❌ Deferred, deliberately, and it is the largest known limitation. §11. |
| **Specular indirect** | ❌ Already solved by the reflection probes (§1). This field improves them (§7.5) but does not replace them. |

### 2.2 The occlusion bands — and there are two, not three

Stated here because it settles a question `re_engine_rendering.md` §11.1 got
wrong, and because the answer is a property of the techniques rather than of
this map.

| band | owner |
|---|---|
| **below cell scale** — creases, contacts, the line where a crate meets the floor | **SSAO**, at depth-buffer resolution. Nothing else reaches it: a field at one value per cell cannot represent a crease, and no increase in field resolution is going to. |
| **cell scale and above** — a room being enclosed, a courtyard under an overhang, an interior that stops receiving open-ground irradiance | **This field.** It answers the same question *directionally and in colour*, with bounce, rather than as a scalar multiply on an ambient term that was already the wrong colour. |

**There is no third band, so there is no SDF AO.** §11.1 ranks SDF ambient
occlusion as its first payoff and "the cheapest step"; between SSAO below and a
traced irradiance field above there is nothing left for it to do but compute a
degraded, colourless version of the field's answer at the field's own
resolution. That section now carries a re-ranking note. The SDF's *shadow* case
is untouched and is a separate feature.

**And judge that at large-map scale, not against this board.** cromwell's target
is RTS, FPS and third-person, and the tile game is a prototype that exercises
engine features rather than the content the engine is aimed at — so "bounded
interiors, therefore X" is not an argument the engine may accept. The band split
survives the change of scale because it is about technique resolution: SSAO's
band is set by the depth buffer and the field's by its own cell size, on any map
of any size. What does *not* survive unchanged is everything about how the field
is built and stored — §8.1.

---

## 3. Why a grid and not an ambient lightmap

`source2_rendering.md`'s shelved-bake post-mortem argues, persuasively, that an
**ambient / sky-visibility bake** is the piece of `SunBaker` worth reviving:

> "The direct-sun bake is the *least* forgiving thing to precompute — it is high
> frequency, and it must be fresh the instant a wall falls. An ambient /
> sky-visibility bake inverts both … The `RayCaster`, the patch list, the atlas
> packing and the index texture are all reusable as they stand."

Every word of that is right and it still loses, for a reason the same
post-mortem supplies three paragraphs earlier. Its **first named suspect** for
why the sun bake looked wrong despite measuring well:

> "**Only cell faces are baked.** Props, crates, ramp treads, ladders and
> blocked mass have no patch and stay on the shadow map. A frame therefore
> mixes baked and shadow-mapped shadows with different softness … which reads
> as *inconsistent* whatever the quality of either half."

**An ambient lightmap reproduces that failure exactly.** Its patch key is
`(cell, face)`, five faces per cell, so it can light the floor and the walls and
it cannot light a crate, a unit, a ramp tread, a particle or a pane of glass.
Those would keep the flat 0.42 hemisphere while the walls beside them went dark,
and the seam between the two would be the artefact — the same artefact, from the
same cause, in the same shape.

A volumetric field has no such split. Everything in the world reads the same
structure by asking where it is.

| | verdict |
|---|---|
| **Irradiance grid, one probe per cell** | **Chosen.** One structure lights static geometry, dynamic objects, props and particles alike. No parameterisation, no atlas, no index texture, no patch list, nothing to keep in step with the render mesh. |
| Ambient lightmap on cell faces | Higher spatial resolution on the surfaces it covers, and it covers only surfaces the lattice can name. Reproduces the sun bake's first failure candidate. Rejected. |
| Both | The grid could be resolution-boosted on static faces later. Not now — one system, and see whether it is short of resolution before building a second. |
| Screen-space GI | Cannot see offscreen geometry, so it cannot darken an interior; it does not address the problem. Possibly worth adding later for contact-scale bounce, where it is genuinely good. |
| Voxel cone tracing | The world *is* a voxel grid so the structure is free, but cone-traced specular is expensive and leak-prone and buys little the grid does not. |
| Any full bake | The destructible map is the whole objection. Re-bake is a blocking cost that grows with blast radius and reached 1.6 s in the shelved bake's stress measurements. |

**The grid has nothing to invalidate**, and that is the property doing the work.
Probes are re-traced whether or not anything changed, so destruction is not an
event the lighting has to *handle* — at most, probes near the blast go to the
front of the queue and the hole fills with light over the next handful of
frames. Behind a detonation animation on a turn-based board, that convergence is
not perceivable.

---

## 4. Storage — four directions, and why not SH3

**Four directional colours, each R11G11B10Float. 16 bytes a probe.** Capcom's
RE7 choice, and the one `re_engine_rendering.md` §11.2 picks.

The reflex choice is third-order spherical harmonics — 9 coefficients per
channel, 27 floats — and it is what Capcom themselves moved *toward* by Monster
Hunter: World (§4.1.1). Four reasons not to follow them:

1. **Size, by a factor of seven.** 16 bytes against 108 (or 54 at fp16). 83 KB
   for the world against 560 KB. At 83 KB the upload is free and two copies fit
   in cache; at 560 KB neither statement survives.
2. **Fetch count in the shader.** Four directions is four texture reads per
   probe. SH3 is seven (27 coefficients over RGBA). §5's gather multiplies
   whatever this number is by eight.
3. **No ringing, so no window function.** SH's HDR failure mode is a dark rim
   around a bright source, and the fix — a Gaussian, Hanning or Lanczos window,
   which MHW names — is a tuning parameter that trades the sharpness you bought
   the extra coefficients for. Four directional lobes cannot ring and cannot go
   negative, so **a probe can never darken a surface**, which SH can and does.
4. **Evaluation is four dots and a normalise.** No basis evaluation, nothing to
   get wrong, nothing to profile.

**And the objection those four do not answer: is four lobes simply too crude?**
Every reason above is *a priori* — arithmetic about bytes and fetches, which
says the format is cheap and says nothing about whether it is enough. The answer
is empirical and it is the strongest single piece of evidence in this note:
**Resident Evil 4 Remake shipped its indirect lighting on exactly this structure
in 2023, with ray-traced GI switched off entirely** (`re_engine_rendering.md`
§4.4, §11.2), and it is the best-looking title in the series. Four directional
colours per probe is not a compromise this note is talking itself into; it is
what carries a shipped frame that comfortably outclasses ours. If the field
looks flat after P3, suspect the tracer's energy before suspecting the lobe
count — §11's last question exists for that reason.

**And one argument for SH that turns out not to be one.** §4.1.1 lists MHW's
"time of day is a linear interpolation between coefficient sets" as an SH
property. It is not — it is a property of *any linear projection of the
lighting*, and four directional lobes are one. A day-night cycle costs one lerp
either way. Recorded because the note presented it as a differentiator and it
should not be read as one.

**Where the four directions point.** Capcom do not say. A regular tetrahedron is
the isotropic answer; this world is not isotropic, so orient it with **one lobe
at +Y and three splayed at −19.47° elevation, 120° apart**. Light in a tactical
map arrives from above and bounces laterally, and the up lobe is the one every
floor and every unit's shoulders read. Treat the orientation as tunable and the
lobe count as not.

**Layout.** Four 3D textures of `width × height × depth` in R11G11B10F, or one
array of four slices — not one texture of `4 × width`, which makes the address
arithmetic carry a stride nobody will remember. Two fields, current and target,
so a re-trace crossfades in rather than popping.

---

## 5. Leak rejection is the part not to skimp on

**Two independent engines name light leak as the thing that kills probe GI.**
Capcom's own 2016 verdict on their network is unusually blunt — interpolation
between probes is *"not beautiful — just like vertex colours"*, and light leaks
are worked around **by moving the probe by hand**. By RE8 the engine has shipped
`LightProbeBlocker` and `LightProbeBlockerHole` types, which is exactly what you
build after complaining about leaks for five years. `source2_rendering.md` §11
reaches the same conclusion about DDGI independently.

Both of Capcom's complaints are consequences of **irregular probe placement in a
triangle world**: interpolating across a tetrahedron is what looks like vertex
colour, and hand-moving a probe is what you do when you cannot say which cell it
belongs to. Neither applies on a regular lattice. But leaking through a thin
wall does, and it is the one that reads worst — a sealed room glowing along the
wall it shares with the street is more obviously wrong than the room being
uniformly too bright, which is the bug today.

### 5.1 The query, made concrete

§11 claims the rejection can be *exact* here because the solid cells are known.
It never says who asks. The shader does, and it cannot afford a DDA — so give it
the answer in a texture.

**Three bits per cell: is the +X, +Y or +Z edge blocking?** That is the lattice's
own edge record, which already exists and already knows about walls, windows and
full-cover edges, reduced to what light cares about. One byte per cell, 5184
bytes for the map, resident in L1 forever.

Then the gather:

```
for each of the 8 probes around the sample point:
    w  = trilinear weight
    w *= max(0, dot(normal, directionToProbe))     // DDGI's backface term
    w *= edgesClear(sampleCell, probeCell)         // 0..3 bit lookups
```

**The tap is always in the sample point's 2×2×2 neighbourhood**, so the "ray"
from sample cell to probe cell is at most one step on each axis and
`edgesClear` is at most three bit tests — no traversal, no loop bound to argue
about, no Chebyshev test, no per-probe depth map. That is what §11 meant by
"cheaper and exact", spelled out.

Plus two supporting rules, both cheap:

- **Probes in solid cells are dead**, marked at trace time and given zero
  weight. If all eight taps are dead, fall back to the analytic sky rather than
  to black — a hole in the field must read as "not known yet", never as an
  unlit void.
- **Sample at `worldPosition + normal * step`**, exactly as `probes.glsl` already
  does for reflection selection with `kProbeSurfaceStep = 0.25`. A wall is 0.09
  tiles thick, so its two faces are 0.045 apart with a room boundary between
  them; testing the raw position is a coin toss that flickers along every wall.
  The reflection path already learned this and the argument transfers verbatim.

### 5.2 The cost, and the escape hatch

Eight taps × four directions is **32 texture fetches per shaded pixel**, against
the one arithmetic evaluation of `skyIrradiance` it replaces. The working set is
83 KB + 5 KB and never changes within a frame, so it is a cache-resident fetch
rather than a memory one — but 32 of anything per pixel is a real number and
this replaces something that was free.

Hardware trilinear on the 3D texture would make it 4 fetches and is **the wrong
trade**: it filters across walls by construction, which is the entire bug. Do not
take it.

**If it measures badly, the escape hatch is resolution, not correctness:**
evaluate the field at half resolution into a screen-space irradiance buffer and
bilaterally upsample it. Indirect diffuse is low frequency, which is why this
works and why the same trick is already in the tree for SSAO. Do it if the
measurement asks, not before.

---

## 6. Where the tracing happens — CPU, and the reason is the readback

**CPU, against the existing `RayCaster`, multithreaded as `SunBaker` already
does it.** Not compute, and the argument is not "compute is hard" — the RHI has
`beginCompute`, `createComputeShader` and `dispatch`, and a DDA over a 5 KB
solidity texture is fifty lines of GLSL.

It is **multi-bounce** that decides it. Probes gather from *other probes'*
previous values where a ray lands, so the tracer must read the field it writes.
With the field authoritative on the CPU, that read is an array index and the GPU
gets an 83 KB upload it never reads back. With tracing in compute, the field is
authoritative on the GPU and the CPU needs it back — for a dev overlay, for a
gameplay query about how lit a cell is, for anything — and CLAUDE.md's GPU note
is exactly this: *"the readback is usually the problem, not the dispatch."*

**And GL has no second queue, which is the argument the readback one overshadows.**
Capcom's figures for this class of work — 1.5 ms to sample the probe network,
2.5 ms for SDF shadows — are all quoted as *overlapped*, running on an async
compute queue behind the shadow pass, because a console lets them. GL has one
queue: `dispatch` orders against the draws around it, so every millisecond of a
compute tracer lands on the critical path in full. **A CPU tracer is the only
configuration here that genuinely runs concurrently with the GPU frame.** That
is a property of the API rather than of this map, so unlike everything else in
this section it does not soften at larger scale — it changes only if the
renderer gains a second queue, which means Vulkan, not a bigger world. §8.1's
"compute tracer + GPU field" substitution should be read with that attached: it
is the right answer when the world outgrows a CPU, and it is still buying
concurrency it will not get until the RHI has a queue to put it on.

Three supporting reasons:

- **The work is already written.** `RayCaster` with `RayRules::Sunlight`,
  `SunBaker`'s thread pool, and `bakeRegion`'s shadow-shaft collection are the
  three pieces a probe tracer needs, and all three exist and are measured.
- **The world is 5184 cells and the sun is one light.** This is not a problem
  that outgrows a CPU. `raytracing.md` §7 already flags that the
  lattice-as-acceleration-structure argument holds *because* the world is 24×24
  and mostly architecture — and evaporates for a project with streamed instanced
  meshes. A future genre that breaks that assumption changes this decision; the
  storage and sampling halves survive it unchanged (§8).
- **A static world converges and then costs nothing.** DDGI assumes everything
  moves and pays every frame for it. Here the field is only wrong when geometry
  changes, the sun moves, or a probe has never been traced. That makes the
  schedule event-driven with a low-rate background sweep, not a fixed per-frame
  budget — §7.

**The steady state is therefore zero.** That is the number to protect in review.

---

## 7. The build order

### 7.0 P0 — this waits for clustered forward, and that is a dependency

`raytracing.md` §6 ranks local lights first and probe GI third. That ordering was
reached from the question "what makes this frame unlike RE4R's", and it is right
for a second reason nobody wrote down:

**Sky occlusion makes interiors correctly dark, and today there is nothing in
the world that could light them.** One directional sun and a flat hemisphere is
the entire light rig. Ship P1 first and a windowless room goes from wrongly lit
to correctly black, which is a *worse* frame and an unshippable one. Local lights
are what make a dark interior a lit interior rather than an empty one.

So: **clustered forward lands first**, which itself waits for RHI parity. This is
not a queue, it is a chain.

### 7.1 P1 — the field exists and changes nothing

`DeviceIrradianceField`, the four 3D textures, the scene-block parameters, the
`rhi/include/irradiance.glsl` sampler, the dev-panel toggle, the upload, the
profiler zone. **Filled entirely from `skyIrradiance`** — every probe gets the
analytic hemisphere projected onto its four lobes.

The frame is pixel-for-pixel what it was. That is the acceptance test: if
anything moves, the projection or the reconstruction is wrong, and finding that
out here rather than three phases later is the whole point of spending a phase
on it. This is where the gather, the edge-bit texture and the dead-probe path
get written and proven against a known answer.

### 7.2 P2 — sky occlusion, and this is the big one

Trace each probe's four lobes for **sky visibility only**: fire rays, count what
escapes the lattice, weight by lobe. No hit shading, no bounce, no feedback —
`RayRules::Sunlight` and the DDA, which is what `SunBaker` already does with the
sun direction substituted for a hemisphere.

**Interiors go dark.** This single phase is most of the visual value in this
note, and it needs none of the machinery in P3–P5.

**Leak rejection ships here, not later.** It is tempting to defer §5 as an
optimisation of a working feature; it is not one. Leak is invisible while
indirect light is spatially uniform, and becomes the most obvious artefact in
the frame the moment it is not. P2 is the phase that makes it not.

### 7.3 P3 — one bounce

Rays that hit a surface now shade it: `albedo × (sun × visibility + sky)`. Colour
bleed appears — a red wall tints the floor beside it, light through a window
carries the frame's colour into the room.

The material data this needs is albedo per cell face, which the lattice can
answer and the render mesh already knows.

### 7.4 P4 — multi-bounce, for free

Change one line: where a ray hits, gather the **previous frame's probe
irradiance** at the hit point instead of only the direct term. Cost is
identical; the result converges to many bounces over a handful of frames. This is
DDGI's central trick and the reason a one-bounce budget is not a one-bounce look.

Watch for the usual failure: energy compounding without a loss term makes a
white room brighten every frame until it blows out. Clamp the feedback and keep
albedo honest.

### 7.5 P5 — relight the reflection probes

The four-line renormalisation from `re_engine_rendering.md` §4.2, attributed
there to Lazarov 2014:

```
IndirectSpecular = luma(ProbeDiffuse)
                 * luma(LocalCubemap(r, mip))
                 / luma(LocalCubemap(n, lowestMip))
```

Take the cubemap's **shape** and rescale it by the **level** the irradiance field
says this point is actually receiving. The capture supplies directionality, the
field supplies intensity.

Nearly free once the field exists, and it fixes a real current bug: a cubemap
captured in one lighting condition and sampled in another makes every surface
read as metal, because the specular term stays bright while the diffuse goes
dark. It is also a partial answer to reflection staleness under destruction — a
cubemap captured before a grenade is *geometrically* stale afterwards and this
does not fix that, but the far more common and worse-reading error of a
reflection being lit for the wrong side of a wall is exactly what it corrects.

### 7.6 The schedule, throughout

Not a fixed round-robin. **A dirty set with a priority queue**, fed by:

- **Geometry change** — the demolished cells, plus their shadow shaft.
  `SunBaker::collectShadowShaft` already computes the second set and the
  argument for why it is a line rather than a hemisphere (one sun) holds here
  for the direct term.
- **Sun movement** — everything, at a low background rate.
- **Never traced** — map load, at a high rate until the front settles.
- **A slow background sweep** regardless, so nothing is ever permanently wrong
  and a bug in the dirty logic is self-healing rather than a permanent dark
  room.

Cap the per-frame budget in probes, not in rays, and put the remaining dirty
count in the dev overlay — `DeviceProbeSet::staleFaceCount` exists for precisely
this reason and its header says why: *"a probe set that never reaches zero is a
scheduler bug, and it is otherwise completely invisible."*

---

## 8. Where things live — and the architectural rule bites here

**The tracer cannot be in cromwell.** `RayCaster`, `SunBaker`, `Lattice` and
`World` are all under `game/`, and CLAUDE.md's one architectural rule is that
cromwell may not include, link against or name anything under `game/`. A probe
tracer that walks the lattice is a game system, and `cmake --build . --target
cromwell` will say so.

That is not an obstacle, it is the right seam, and **`DeviceProbeSet` already
established it** in its opening lines:

> "WHICH rooms exist is the game's business; WHAT a probe means to a surface is
> the shader's; WHO renders a face is `ScenePipeline`'s — this hands out the
> matrices and the slice to draw into."

The same split, one level up:

| | Owner | Why |
|---|---|---|
| The 3D textures, their format, the upload, the shader include, the gather | **cromwell** — `DeviceIrradianceField`, beside `DeviceProbeSet` on `RenderScene` | It is a GPU resource and a sampling convention. Nothing in it names a lattice. |
| Field dimensions, cell size, world origin | **cromwell**, as data the game sets | A grid of probes over a box is genre-neutral. |
| Which cells are solid, which edges block | **The game**, uploaded as the edge-bit texture | The engine receives one byte per cell and never learns what a wall is. |
| Tracing — rays, visibility, hit shading, the schedule | **The game** — `game/light/IrradianceTracer`, beside `SunBaker` | It is a question about the world, answerable only by something that knows what the world is made of. |

**And this is the three-genre test passed rather than dodged.** An RTS or an FPS
on cromwell has no tile lattice, so it cannot use this tracer — and it does not
need to. It supplies a field of the same shape from whatever it can trace:
compute DDA over an SDF, a mesh tracer, an offline bake, or a hand-authored
gradient. The engine half is unchanged, because the engine half never knew.

**One housekeeping note, and it resolves itself.** `cromwell/lighting/` holds six
units today, which would make `DeviceIrradianceField` the seventh and put the
folder on the split threshold. Four of the six — `PbrShader`, `ProbeSpheres`,
`ReflectionProbeSet`, `ShadowMap` — are raylib types that leave with the raylib
path, so by the time this lands the folder is smaller than it is now and the
split question does not arise. **Do not pre-emptively subdivide it**; count the
units after the removal, not before.

### 8.1 What survives a large map, and what does not

**The engine's target is a large map and this board is not one.** 24 × 24 × 9 is
a prototype that exercises engine features; an RTS or an open third-person map is
the content cromwell is actually aimed at. Every number in §2 and §9 is a
prototype number, and it is worth being explicit about which of the *decisions*
they support are scale-free and which are not — because getting that wrong is how
a feature ships that has to be rewritten rather than extended.

**Scale-free — these are the design, and they hold at any map size:**

| | why it holds |
|---|---|
| **Four directional lobes, R11G11B10F** | A per-probe cost. 16 bytes is 16 bytes whether there are five thousand probes or five million, and every reason in §4 — fetch count, no ringing, non-negativity — is per-probe too. |
| **The eight-tap gather with weighted rejection** | Per-pixel and constant. It does not know how many probes exist. |
| **Irradiance only, no specular term** | Capcom's MHW simplification, and the reason the field composes with reflection probes instead of competing with them. |
| **Multi-bounce by previous-frame feedback** | Costs one gather regardless of field size. |
| **The engine/game seam in §8** | The point of it. The engine takes a field; who traced it and against what is not its business. |
| **The two occlusion bands in §2.2** | Technique resolution, not content. |

**Not scale-free — these are prototype choices and the engine must not bake them
in:**

| | what breaks, and the known answer |
|---|---|
| **One probe per cell, over the whole world** | 5184 probes is 83 KB; a large map is millions and hundreds of MB. The shipped answer everywhere is a **clipmap or cascade centred on the camera** — fine detail near, coarse far, scrolled rather than rebuilt. RE ENGINE ships exactly this shape for its own volumetric data (`GlobalSDFResolution`, `GlobalSDFClipmapNum` in RE4R's runtime, §2.5). **Build the field addressing so a cascade can be added without changing the sampler**: the shader should ask "give me the irradiance at this world point", never "index cell (x,y,z)". |
| **CPU tracing (§6)** | Does not survive. §6 chose CPU on a readback argument, and that argument only bites in the *mixed* configuration — a CPU tracer with a GPU-authoritative field. Both pure configurations are coherent: CPU tracer + CPU field (chosen here, because `RayCaster` exists and the world is 5184 cells), or **compute tracer + GPU field**, which has no readback either because both ends are on the GPU. At large-map scale the second is the only option. **So §6's decision is the game's, not the engine's, and §8's table already puts tracing on the game side — which is what makes this a substitution rather than a rewrite.** |
| **Three edge bits per cell for leak rejection (§5.1)** | The lattice's *implementation* of a general contract, not the contract. A genre with no discrete world has no edges to read. The engine-side contract is "a per-sample occlusion weight the field's supplier provides"; a lattice answers it with edge bits, a mesh world with per-probe visibility or DDGI's Chebyshev depth. **Do not let the edge-bit texture appear in an engine-side interface** — it is exactly the kind of game vocabulary that must not cross the seam. |
| **"A static world converges and then costs nothing" (§6)** | True of a bounded board with a fixed sun. A large map with streaming and a day-night cycle has a permanently moving front of dirty probes, which makes the §7.6 priority queue load-bearing rather than a convenience, and raises the value of the coefficient lerp in §4 from a nicety to the mechanism. |
| **Every cost in §9** | Prototype measurements on a prototype map. They say the technique is affordable here; they say nothing about an RTS map and should never be quoted as though they do. |

**The one-line version:** the storage format, the sampler and the seam are the
engine's and are scale-free; the tracer, the field's extent and the occlusion
hint are the game's and are expected to be replaced wholesale for a different
genre. If a large-map project can drop in a compute tracer and a clipmap without
touching `DeviceIrradianceField`'s interface, this note got the split right.

---

## 9. Costs

Estimates, and marked as such. The first task of P2 is to replace the CPU line
with a measurement.

| | |
|---|---|
| **Field memory** | 83 KB (5184 probes × 16 B), ×2 for the crossfade. Plus 5 KB of edge bits. |
| **Upload** | 83 KB per changed frame. Below noticing. |
| **CPU, full refresh** | 5184 probes × 64 rays = **332k rays**. At the shelved bake's measured ~43 Mray/s that is **~7.7 ms** for the entire world, once. GI rays shade what they hit where bake rays stopped, so read it as an optimistic floor. |
| **CPU, steady state** | **Zero.** Nothing dirty, nothing traced. |
| **CPU, after a demolition** | The blast cells plus their shadow shaft, spread over several frames. `bakeRegion` measured 36 ms at a real grenade radius for a *far* denser target (per-texel, not per-cell), so this should be small — but it is the number to watch, because it is the one that arrives during an explosion. |
| **GPU, sampling** | 32 cache-resident fetches per shaded pixel, replacing one arithmetic evaluation. Capcom's tetrahedral network cost **1.5 ms at 1080p**; ours skips their traversal entirely and adds a gather, so treat 1.5 ms as a ceiling to come in under, not a target. |
| **Profiler zones** | `probe trace` and `probe upload` on the CPU side, added in the same commit as the system per CLAUDE.md. **No new GPU zone** — the sampling happens inside `lit scene` and one system gets one row until a measurement earns a split. |

---

## 10. Rejected, and why

| | verdict |
|---|---|
| **Spherical harmonics, third order** | 7× the memory and 7 fetches against 4, plus ringing and a window function to tune. Capcom moved toward it; we are not carrying their probe density. §4. |
| **Hardware trilinear filtering** | Filters across walls by construction, which is the exact bug §5 exists to prevent. Four fetches instead of thirty-two is not worth the artefact both reference engines name as the killer. |
| **Chebyshev / per-probe depth (DDGI's leak rejection)** | Statistical where ours can be exact, and it costs a depth map per probe. §5.1. |
| **Tracing in compute** | The multi-bounce feedback needs the field readable where it is written, and a readback is the thing CLAUDE.md warns about. Revisit if a genre without a lattice arrives — §8's seam survives it. |
| **An ambient lightmap** | §3. Reproduces the sun bake's first named failure. |
| **Probes at cell corners rather than centres** | Makes the eight taps the eight corners of the cell you are in, which sounds like it localises the interpolation and does not — a corner is shared with the room next door either way. Cell centres match the lattice's own indexing and the dead-probe test is then "is this cell solid", which is one lookup. |
| **Hand-placed probe volumes** | The thing Capcom retreated *from*, and we have no reason to build what they abandoned. If a room ever needs an override, that is a `LightProbeBlocker`-shaped feature and it is not this note. |
| **Lumen-shaped realtime GI** | Most of Lumen is machinery for tracing arbitrary triangle meshes — a surface cache of material cards so a ray hit can be shaded without re-evaluating its material, per-mesh distance fields merged into a global one, and a screen-space radiance-probe final gather. On a lattice the first two collapse to nothing: the cells *are* the surface representation and `RayCaster` already traces them. What is left is the probes, which is this note. **We are not rejecting Lumen; we are building the part of it that does not evaporate.** Its multi-bounce trick — the cache feeding itself — is §7.4 verbatim. |
| **Hardware path tracing** | What RE ENGINE itself now ships for Requiem and PRAGMATA (GDC 2026), replacing everything §4.1 describes. Needs ray-tracing hardware and an API that exposes it; the RHI targets GL 4.3 and `raytracing.md` is a research note, not a plan. Revisit if a Vulkan backend lands with RT extensions — and note the seam in §8 survives it, because a path tracer supplying an irradiance field is still a field. |

**A note on the shape of those last two, because §2's framing invites the
objection.** §2 says *"we start where Capcom finished"*, meaning Monster Hunter:
World in 2017 — and Capcom did not finish there. **Both reference engines have
since left the approach this note adopts:** Epic ships Lumen, and Capcom's own
2025–26 titles path trace direct and indirect light in one pipeline. Read
honestly, this note is adopting a technique its two sources moved on from.

That is not the argument against it that it first appears to be, and the reason
is worth stating rather than assuming. **They moved on to solve problems this
project does not have** — open worlds, a moving sun, streamed instanced meshes,
arbitrary triangle geometry — and they moved on *to hardware we cannot reach*.
`re_engine_rendering.md` §11.0 makes the point in the other direction and it is
the same point: RE2R's architecture was excellent because it was aimed precisely
at bounded interiors with static geometry and no day/night cycle, and **every one
of those conditions describes this board**. The techniques worth taking are the
ones that exploit that, not the ones Capcom built after their content outgrew
them.

The honest residual: §8.1 already marks the tracer and the field extent as the
parts expected to be replaced wholesale for a different genre. **A Lumen-shaped
or path-traced future is that replacement**, and it lands on the game side of the
seam. If it ever arrives, `DeviceIrradianceField` should not notice.

---

## 11. Open questions

- **Does the field gather from local lights?** The largest known limitation, and
  it is a sequencing problem rather than a hard one. By the time this lands the
  light list is a GPU structure that clustered forward owns, and a CPU tracer
  would need its own copy. Until then a muzzle flash lights what it directly
  hits and bounces off nothing. **The cost of closing it is one CPU-side mirror
  of the light list plus a loop in the hit shading** — worth scoping before
  clustered forward finalises where the light list lives, because a
  GPU-only-by-design list makes this expensive later and cheap now.
- **How many rays per probe, and does it need a low-discrepancy sequence?** 64 is
  DDGI's number for probes that re-trace every frame under temporal
  accumulation. A field that converges and stops may want more per probe and
  fewer probes per frame — the opposite trade. Measure before choosing.
- **Does the sun move in this game?** The whole schedule turns on it. A fixed sun
  makes the field a one-time cost after load; a day-night cycle makes it a
  permanent background load and raises the value of the coefficient lerp in §4.
  Nobody has decided, and it is a design question, not a rendering one.
- **What does a unit sample?** One point at the chest is one lookup and will pop
  as a unit crosses a cell boundary. Two or three points blended along the body
  is the usual fix and costs what it costs. Not urgent until units read the
  field at all, but it decides whether the sampler needs a "sample the field at
  an arbitrary point" entry point separate from the fragment path.
- **Is 0.42 deletable?** `uExposureAndAmbient.y` exists because the two-lobe sky
  needs a fudge to land at a plausible irradiance. A traced field *is* an
  irradiance and should need no such factor. If it still does after P3, that is
  evidence of an energy bug in the tracer and should be chased rather than
  tuned around.
- **Does this interact with the shelved sun bake if that ever revives?** Both
  read the same `RayCaster` over the same lattice for related questions. If the
  ambient bake comes back for static faces as a resolution boost (§3, "both"),
  the two must agree at the boundary or the seam is the artefact — which is the
  sun bake's original failure a third time.
